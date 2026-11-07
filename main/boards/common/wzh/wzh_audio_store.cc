#include "wzh_audio_store.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "WzhAudioStore"

namespace wzh {
namespace {

constexpr uint32_t kLegacyMagic = 0x41444E50;  // "PNDA"（小端）
constexpr size_t kLegacyHeaderSize = 12;
constexpr size_t kReadChunk = 4096;

// 容器标记校验：OGG 页 + OpusHead 头。
bool LooksLikeOggOpus(const uint8_t* data, size_t size) {
    if (size < 4 || std::memcmp(data, "OggS", 4) != 0) {
        return false;
    }
    const size_t scan = std::min<size_t>(size, 64);
    for (size_t i = 0; i + 8 <= scan; ++i) {
        if (std::memcmp(data + i, "OpusHead", 8) == 0) {
            return true;
        }
    }
    return false;
}

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

void CopyEntry(AudioFileInfo& file, const uint8_t* entry, bool has_category) {
    std::memcpy(file.name, entry, sizeof(file.name));
    file.name[sizeof(file.name) - 1] = '\0';
    std::memcpy(&file.offset, entry + 64, sizeof(file.offset));
    std::memcpy(&file.size, entry + 68, sizeof(file.size));
    std::memcpy(&file.sample_rate, entry + 72, sizeof(file.sample_rate));
    std::memcpy(&file.duration_ms, entry + 76, sizeof(file.duration_ms));
    if (has_category) {
        std::memcpy(file.category, entry + 80, sizeof(file.category));
        file.category[sizeof(file.category) - 1] = '\0';
    } else {
        // 老 TOC 没有分类字段，按普通动物声音参与随机选择。
        std::strcpy(file.category, "animal");
    }
}

bool EntryValid(const AudioFileInfo& file, uint32_t data_start, uint64_t capacity) {
    const uint64_t address = static_cast<uint64_t>(data_start) + file.offset;
    return file.name[0] != '\0' && file.size > 0 && file.offset != 0xFFFFFFFF &&
           address < capacity && file.size <= capacity - address;
}

void SerializeEntry(uint8_t* out, const AudioFileInfo& file) {
    std::memset(out, 0, sizeof(AudioFileInfo));  // 96 字节，与 TOC 条目一致
    std::memcpy(out, file.name, strnlen(file.name, sizeof(file.name)));
    std::memcpy(out + 64, &file.offset, sizeof(file.offset));
    std::memcpy(out + 68, &file.size, sizeof(file.size));
    std::memcpy(out + 72, &file.sample_rate, sizeof(file.sample_rate));
    std::memcpy(out + 76, &file.duration_ms, sizeof(file.duration_ms));
    std::memcpy(out + 80, file.category, strnlen(file.category, sizeof(file.category)));
}

struct WritingGuard {
    std::atomic<bool>& flag;
    ~WritingGuard() { flag.store(false); }
};

struct AsyncEraseArgs {
    AudioStore* self;
    std::function<void(bool)> on_done;
};

}  // namespace

bool AudioStore::Init() {
    const ProductConfig& product = Product();
    wzh_flash_pins_t pins = {};
    pins.host = product.flash_host;
    pins.cs_pin = product.flash_cs_pin;
    pins.clk_pin = product.flash_clk_pin;
    pins.mosi_pin = product.flash_mosi_pin;
    pins.miso_pin = product.flash_miso_pin;

    const esp_err_t err = wzh_flash_init(&pins);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "外部音频 Flash 初始化失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "外部音频 Flash: %s, JEDEC %02X %02X %02X", wzh_flash_type_name(),
             wzh_flash_jedec_id()[0], wzh_flash_jedec_id()[1], wzh_flash_jedec_id()[2]);

    capacity_ = wzh_flash_capacity();
    erase_size_ = wzh_flash_erase_size();
    program_size_ = wzh_flash_program_size();
    if (wzh_flash_type() == WZH_FLASH_NAND_W25N01GV) {
        // TOC 独占 block0（32 个 4KB 槽），数据从 block1 开始——与老工程
        // 布局一致，现场已烧录默认声音的设备无需迁移。
        toc_slot_count_ = erase_size_ / kTocSlotSize;
        data_start_ = erase_size_;
    } else {
        // NOR 只有 1 个 TOC 槽（sector0），数据从 4096 开始，同样兼容老布局。
        toc_slot_count_ = 1;
        data_start_ = kTocSlotSize;
    }
    files_.reserve(kMaxFiles);
    return LoadToc();
}

bool AudioStore::ReadFlash(uint32_t addr, void* data, size_t size) {
    return wzh_flash_read(addr, data, size) == ESP_OK;
}

bool AudioStore::TryParseV3(const uint8_t* slot, uint32_t* seq,
                            std::vector<AudioFileInfo>* out) const {
    TocHeaderV3 header = {};
    std::memcpy(&header, slot, sizeof(header));
    if (header.magic != kTocV3Magic || header.version != 3 || header.count > kMaxFiles) {
        return false;
    }
    if (kTocV3HeaderSize + header.count * kEntrySize > kTocSlotSize) {
        return false;
    }
    uint32_t crc = esp_rom_crc32_le(0, slot + kTocV3HeaderSize, header.count * kEntrySize);
    crc = esp_rom_crc32_le(crc, reinterpret_cast<const uint8_t*>(&header.seq), sizeof(header.seq));
    if (crc != header.crc) {
        return false;
    }
    out->clear();
    // count==0 是合法的空目录（最后一个文件删除后的状态）。
    for (uint32_t i = 0; i < header.count; ++i) {
        AudioFileInfo file = {};
        CopyEntry(file, slot + kTocV3HeaderSize + i * kEntrySize, true);
        if (!EntryValid(file, data_start_, capacity_)) {
            ESP_LOGW(TAG, "v3 目录条目 %lu 无效，跳过", static_cast<unsigned long>(i));
            continue;
        }
        out->push_back(file);
    }
    *seq = header.seq;
    return true;
}

bool AudioStore::TryParseLegacy(const uint8_t* slot, std::vector<AudioFileInfo>* out) const {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t count = 0;
    std::memcpy(&magic, slot, sizeof(magic));
    std::memcpy(&version, slot + 4, sizeof(version));
    std::memcpy(&count, slot + 8, sizeof(count));
    if (magic != kLegacyMagic || (version != 1 && version != 2) || count > kMaxFiles) {
        return false;
    }
    // v1 条目 80 字节；v2 末尾追加 16 字节 category，共 96 字节。
    const size_t entry_size = version >= 2 ? kEntrySize : 80;
    if (kLegacyHeaderSize + count * entry_size > kTocSlotSize) {
        return false;
    }
    out->clear();
    uint32_t valid = 0;
    for (uint32_t i = 0; i < count; ++i) {
        AudioFileInfo file = {};
        CopyEntry(file, slot + kLegacyHeaderSize + i * entry_size, version >= 2);
        // 与老工程一致：单个损坏条目只跳过，不让整颗芯片的音频失效。
        if (!EntryValid(file, data_start_, capacity_)) {
            ESP_LOGW(TAG, "旧目录条目 %lu 无效，跳过", static_cast<unsigned long>(i));
            continue;
        }
        ++valid;
        out->push_back(file);
    }
    return valid > 0;
}

bool AudioStore::LoadToc() {
    std::lock_guard<std::mutex> lock(mutex_);
    files_.clear();

    std::vector<uint8_t> slot(kTocSlotSize);
    // 1) 扫描全部 v3 槽，取最大 seq。
    int32_t best_slot = -1;
    uint32_t best_seq = 0;
    std::vector<AudioFileInfo> best;
    for (uint32_t i = 0; i < toc_slot_count_; ++i) {
        if (!ReadFlash(i * kTocSlotSize, slot.data(), slot.size())) {
            break;
        }
        std::vector<AudioFileInfo> parsed;
        uint32_t seq = 0;
        if (TryParseV3(slot.data(), &seq, &parsed) && seq > best_seq) {
            best_seq = seq;
            best_slot = static_cast<int32_t>(i);
            best = std::move(parsed);
        }
    }
    if (best_slot >= 0) {
        files_ = std::move(best);
        layout_ = Layout::kV3;
        current_slot_ = best_slot;
        toc_seq_ = best_seq;
        ESP_LOGI(TAG, "TOC v3: %u 个声音（槽 %ld, seq %lu）", static_cast<unsigned>(files_.size()),
                 static_cast<long>(best_slot), static_cast<unsigned long>(best_seq));
        return true;
    }

    // 2) 回退读老工程 v1/v2（固定第 0 槽）。
    if (!ReadFlash(0, slot.data(), slot.size())) {
        return false;
    }
    std::vector<AudioFileInfo> legacy;
    if (TryParseLegacy(slot.data(), &legacy)) {
        uint32_t version = 0;
        std::memcpy(&version, slot.data() + 4, sizeof(version));
        files_ = std::move(legacy);
        layout_ = Layout::kLegacy;
        current_slot_ = -1;
        ESP_LOGI(TAG, "TOC v%lu: %u 个声音（老工程格式）", static_cast<unsigned long>(version),
                 static_cast<unsigned>(files_.size()));
        return true;
    }
    layout_ = Layout::kEmpty;
    ESP_LOGW(TAG, "未发现有效音频目录（出厂状态），离线动作仍可用");
    return false;
}

bool AudioStore::SlotIsVirgin(uint32_t slot_index) {
    uint8_t header[kTocV3HeaderSize];
    if (!ReadFlash(slot_index * kTocSlotSize, header, sizeof(header))) {
        return false;
    }
    for (size_t i = 0; i < sizeof(header); ++i) {
        if (header[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

bool AudioStore::ProgramWithVerify(uint32_t addr, const uint8_t* data, size_t size) {
    std::vector<uint8_t> readback(kReadChunk);
    size_t done = 0;
    while (done < size) {
        const size_t chunk = std::min<size_t>(kReadChunk, size - done);
        if (wzh_flash_write(addr + done, data + done, chunk) != ESP_OK) {
            return false;
        }
        if (!ReadFlash(addr + done, readback.data(), chunk) ||
            std::memcmp(readback.data(), data + done, chunk) != 0) {
            ESP_LOGE(TAG, "写入校验失败 @ 0x%08lX", static_cast<unsigned long>(addr + done));
            return false;
        }
        done += chunk;
    }
    return true;
}

bool AudioStore::CommitToc(const std::vector<AudioFileInfo>& entries) {
    if (entries.size() > static_cast<size_t>(kMaxFiles)) {
        return false;
    }
    std::vector<uint8_t> image(kTocSlotSize, 0xFF);
    TocHeaderV3 header = {};
    header.magic = kTocV3Magic;
    header.version = 3;
    header.seq = ++toc_seq_;
    header.count = static_cast<uint32_t>(entries.size());
    for (uint32_t i = 0; i < header.count; ++i) {
        SerializeEntry(image.data() + kTocV3HeaderSize + i * kEntrySize, entries[i]);
    }
    header.crc = esp_rom_crc32_le(0, image.data() + kTocV3HeaderSize, header.count * kEntrySize);
    header.crc = esp_rom_crc32_le(header.crc, reinterpret_cast<const uint8_t*>(&header.seq),
                                  sizeof(header.seq));
    std::memcpy(image.data(), &header, sizeof(header));

    // 选择目标槽：v3 滚动到下一槽；legacy/空目录从 1 号槽开始（NOR 单槽
    // 只能原地擦写）。所有槽用尽后压实：擦除 TOC 区并从 0 号槽重新开始。
    uint32_t target = 0;
    if (layout_ == Layout::kV3 && current_slot_ >= 0) {
        target = (current_slot_ + 1) % toc_slot_count_;
    } else if (toc_slot_count_ > 1) {
        target = 1;
    }
    if (!SlotIsVirgin(target)) {
        bool found_virgin = false;
        for (uint32_t i = 0; i < toc_slot_count_; ++i) {
            if (SlotIsVirgin(i)) {
                target = i;
                found_virgin = true;
                break;
            }
        }
        if (!found_virgin) {
            ESP_LOGW(TAG, "TOC 槽已用尽，压实后重写（压实瞬间断电需重新同步音频）");
            if (wzh_flash_erase_unit(0) != ESP_OK) {
                return false;
            }
            target = 0;
        }
    }
    if (!ProgramWithVerify(target * kTocSlotSize, image.data(), image.size())) {
        ESP_LOGE(TAG, "TOC 写入校验失败，保留旧目录");
        --toc_seq_;
        return false;
    }
    // files_ 的底层存储保持不变（reserve 过），条目被原位替换，因此
    // 其他任务持有的 name 指针不会悬空。
    files_ = entries;
    layout_ = Layout::kV3;
    current_slot_ = static_cast<int32_t>(target);
    ESP_LOGI(TAG, "TOC 提交: %u 个声音（槽 %lu, seq %lu）", static_cast<unsigned>(files_.size()),
             static_cast<unsigned long>(target), static_cast<unsigned long>(header.seq));
    return true;
}

bool AudioStore::GetFile(int index, AudioFileInfo* out) const {
    if (out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= static_cast<int>(files_.size())) {
        return false;
    }
    *out = files_[index];
    return true;
}

std::vector<AudioFileInfo> AudioStore::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return files_;
}

const AudioFileInfo* AudioStore::PeekFile(int index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= static_cast<int>(files_.size())) {
        return nullptr;
    }
    return &files_[index];
}

int AudioStore::FindByName(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < files_.size(); ++i) {
        if (name == files_[i].name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int AudioStore::RandomInCategory(const std::string& category) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int selected = -1;
    int matches = 0;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (category == files_[i].category && (esp_random() % ++matches) == 0) {
            selected = static_cast<int>(i);
        }
    }
    return selected;
}

int AudioStore::CountInCategory(const std::string& category) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& file : files_) {
        if (category == file.category) {
            ++count;
        }
    }
    return count;
}

uint64_t AudioStore::UsedBytesLocked() const {
    uint32_t end = 0;
    for (const auto& file : files_) {
        end = std::max(end, file.offset + file.size);
    }
    return end;
}

uint64_t AudioStore::used_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return UsedBytesLocked();
}

uint64_t AudioStore::free_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t used = AlignUp(UsedBytesLocked(), erase_size_);
    return used >= capacity_ ? 0 : capacity_ - used;
}

bool AudioStore::WriteFile(const std::string& name, uint32_t size, uint32_t sample_rate,
                           uint32_t duration_ms, const std::string& category,
                           const std::function<size_t(uint8_t*, size_t)>& reader,
                           std::string* error) {
    auto fail = [error, &name](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        ESP_LOGW(TAG, "WriteFile(%s): %s", name.c_str(), message);
        return false;
    };
    if (writing_.exchange(true)) {
        return fail("另一个写入正在进行");
    }
    WritingGuard guard{writing_};
    if (name.empty() || name.size() >= sizeof(AudioFileInfo::name)) {
        return fail("文件名长度非法");
    }
    if (size == 0 || size > 32ULL * 1024 * 1024) {
        return fail("文件大小非法");
    }
    std::string cat = category.empty() ? "animal" : category;
    if (cat.size() >= sizeof(AudioFileInfo::category)) {
        return fail("分类长度非法");
    }
    // Serialize writers with writing_, but never hold the catalogue mutex
    // across network reads. Touch/actions and status queries must stay responsive.
    std::vector<AudioFileInfo> entries;
    size_t replace_index = 0;
    uint32_t offset = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries = files_;
        replace_index = entries.size();
        for (size_t i = 0; i < entries.size(); ++i) {
            if (name == entries[i].name) {
                replace_index = i;
                break;
            }
        }
        if (replace_index == entries.size() && entries.size() >= static_cast<size_t>(kMaxFiles)) {
            return fail("目录已满（32 个文件）");
        }
        // Include the old file in the allocation: it remains readable until commit.
        offset = AlignUp(UsedBytesLocked(), erase_size_);
    }

    // 先读头部做 OGG/Opus 校验，避免把垃圾数据写进 Flash。
    std::vector<uint8_t> first(kReadChunk);
    size_t first_len = reader(first.data(), std::min<size_t>(first.size(), size));
    if (first_len == 0 || first_len == static_cast<size_t>(-1) ||
        first_len > std::min<size_t>(first.size(), size)) {
        return fail("数据源读取失败");
    }
    if (!LooksLikeOggOpus(first.data(), first_len)) {
        return fail("内容不是 OGG/Opus");
    }

    // 追加到目录末尾，按擦除粒度对齐；NAND 逐单元跳过坏块。
    auto range_ok = [&](uint64_t start, uint64_t bytes) {
        for (uint64_t a = start; a < start + bytes; a += erase_size_) {
            bool ok = false;
            if (wzh_flash_unit_ok(static_cast<uint32_t>(a), &ok) != ESP_OK || !ok) {
                return false;
            }
        }
        return true;
    };
    while (static_cast<uint64_t>(data_start_) + offset + size > capacity_ ||
           !range_ok(data_start_ + offset, size)) {
        offset += erase_size_;
        if (static_cast<uint64_t>(data_start_) + offset + size > capacity_) {
            return fail("剩余空间不足（或坏块过多）");
        }
    }

    // 提前擦除所有需要的单元，之后按页顺序编程。
    for (uint64_t addr = data_start_ + offset; addr < data_start_ + offset + size;
         addr += erase_size_) {
        if (wzh_flash_erase_unit(static_cast<uint32_t>(addr)) != ESP_OK) {
            return fail("擦除失败");
        }
    }

    // 流式写入 + 逐页回读校验。programmed 是已编程进 Flash 的字节数。
    std::vector<uint8_t> page(program_size_);
    size_t page_fill = 0;
    size_t programmed = 0;
    auto program_full = [&]() {
        if (!ProgramWithVerify(data_start_ + offset + programmed, page.data(), page.size())) {
            return false;
        }
        programmed += page.size();
        page_fill = 0;
        return true;
    };
    size_t written = 0;
    auto feed_bytes = [&](const uint8_t* data, size_t len) {
        while (len > 0) {
            const size_t take = std::min(len, page.size() - page_fill);
            std::memcpy(page.data() + page_fill, data, take);
            page_fill += take;
            data += take;
            len -= take;
            if (page_fill == page.size() && !program_full()) {
                return false;
            }
        }
        return true;
    };
    if (!feed_bytes(first.data(), first_len)) {
        return fail("写入校验失败");
    }
    written = first_len;
    while (written < size) {
        const size_t want = std::min<size_t>(page.size() - page_fill, size - written);
        const size_t got = reader(page.data() + page_fill, want);
        if (got == static_cast<size_t>(-1) || got > want) {
            return fail("数据源读取失败");
        }
        if (got == 0) {
            return fail("数据提前结束");
        }
        page_fill += got;
        written += got;
        if (page_fill == page.size() && !program_full()) {
            return fail("写入校验失败");
        }
    }
    if (page_fill > 0) {
        std::memset(page.data() + page_fill, 0xFF, page.size() - page_fill);
        if (!program_full()) {
            return fail("写入校验失败");
        }
    }

    AudioFileInfo file = {};
    std::strncpy(file.name, name.c_str(), sizeof(file.name) - 1);
    file.offset = offset;
    file.size = written;
    file.sample_rate = sample_rate;
    file.duration_ms = duration_ms;
    std::strncpy(file.category, cat.c_str(), sizeof(file.category) - 1);

    if (replace_index < entries.size())
        entries[replace_index] = file;
    else
        entries.push_back(file);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!CommitToc(entries)) {
            return fail("目录提交失败");
        }
    }
    ESP_LOGI(TAG, "写入完成: %s（%lu 字节 -> offset %lu）", name.c_str(),
             static_cast<unsigned long>(written), static_cast<unsigned long>(offset));
    return true;
}

bool AudioStore::DeleteFile(const std::string& name, std::string* error) {
    auto fail = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (writing_.exchange(true)) {
        return fail("另一个写入正在进行");
    }
    WritingGuard guard{writing_};
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AudioFileInfo> entries = files_;
    bool removed = false;
    for (auto it = entries.begin(); it != entries.end();) {
        if (name == it->name) {
            it = entries.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (!removed) {
        return fail("文件不存在");
    }
    // 数据不回收（孤儿数据由下次同步或整体擦除处理）；count==0 的空目录
    // 是合法 v3 状态，重启后仍然成立。
    if (!CommitToc(entries)) {
        return fail("目录提交失败");
    }
    return true;
}

bool AudioStore::EraseAll(std::string* error) {
    auto fail = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (capacity_ == 0 || erase_size_ == 0) {
        return fail("外部音频 Flash 未就绪");
    }
    if (writing_.exchange(true)) {
        return fail("另一个写入正在进行");
    }
    WritingGuard guard{writing_};
    // NAND 逐块擦除（驱动跳过坏块）、NOR 整片擦除；两者都覆盖 TOC 区。
    if (wzh_flash_erase_all() != ESP_OK) {
        // An incomplete erase cannot leave old in-RAM entries playable.
        std::lock_guard<std::mutex> lock(mutex_);
        files_.clear();
        layout_ = Layout::kEmpty;
        current_slot_ = -1;
        toc_seq_ = 0;
        return fail("擦除未完成，音频库已停用，请重试擦除后重新导入");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    files_.clear();
    layout_ = Layout::kEmpty;
    current_slot_ = -1;
    toc_seq_ = 0;
    ESP_LOGW(TAG, "外部音频 Flash 已全部擦除");
    return true;
}

bool AudioStore::EraseAllAsync(std::function<void(bool)> on_done) {
    auto* args = new AsyncEraseArgs{this, std::move(on_done)};
    auto trampoline = [](void* arg) {
        std::unique_ptr<AsyncEraseArgs> ctx(static_cast<AsyncEraseArgs*>(arg));
        std::string error;
        const bool ok = ctx->self->EraseAll(&error);
        if (!ok) {
            ESP_LOGE(TAG, "后台全片擦除失败: %s", error.c_str());
        }
        if (ctx->on_done) {
            ctx->on_done(ok);
        }
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(trampoline, "wzh_erase_all", 6144, args, 1, nullptr) != pdPASS) {
        delete args;
        return false;
    }
    return true;
}

}  // namespace wzh
