#include "wzh_onboarding_store.h"

#include <array>
#include <cstddef>
#include <mutex>

#include <nvs.h>

namespace wzh {
namespace {
constexpr char kNamespace[] = "onboarding";
constexpr char kRecord[] = "progress";
// v2 adds the "skipped" mask, so a step abandoned after three failures is
// not replayed on every boot. v1 records still load (skipped = 0).
constexpr uint8_t kVersion = 2;
constexpr uint8_t kLegacyVersion = 1;
std::recursive_mutex store_mutex;

bool ReadLegacyComplete(bool& complete) {
    complete = false;
    nvs_handle_t handle = 0;
    const auto opened = nvs_open("wzh_web", NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (opened != ESP_OK)
        return false;
    uint8_t value = 0;
    const auto result = nvs_get_u8(handle, "setup_guide", &value);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (result != ESP_OK)
        return false;
    complete = value != 0;
    return true;
}

bool WriteLegacyComplete() {
    nvs_handle_t handle = 0;
    if (nvs_open("wzh_web", NVS_READWRITE, &handle) != ESP_OK)
        return false;
    auto result = nvs_set_u8(handle, "setup_guide", 1);
    if (result == ESP_OK)
        result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}
}  // namespace

bool SaveOnboardingProgress(const OnboardingProgress& progress) {
    std::lock_guard<std::recursive_mutex> lock(store_mutex);
    if (progress.step > OnboardingProgress::kStepCount ||
        progress.attempts >= OnboardingProgress::kMaxAttempts ||
        (progress.flags & ~uint8_t{3}) != 0 || (progress.completed & ~uint16_t{0x1fff}) != 0 ||
        (progress.skipped & ~uint16_t{0x1fff}) != 0 ||
        (progress.completed & progress.skipped) != 0 ||
        ((progress.flags & OnboardingProgress::kOfflineComplete) &&
         (progress.completed & OnboardingProgress::kOfflineMask) !=
             OnboardingProgress::kOfflineMask) ||
        ((progress.flags & OnboardingProgress::kOnlineComplete) &&
         (progress.completed & OnboardingProgress::kOnlineMask) != OnboardingProgress::kOnlineMask))
        return false;
    const std::array<uint8_t, 8> bytes = {kVersion,
                                          progress.flags,
                                          progress.step,
                                          progress.attempts,
                                          static_cast<uint8_t>(progress.completed),
                                          static_cast<uint8_t>(progress.completed >> 8),
                                          static_cast<uint8_t>(progress.skipped),
                                          static_cast<uint8_t>(progress.skipped >> 8)};
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    // The individual keys make progress inspectable for existing tooling;
    // only the versioned blob is authoritative after an interrupted write.
    auto result = nvs_set_u8(handle, "flags", progress.flags);
    if (result == ESP_OK)
        result = nvs_set_u8(handle, "step", progress.step);
    if (result == ESP_OK)
        result = nvs_set_u8(handle, "attempts", progress.attempts);
    if (result == ESP_OK)
        result = nvs_set_blob(handle, kRecord, bytes.data(), bytes.size());
    if (result == ESP_OK)
        result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK && (!progress.Done() || WriteLegacyComplete());
}

bool PeekOnboardingProgress(OnboardingProgress& progress) {
    std::lock_guard<std::recursive_mutex> lock(store_mutex);
    nvs_handle_t handle = 0;
    auto result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        // No record yet (or NVS is unreadable): report "nothing done" without
        // writing. HTTP handlers poll this every few seconds.
        bool legacy = false;
        if (result == ESP_ERR_NVS_NOT_FOUND && ReadLegacyComplete(legacy) && legacy)
            progress = OnboardingProgress::MigratedComplete();
        else
            progress = OnboardingProgress{};
        return true;
    }
    std::array<uint8_t, 8> bytes{};
    size_t size = bytes.size();
    result = nvs_get_blob(handle, kRecord, bytes.data(), &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        bool legacy = false;
        progress = (ReadLegacyComplete(legacy) && legacy) ? OnboardingProgress::MigratedComplete()
                                                          : OnboardingProgress{};
        return true;
    }
    if (result != ESP_OK || bytes[1] > 3 || bytes[2] > OnboardingProgress::kStepCount ||
        bytes[3] >= OnboardingProgress::kMaxAttempts || (bytes[5] & 0xe0) != 0)
        return false;
    const bool legacy = bytes[0] == kLegacyVersion && size == 6;
    if (!legacy && (bytes[0] != kVersion || size != 8))
        return false;
    progress.flags = bytes[1];
    progress.step = bytes[2];
    progress.attempts = bytes[3];
    progress.completed = static_cast<uint16_t>(bytes[4] | (uint16_t{bytes[5]} << 8));
    progress.skipped = legacy ? 0 : static_cast<uint16_t>(bytes[6] | (uint16_t{bytes[7]} << 8));
    return true;
}

bool LoadOnboardingProgress(OnboardingProgress& progress) {
    std::lock_guard<std::recursive_mutex> lock(store_mutex);
    nvs_handle_t handle = 0;
    auto result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        bool legacy = false;
        if (!ReadLegacyComplete(legacy))
            return false;
        progress = legacy ? OnboardingProgress::MigratedComplete() : OnboardingProgress{};
        return SaveOnboardingProgress(progress);
    }
    if (result != ESP_OK)
        return false;
    // Read up to the v2 size; a v1 record is simply shorter.
    std::array<uint8_t, 8> bytes{};
    size_t size = bytes.size();
    result = nvs_get_blob(handle, kRecord, bytes.data(), &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        bool legacy = false;
        if (!ReadLegacyComplete(legacy))
            return false;
        progress = legacy ? OnboardingProgress::MigratedComplete() : OnboardingProgress{};
        return SaveOnboardingProgress(progress);
    }
    if (result != ESP_OK || bytes[1] > 3 || bytes[2] > OnboardingProgress::kStepCount ||
        bytes[3] >= OnboardingProgress::kMaxAttempts || (bytes[5] & 0xe0) != 0)
        return false;
    const bool legacy = bytes[0] == kLegacyVersion && size == 6;
    if (!legacy && (bytes[0] != kVersion || size != 8))
        return false;
    progress.flags = bytes[1];
    progress.step = bytes[2];
    progress.attempts = bytes[3];
    progress.completed = static_cast<uint16_t>(bytes[4] | (uint16_t{bytes[5]} << 8));
    progress.skipped = legacy ? 0 : static_cast<uint16_t>(bytes[6] | (uint16_t{bytes[7]} << 8));
    if (((progress.flags & OnboardingProgress::kOfflineComplete) &&
         (progress.completed & OnboardingProgress::kOfflineMask) !=
             OnboardingProgress::kOfflineMask) ||
        ((progress.flags & OnboardingProgress::kOnlineComplete) &&
         (progress.completed & OnboardingProgress::kOnlineMask) !=
             OnboardingProgress::kOnlineMask) ||
        (progress.skipped & ~uint16_t{0x1fff}) != 0 || (progress.completed & progress.skipped) != 0)
        return false;
    progress.Resume();
    return true;
}

}  // namespace wzh
