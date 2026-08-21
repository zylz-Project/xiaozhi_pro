#include "panda_audio.h"

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
#include "ogg_demuxer.h"
#include "panda_controller.h"
#include "w25n01gv.h"

#define TAG "PandaAudio"

namespace {
// PNDA 按小端序保存为 0x41444E50，即 Flash 中依次为字符 P、N、D、A。
constexpr uint32_t kTocMagic = 0x41444E50;  // "PNDA"
constexpr size_t kReadChunk = 4096;
// 估算响度用的参考包大小：48k 单声道 60ms 的典型语音包约 150~250 字节。
constexpr float kLoudnessRefBytes = 220.0f;

bool Contains(const char* text, const char* token) {
    return text != nullptr && std::strstr(text, token) != nullptr;
}

void UpdateEnvelope(float raw, float& fast_env, float& slow_env, std::atomic<float>& level,
                    std::atomic<float>& attack) {
    raw = std::clamp(raw, 0.0f, 1.0f);
    fast_env += (raw - fast_env) * (raw > fast_env ? 0.48f : 0.12f);
    slow_env += (raw - slow_env) * 0.035f;
    float onset = (fast_env - slow_env * 1.08f) * 3.2f;
    onset = std::clamp(onset, 0.0f, 1.0f);
    level.store(slow_env);
    attack.store(onset);
}
}  // namespace

bool PandaAudio::Initialize() {
    // 本模块只读 NAND 闪存。w25n01gv 驱动负责 SPI 总线/设备初始化，这里
    // 只校验器件容量并读取 TOC，不执行擦除、写入或联网同步。
    const esp_err_t err = w25n01gv_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "W25N01GV init failed: %s", esp_err_to_name(err));
        return false;
    }
    // w25n01gv_init 已通过 JEDEC ID 校验芯片；get_capacity 返回的是用户区
    // 容量（125 MiB，物理 128 MiB 扣 BBM 保留块），不能与物理容量比较。
    ESP_LOGI(TAG, "External NAND capacity: %llu MiB",
             static_cast<unsigned long long>(w25n01gv_get_capacity() / 1024 / 1024));
    if (!ReadToc()) {
        ESP_LOGW(TAG, "No valid default-audio TOC; offline movement will remain available");
        return false;
    }

    play_queue_ = xQueueCreate(3, sizeof(int));
    if (play_queue_ == nullptr ||
        xTaskCreate(AudioTaskEntry, "panda_audio", 20480, this, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start offline audio task");
        return false;
    }
    ESP_LOGI(TAG, "Loaded %d provisioned sounds; network audio sync is disabled", file_count_);
    return true;
}

bool PandaAudio::Read(uint32_t address, void* data, size_t size) {
    if (data == nullptr || size == 0 || address >= PANDA_FLASH_SIZE ||
        size > PANDA_FLASH_SIZE - address) {
        return false;
    }
    return w25n01gv_read(address, static_cast<uint8_t*>(data), size) == ESP_OK;
}

bool PandaAudio::ReadToc() {
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

    const size_t entry_size = version >= 2 ? kEntrySize : 80;
    if (12 + count * entry_size > sizeof(toc)) {
        return false;
    }
    // 与旧工程 flash_audio_init 一致：逐个校验条目，遇到损坏条目只跳过，
    // 而不是让整颗 flash 的音频全部失效。
    uint32_t valid = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* entry = toc + 12 + i * entry_size;
        auto& file = files_[valid];
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
            std::strcpy(file.category, "animal");
        }
        const uint32_t address = PANDA_FLASH_DATA_START + file.offset;
        if (file.name[0] == '\0' || file.size == 0 || address < PANDA_FLASH_DATA_START ||
            address >= PANDA_FLASH_SIZE || file.size > PANDA_FLASH_SIZE - address) {
            ESP_LOGW(TAG, "Skipping invalid TOC entry %lu", static_cast<unsigned long>(i));
            continue;
        }
        ++valid;
    }
    file_count_ = static_cast<int>(valid);
    if (file_count_ > 0) {
        ESP_LOGI(TAG, "TOC loaded: %d valid of %lu entries", file_count_,
                 static_cast<unsigned long>(count));
    }
    return file_count_ > 0;
}

int PandaAudio::SelectFile(PandaAction action) const {
    int selected = -1;
    int matches = 0;
    // 蓄水池采样在所有匹配声音中等概率选择，无需额外动态数组。
    for (int i = 0; i < file_count_; ++i) {
        const auto& file = files_[i];
        if (std::strcmp(file.category, "ambient") == 0) {
            continue;
        }
        bool match = false;
        switch (action) {
            case PandaAction::kEat:
                match = Contains(file.name, "吃竹") || Contains(file.name, "咀嚼") ||
                        Contains(file.name, "eat") || Contains(file.name, "chew");
                break;
            case PandaAction::kListen:
                match = Contains(file.name, "虫鸣") || Contains(file.name, "鸟叫") ||
                        Contains(file.name, "下雨") || Contains(file.name, "流水");
                break;
            case PandaAction::kSleepy:
                match = Contains(file.name, "入睡") || Contains(file.name, "sleep") ||
                        Contains(file.name, "撒娇");
                break;
            case PandaAction::kStartled:
                match = Contains(file.name, "叫声") || Contains(file.name, "call") ||
                        Contains(file.name, "roar");
                break;
            case PandaAction::kHappy:
                match = Contains(file.name, "嘤嘤") || Contains(file.name, "宝宝") ||
                        Contains(file.name, "happy") || Contains(file.name, "joy");
                break;
            case PandaAction::kAffection:
                match = Contains(file.name, "撒娇") || Contains(file.name, "亲近") ||
                        Contains(file.name, "nuzzle") || Contains(file.name, "comfort");
                break;
            case PandaAction::kDiscover:
                match = file.duration_ms > 0 && file.duration_ms <= 1500;
                break;
            case PandaAction::kProud:
                match = Contains(file.name, "成年") || Contains(file.name, "威风") ||
                        Contains(file.name, "叫声") || Contains(file.name, "call") ||
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
    for (int i = 0; i < file_count_; ++i) {
        if (std::strcmp(files_[i].category, "ambient") != 0 && (esp_random() % ++matches) == 0) {
            selected = i;
        }
    }
    return selected;
}

int PandaAudio::RandomIndexInCategory(const char* category) const {
    int selected = -1;
    int matches = 0;
    for (int i = 0; i < file_count_; ++i) {
        if (std::strcmp(files_[i].category, category) == 0 && (esp_random() % ++matches) == 0) {
            selected = i;
        }
    }
    return selected;
}

bool PandaAudio::PlayForAction(PandaAction action) {
    if (play_queue_ == nullptr) {
        return false;
    }
    if (!Application::GetInstance().GetAudioService().IsPlaybackIdle()) {
        return false;
    }
    const int index = SelectFile(action);
    return index >= 0 && xQueueSend(play_queue_, &index, 0) == pdTRUE;
}

bool PandaAudio::PlayRandomSound(const char* category) {
    if (play_queue_ == nullptr) {
        return false;
    }
    if (!Application::GetInstance().GetAudioService().IsPlaybackIdle()) {
        return false;
    }
    const int index = RandomIndexInCategory(category);
    return index >= 0 && xQueueSend(play_queue_, &index, 0) == pdTRUE;
}

void PandaAudio::StopPlayback() {
    playback_generation_.fetch_add(1);
    if (play_queue_ != nullptr) {
        int discarded = -1;
        while (xQueueReceive(play_queue_, &discarded, 0) == pdTRUE) {
        }
    }
    is_playing_.store(false);
    motion_level_.store(0.0f);
    motion_attack_.store(0.0f);
    // 清掉输出包络残留：本地声音的输出也会喂 tts_level_，进入对话时若不
    // 清零，speaking 状态会带着上一段本地声音的包络立刻"舞动"。
    tts_level_.store(0.0f);
    tts_attack_.store(0.0f);
    Application::GetInstance().GetAudioService().ResetDecoder();
}

PandaMotionData PandaAudio::GetMotionData() const {
    PandaMotionData data = {};
    data.playing = is_playing_.load();
    data.level = motion_level_.load();
    data.attack = motion_attack_.load();
    data.elapsed_ms = motion_elapsed_ms_.load();
    data.duration_ms = motion_duration_ms_.load();
    data.sound_index = motion_sound_index_.load();
    return data;
}

void PandaAudio::FeedTtsLevel(float level, float attack) {
    tts_level_.store(level);
    tts_attack_.store(attack);
}

PandaMotionData PandaAudio::GetTtsMotionData() const {
    PandaMotionData data = {};
    data.playing = tts_level_.load() > 0.01f;
    data.level = tts_level_.load();
    data.attack = tts_attack_.load();
    data.sound_index = -1;
    return data;
}

int PandaAudio::SoundCountByCategory(const char* category) const {
    int count = 0;
    for (int i = 0; i < file_count_; ++i) {
        if (std::strcmp(files_[i].category, category) == 0) {
            ++count;
        }
    }
    return count;
}

const char* PandaAudio::SoundName(int index) const {
    if (index < 0 || index >= file_count_) {
        return "???";
    }
    return files_[index].name;
}

int PandaAudio::SoundDurationMs(int index) const {
    if (index < 0 || index >= file_count_) {
        return 0;
    }
    return static_cast<int>(files_[index].duration_ms);
}

const char* PandaAudio::SoundCategory(int index) const {
    if (index < 0 || index >= file_count_) {
        return "???";
    }
    return files_[index].category;
}

void PandaAudio::AudioTaskEntry(void* arg) { static_cast<PandaAudio*>(arg)->AudioTask(); }

void PandaAudio::AudioTask() {
    int index = -1;
    while (true) {
        if (xQueueReceive(play_queue_, &index, portMAX_DELAY) == pdTRUE) {
            PlayFile(index);
        }
    }
}

bool PandaAudio::PlayFile(int index) {
    if (index < 0 || index >= file_count_) {
        return false;
    }
    auto& app = Application::GetInstance();
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    const uint32_t generation = playback_generation_.load();
    const auto& file = files_[index];
    motion_sound_index_.store(index);
    motion_elapsed_ms_.store(0);
    motion_duration_ms_.store(file.duration_ms);
    motion_level_.store(0.0f);
    motion_attack_.store(0.0f);
    fast_envelope_ = 0.0f;
    slow_envelope_ = 0.0f;
    is_playing_.store(true);

    // OggDemuxer 只拆出 Opus 包；真正的解码和 I2S 输出仍由 AudioService 完成。
    bool ok = true;
    OggDemuxer demuxer;
    demuxer.OnDemuxerFinished([this, &app, &ok, generation](const uint8_t* data, int sample_rate,
                                                            size_t size) {
        if (playback_generation_.load() != generation) {
            ok = false;
            return;
        }
        // 用 Opus 包大小估算响度：语音包越大越“响”，与真实 PCM 包络趋势一致。
        if (sample_rate > 0) {
            const uint32_t frame_ms = 60;
            const float kbps =
                static_cast<float>(size) * 8.0f * 1000.0f / static_cast<float>(frame_ms);
            const float raw = kbps / 24000.0f;
            UpdateEnvelope(raw, fast_envelope_, slow_envelope_, motion_level_, motion_attack_);
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
    ESP_LOGI(TAG, "Playing offline sound: %s", file.name);
    for (uint32_t offset = 0;
         ok && playback_generation_.load() == generation && offset < file.size;) {
        const size_t chunk = std::min<size_t>(sizeof(buffer), file.size - offset);
        if (!Read(PANDA_FLASH_DATA_START + file.offset + offset, buffer, chunk)) {
            ok = false;
            break;
        }
        demuxer.Process(buffer, chunk);
        offset += chunk;
        motion_elapsed_ms_.store(
            static_cast<uint32_t>(offset * file.duration_ms / std::max<uint32_t>(1, file.size)));
    }
    is_playing_.store(false);
    motion_level_.store(0.0f);
    motion_attack_.store(0.0f);
    return ok;
}