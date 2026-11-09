#include "voice_config.h"

#include <nvs.h>
#include <cstring>
#include <mutex>

namespace voice_tts {
namespace {
constexpr char kNamespace[] = "voice_tts";
constexpr char kAliKey[] = "ali_key";
constexpr char kAliVoice[] = "ali_voice";
constexpr char kAliVoicePair[] = "ali_pair";
constexpr uint32_t kVoicePairVersion = 0x31564150;
struct VoicePair {
    uint32_t version = kVoicePairVersion;
    char current[129]{};
    char previous[129]{};
};
std::mutex voice_pair_mutex;
}  // namespace

static bool SaveValue(const char* name, const std::string& key, size_t max_size) {
    if (key.size() > max_size)
        return false;
    for (unsigned char c : key) {
        if (c < 0x21 || c > 0x7e)
            return false;
    }
    nvs_handle_t nvs = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &nvs) != ESP_OK)
        return false;
    esp_err_t result = key.empty() ? nvs_erase_key(nvs, name) : nvs_set_str(nvs, name, key.c_str());
    if (result == ESP_ERR_NVS_NOT_FOUND && key.empty())
        result = ESP_OK;
    if (result == ESP_OK)
        result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}

static std::string LoadValue(const char* name, size_t max_size) {
    nvs_handle_t nvs = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &nvs) != ESP_OK)
        return {};
    size_t size = 0;
    if (nvs_get_str(nvs, name, nullptr, &size) != ESP_OK || size < 2 || size > max_size + 1) {
        nvs_close(nvs);
        return {};
    }
    std::string result(size, '\0');
    if (nvs_get_str(nvs, name, result.data(), &size) != ESP_OK) {
        nvs_close(nvs);
        return {};
    }
    nvs_close(nvs);
    result.resize(size - 1);
    return result;
}

std::string LoadAliKey() { return LoadValue(kAliKey, 256); }
bool SaveAliKey(const std::string& key) { return SaveValue(kAliKey, key, 256); }

static bool LoadPairOrLegacy(VoicePair& pair) {
    nvs_handle_t nvs = 0;
    const auto opened = nvs_open(kNamespace, NVS_READONLY, &nvs);
    if (opened == ESP_ERR_NVS_NOT_FOUND) {
        pair = VoicePair{};
        return true;
    }
    if (opened != ESP_OK)
        return false;
    size_t size = sizeof(pair);
    auto result = nvs_get_blob(nvs, kAliVoicePair, &pair, &size);
    bool legacy = false;
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        legacy = true;
        pair = VoicePair{};
        size = sizeof(pair.current);
        result = nvs_get_str(nvs, kAliVoice, pair.current, &size);
        if (result == ESP_ERR_NVS_NOT_FOUND)
            result = ESP_OK;
    }
    nvs_close(nvs);
    return result == ESP_OK && pair.version == kVoicePairVersion &&
           std::memchr(pair.current, '\0', sizeof(pair.current)) &&
           std::memchr(pair.previous, '\0', sizeof(pair.previous)) &&
           (legacy ? (size >= 1 && size <= sizeof(pair.current)) : size == sizeof(pair));
}
static bool SavePair(const VoicePair& pair) {
    nvs_handle_t nvs = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &nvs) != ESP_OK)
        return false;
    auto result = nvs_set_blob(nvs, kAliVoicePair, &pair, sizeof(pair));
    if (result == ESP_OK)
        result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}
std::string LoadAliVoice() {
    std::lock_guard<std::mutex> lock(voice_pair_mutex);
    VoicePair pair;
    return LoadPairOrLegacy(pair) ? pair.current : "";
}
std::string LoadPreviousAliVoice() {
    std::lock_guard<std::mutex> lock(voice_pair_mutex);
    VoicePair pair;
    return LoadPairOrLegacy(pair) ? pair.previous : "";
}
bool SaveAliVoice(const std::string& voice) {
    if (voice.size() > 128)
        return false;
    for (unsigned char c : voice)
        if (c < 0x21 || c > 0x7e)
            return false;
    std::lock_guard<std::mutex> lock(voice_pair_mutex);
    VoicePair pair;
    if (!LoadPairOrLegacy(pair))
        return false;
    if (voice == pair.current)
        return true;
    if (pair.current[0])
        std::memcpy(pair.previous, pair.current, sizeof(pair.previous));
    std::memset(pair.current, 0, sizeof(pair.current));
    std::memcpy(pair.current, voice.data(), voice.size());
    return SavePair(pair);
}
bool CommitAliVoice(const std::string& voice) {
    // Enrollment starts only when Ali is selected, so one existing key is the
    // entire persistent commit. The previous ID remains the fallback on error.
    return !voice.empty() && SaveAliVoice(voice);
}
bool RestorePreviousAliVoice() {
    std::lock_guard<std::mutex> lock(voice_pair_mutex);
    VoicePair pair;
    if (!LoadPairOrLegacy(pair))
        return false;
    if (!pair.previous[0])
        return false;
    char saved[sizeof(pair.current)];
    std::memcpy(saved, pair.current, sizeof(saved));
    std::memcpy(pair.current, pair.previous, sizeof(pair.current));
    std::memcpy(pair.previous, saved, sizeof(pair.previous));
    return SavePair(pair);
}
bool ClearAliVoiceSelection() {
    std::lock_guard<std::mutex> lock(voice_pair_mutex);
    VoicePair current;
    if (!LoadPairOrLegacy(current))
        return false;
    VoicePair empty;
    if (!SavePair(empty))
        return false;
    return SaveValue(kAliVoice, "", 128);
}
bool ForgetPreviousAliVoice(const std::string& voice) {
    std::lock_guard<std::mutex> lock(voice_pair_mutex);
    VoicePair pair;
    if (!LoadPairOrLegacy(pair) || voice == pair.current)
        return false;
    if (voice != pair.previous)
        return true;
    std::memset(pair.previous, 0, sizeof(pair.previous));
    return SavePair(pair);
}
}  // namespace voice_tts
