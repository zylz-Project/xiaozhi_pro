#include "dinosaur_audio.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <esp_log.h>
#include <esp_random.h>

#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "config.h"
#include "dinosaur_controller.h"
#include "ogg_demuxer.h"

#define TAG "DinosaurAudio"

namespace {
// PNDA 按小端序保存为 0x41444E50，即 Flash 中依次为字符 P、N、D、A。
constexpr uint32_t kTocMagic = 0x41444E50;  // "PNDA"
constexpr size_t kReadChunk = 4096;

bool Contains(const char* text, const char* token) {
    return text != nullptr && std::strstr(text, token) != nullptr;
}
}  // namespace

bool DinosaurAudio::Initialize() {
    // 本模块只添加一个 SPI 只读设备，不执行擦除、写入或联网同步。
    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = DINO_FLASH_MOSI_PIN;
    bus_config.miso_io_num = DINO_FLASH_MISO_PIN;
    bus_config.sclk_io_num = DINO_FLASH_CLK_PIN;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = kReadChunk;
    esp_err_t err = spi_bus_initialize(DINO_FLASH_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "External audio flash bus unavailable: %s", esp_err_to_name(err));
        return false;
    }

    spi_device_interface_config_t device_config = {};
    device_config.command_bits = 8;
    device_config.address_bits = 24;
    device_config.mode = 0;
    device_config.clock_speed_hz = 20 * 1000 * 1000;
    device_config.spics_io_num = DINO_FLASH_CS_PIN;
    device_config.queue_size = 2;
    device_config.flags = SPI_DEVICE_HALFDUPLEX;
    err = spi_bus_add_device(DINO_FLASH_HOST, &device_config, &flash_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "External audio flash unavailable: %s", esp_err_to_name(err));
        return false;
    }
    if (!ReadToc()) {
        ESP_LOGW(TAG, "No valid default-audio TOC; offline movement will remain available");
        return false;
    }

    // SPI 读取和 OGG 解复用可能持续数百毫秒，必须放到独立任务；20 KiB
    // 栈主要供 OggDemuxer 的包缓存和 4 KiB Flash 读取缓冲区使用。
    play_queue_ = xQueueCreate(3, sizeof(int));
    if (play_queue_ == nullptr ||
        xTaskCreate(AudioTaskEntry, "dino_audio", 20480, this, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start offline audio task");
        return false;
    }
    ESP_LOGI(TAG, "Loaded %d provisioned sounds; network audio sync is disabled", file_count_);
    return true;
}

bool DinosaurAudio::Read(uint32_t address, void* data, size_t size) {
    // 先做无符号边界检查，防止 address + size 溢出后越过 Flash 末尾。
    if (flash_ == nullptr || data == nullptr || size == 0 || address >= DINO_FLASH_SIZE ||
        size > DINO_FLASH_SIZE - address) {
        return false;
    }
    spi_transaction_t transaction = {};
    transaction.cmd = 0x03;
    transaction.addr = address;
    transaction.rxlength = size * 8;
    transaction.rx_buffer = data;
    return spi_device_transmit(flash_, &transaction) == ESP_OK;
}

bool DinosaurAudio::ReadToc() {
    // TOC 固定放在第一个 4 KiB 扇区：12 字节头部后紧跟定长文件项。
    uint8_t toc[kReadChunk];
    if (!Read(0, toc, sizeof(toc))) {
        return false;
    }
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t count = 0;
    std::memcpy(&magic, toc, sizeof(magic));
    std::memcpy(&version, toc + 4, sizeof(version));
    std::memcpy(&count, toc + 8, sizeof(count));
    if (magic != kTocMagic || (version != 1 && version != 2) || count > kMaxFiles) {
        return false;
    }

    // v1 文件项为 80 字节；v2 在末尾增加了 16 字节 category。
    const size_t entry_size = version >= 2 ? kEntrySize : 80;
    if (12 + count * entry_size > sizeof(toc)) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* entry = toc + 12 + i * entry_size;
        auto& file = files_[i];
        std::memcpy(file.name, entry, sizeof(file.name));
        file.name[sizeof(file.name) - 1] = '\0';
        std::memcpy(&file.offset, entry + 64, sizeof(file.offset));
        std::memcpy(&file.size, entry + 68, sizeof(file.size));
        std::memcpy(&file.sample_rate, entry + 72, sizeof(file.sample_rate));
        std::memcpy(&file.duration_ms, entry + 76, sizeof(file.duration_ms));
        if (version >= 2) {
            std::memcpy(file.category, entry + 80, sizeof(file.category));
            file.category[sizeof(file.category) - 1] = '\0';
        } else {
            // 旧 TOC 没有分类字段，按普通动物声音参与随机选择。
            std::strcpy(file.category, "animal");
        }
        const uint32_t address = DINO_FLASH_DATA_START + file.offset;
        if (file.name[0] == '\0' || file.size == 0 || address < DINO_FLASH_DATA_START ||
            address >= DINO_FLASH_SIZE || file.size > DINO_FLASH_SIZE - address) {
            ESP_LOGW(TAG, "Ignoring invalid TOC entry %lu", static_cast<unsigned long>(i));
            return false;
        }
    }
    file_count_ = static_cast<int>(count);
    return file_count_ > 0;
}

int DinosaurAudio::SelectFile(DinoAction action) const {
    int selected = -1;
    int matches = 0;
    // 使用蓄水池采样在所有匹配声音中等概率选择，无需额外动态数组。
    for (int i = 0; i < file_count_; ++i) {
        const auto& file = files_[i];
        if (std::strcmp(file.category, "ambient") == 0) {
            continue;
        }
        bool match = false;
        switch (action) {
            case DinoAction::kEat:
                match = Contains(file.name, "咀嚼") || Contains(file.name, "eat") ||
                        Contains(file.name, "chew");
                break;
            case DinoAction::kListen:
                match = Contains(file.name, "脚步") || Contains(file.name, "step") ||
                        Contains(file.name, "foot");
                break;
            case DinoAction::kSleepy:
                match = Contains(file.name, "入睡") || Contains(file.name, "sleep");
                break;
            case DinoAction::kStartled:
                match = Contains(file.name, "警觉") || Contains(file.name, "startle");
                break;
            case DinoAction::kHappy:
                match = Contains(file.name, "雀跃") || Contains(file.name, "happy") ||
                        Contains(file.name, "joy");
                break;
            case DinoAction::kAffection:
                match = Contains(file.name, "亲近") || Contains(file.name, "comfort") ||
                        Contains(file.name, "nuzzle");
                break;
            case DinoAction::kDiscover:
                match = file.duration_ms > 0 && file.duration_ms <= 1500;
                break;
            case DinoAction::kProud:
                match = Contains(file.name, "叫声") || Contains(file.name, "call") ||
                        Contains(file.name, "roar");
                break;
        }
        if (match && (esp_random() % ++matches) == 0) {
            selected = i;
        }
    }
    if (selected >= 0) {
        return selected;
    }
    // 没有动作专属声音时，从非 ambient 文件中随机回退一个，保证兼容旧音频包。
    for (int i = 0; i < file_count_; ++i) {
        if (std::strcmp(files_[i].category, "ambient") != 0 && (esp_random() % ++matches) == 0) {
            selected = i;
        }
    }
    return selected;
}

bool DinosaurAudio::PlayForAction(DinoAction action) {
    if (play_queue_ == nullptr) {
        return false;
    }
    // XiaoZhi 的提示音或上一段声音尚未结束时不插入恐龙音效，避免混音。
    if (!Application::GetInstance().GetAudioService().IsPlaybackIdle()) {
        return false;
    }
    const int index = SelectFile(action);
    // 队列已满就放弃本次声音，不允许动作任务在这里等待。
    return index >= 0 && xQueueSend(play_queue_, &index, 0) == pdTRUE;
}

void DinosaurAudio::StopPlayback() {
    playback_generation_.fetch_add(1);
    if (play_queue_ != nullptr) {
        int discarded = -1;
        while (xQueueReceive(play_queue_, &discarded, 0) == pdTRUE) {
        }
    }

    // 本地声音和在线 TTS 共用 AudioService。进入 connecting 时服务端还
    // 没有下发 TTS，因此此处清理解码器只会移除尚未播完的本地声音。
    Application::GetInstance().GetAudioService().ResetDecoder();
}

void DinosaurAudio::AudioTaskEntry(void* arg) { static_cast<DinosaurAudio*>(arg)->AudioTask(); }

void DinosaurAudio::AudioTask() {
    int index = -1;
    while (true) {
        if (xQueueReceive(play_queue_, &index, portMAX_DELAY) == pdTRUE) {
            PlayFile(index);
        }
    }
}

bool DinosaurAudio::PlayFile(int index) {
    if (index < 0 || index >= file_count_) {
        return false;
    }
    auto& app = Application::GetInstance();
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    const uint32_t generation = playback_generation_.load();

    // OggDemuxer 只拆出 Opus 包；真正的解码和 I2S 输出仍由 AudioService 完成。
    bool ok = true;
    OggDemuxer demuxer;
    demuxer.OnDemuxerFinished(
        [this, &app, &ok, generation](const uint8_t* data, int sample_rate, size_t size) {
            if (playback_generation_.load() != generation) {
                ok = false;
                return;
            }
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = 60;
            packet->payload.assign(data, data + size);
            if (!app.GetAudioService().PushPacketToDecodeQueue(std::move(packet), true)) {
                ok = false;
            }
        });

    uint8_t buffer[kReadChunk];
    const auto& file = files_[index];
    ESP_LOGI(TAG, "Playing offline sound: %s", file.name);
    // 分块顺序读取，避免为完整音频文件申请大块 RAM。
    for (uint32_t offset = 0;
         ok && playback_generation_.load() == generation && offset < file.size;) {
        const size_t chunk = std::min<size_t>(sizeof(buffer), file.size - offset);
        if (!Read(DINO_FLASH_DATA_START + file.offset + offset, buffer, chunk)) {
            ok = false;
            break;
        }
        demuxer.Process(buffer, chunk);
        offset += chunk;
    }
    return ok;
}
