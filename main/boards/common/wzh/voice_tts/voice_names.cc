#include "voice_names.h"

#include <nvs.h>
#include <cstring>
#include <mutex>

namespace voice_tts {
namespace {
constexpr uint32_t kVersion = 0x314e5650;
constexpr uint32_t kVersionDuplicateNames = 0x324e5650;
constexpr size_t kLimit = 10;
struct Entry {
    char id[129];
    char name[49];
};
struct Record {
    uint32_t version;
    uint32_t count;
    Entry entries[kLimit];
};
std::mutex names_mutex;

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 128)
        return false;
    for (unsigned char c : id)
        if (c < 0x21 || c > 0x7e)
            return false;
    return true;
}
bool ValidName(const std::string& name) {
    if (name.empty() || name.size() > 48)
        return false;
    for (size_t i = 0; i < name.size();) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7f)
            return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        const size_t n = c >= 0xc2 && c <= 0xdf   ? 2
                         : c >= 0xe0 && c <= 0xef ? 3
                         : c >= 0xf0 && c <= 0xf4 ? 4
                                                  : 0;
        if (!n || i + n > name.size())
            return false;
        for (size_t j = 1; j < n; ++j)
            if ((static_cast<unsigned char>(name[i + j]) & 0xc0) != 0x80)
                return false;
        const auto second = static_cast<unsigned char>(name[i + 1]);
        if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0) ||
            (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90))
            return false;
        i += n;
    }
    return true;
}
bool Read(Record& record) {
    record = {};
    record.version = kVersion;
    nvs_handle_t nvs = 0;
    const auto opened = nvs_open("voice_tts", NVS_READONLY, &nvs);
    if (opened == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (opened != ESP_OK)
        return false;
    size_t size = sizeof(record);
    const auto result = nvs_get_blob(nvs, "voice_names", &record, &size);
    nvs_close(nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        record = {};
        record.version = kVersion;
        return true;
    }
    if (result != ESP_OK || size != sizeof(record) ||
        (record.version != kVersion && record.version != kVersionDuplicateNames) ||
        record.count > kLimit)
        return false;
    for (size_t i = 0; i < record.count; ++i) {
        const auto& e = record.entries[i];
        if (!std::memchr(e.id, '\0', sizeof(e.id)) || !std::memchr(e.name, '\0', sizeof(e.name)) ||
            !ValidId(e.id) || !ValidName(e.name))
            return false;
        for (size_t j = 0; j < i; ++j)
            if (!std::strcmp(e.id, record.entries[j].id) ||
                (record.version == kVersion && !std::strcmp(e.name, record.entries[j].name)))
                return false;
    }
    return true;
}
bool Write(const Record& record) {
    Record stored = record;
    stored.version = kVersion;
    for (size_t i = 0; i < stored.count; ++i)
        for (size_t j = 0; j < i; ++j)
            if (!std::strcmp(stored.entries[i].name, stored.entries[j].name))
                stored.version = kVersionDuplicateNames;
    nvs_handle_t nvs = 0;
    if (nvs_open("voice_tts", NVS_READWRITE, &nvs) != ESP_OK)
        return false;
    auto result = nvs_set_blob(nvs, "voice_names", &stored, sizeof(stored));
    if (result == ESP_OK)
        result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}
}  // namespace

bool IsValidVoiceName(const std::string& name) { return ValidName(name); }

bool LoadVoiceNames(std::vector<VoiceName>& names) {
    std::lock_guard<std::mutex> lock(names_mutex);
    names.clear();
    Record record{};
    if (!Read(record))
        return false;
    for (size_t i = 0; i < record.count; ++i)
        names.push_back({record.entries[i].id, record.entries[i].name});
    return true;
}
bool SetVoiceName(const std::string& id, const std::string& name) {
    if (!ValidId(id) || !ValidName(name))
        return false;
    std::lock_guard<std::mutex> lock(names_mutex);
    Record record{};
    if (!Read(record))
        return false;
    size_t slot = record.count;
    for (size_t i = 0; i < record.count; ++i) {
        if (id == record.entries[i].id)
            slot = i;
    }
    if (slot == record.count) {
        if (record.count == kLimit)
            return false;
        ++record.count;
    } else if (name == record.entries[slot].name) {
        return true;
    }
    record.entries[slot] = {};
    std::memcpy(record.entries[slot].id, id.data(), id.size());
    std::memcpy(record.entries[slot].name, name.data(), name.size());
    return Write(record);
}
bool RemoveVoiceName(const std::string& id) {
    if (!ValidId(id))
        return false;
    std::lock_guard<std::mutex> lock(names_mutex);
    Record record{};
    if (!Read(record))
        return false;
    for (size_t i = 0; i < record.count; ++i) {
        if (id == record.entries[i].id) {
            for (size_t j = i + 1; j < record.count; ++j)
                record.entries[j - 1] = record.entries[j];
            record.entries[--record.count] = {};
            return Write(record);
        }
    }
    return true;
}
}  // namespace voice_tts
