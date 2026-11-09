#include "ssid_manager.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <algorithm>
#include <cstring>

#define TAG "SsidManager"
#define NVS_NAMESPACE "wifi"
#define MAX_WIFI_SSID_COUNT 10

namespace {
constexpr uint32_t kPortalBackupVersion = 0x314b4257;
constexpr char kPortalBackupKey[] = "portal_wbak";
struct PortalBackup {
    uint32_t version = kPortalBackupVersion;
    uint32_t count = 0;
    struct Entry {
        char ssid[33]{};
        char password[65]{};
    } entries[MAX_WIFI_SSID_COUNT];
};
}  // namespace

SsidManager::SsidManager() {
    LoadFromNvs();
    if (!RecoverPortalBackup())
        ESP_LOGE(TAG, "Wi-Fi credential recovery could not be persisted");
}

SsidManager::~SsidManager() {}

void SsidManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ssid_list_.clear();
    SaveToNvs();
}

void SsidManager::LoadFromNvs() {
    ssid_list_.clear();

    // Load ssid and password from NVS from namespace "wifi"
    // ssid, ssid1, ssid2, ... ssid9
    // password, password1, password2, ... password9
    nvs_handle_t nvs_handle;
    auto ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        // The namespace doesn't exist, just return
        ESP_LOGW(TAG, "NVS namespace %s doesn't exist", NVS_NAMESPACE);
        return;
    }
    for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {
        std::string ssid_key = "ssid";
        if (i > 0) {
            ssid_key += std::to_string(i);
        }
        std::string password_key = "password";
        if (i > 0) {
            password_key += std::to_string(i);
        }

        char ssid[33];
        char password[65];
        size_t length = sizeof(ssid);
        if (nvs_get_str(nvs_handle, ssid_key.c_str(), ssid, &length) != ESP_OK) {
            continue;
        }
        length = sizeof(password);
        if (nvs_get_str(nvs_handle, password_key.c_str(), password, &length) != ESP_OK) {
            continue;
        }
        ssid_list_.push_back({ssid, password});
    }
    nvs_close(nvs_handle);
}

void SsidManager::SaveToNvs() {
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle));
    for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {
        std::string ssid_key = "ssid";
        if (i > 0) {
            ssid_key += std::to_string(i);
        }
        std::string password_key = "password";
        if (i > 0) {
            password_key += std::to_string(i);
        }

        if (i < ssid_list_.size()) {
            nvs_set_str(nvs_handle, ssid_key.c_str(), ssid_list_[i].ssid.c_str());
            nvs_set_str(nvs_handle, password_key.c_str(), ssid_list_[i].password.c_str());
        } else {
            nvs_erase_key(nvs_handle, ssid_key.c_str());
            nvs_erase_key(nvs_handle, password_key.c_str());
        }
    }
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

bool SsidManager::SaveToNvsChecked() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    bool ok = true;
    for (int i = 0; i < MAX_WIFI_SSID_COUNT && ok; ++i) {
        const std::string suffix = i ? std::to_string(i) : "";
        const auto ssid_key = "ssid" + suffix;
        const auto pass_key = "password" + suffix;
        if (i < static_cast<int>(ssid_list_.size())) {
            ok = nvs_set_str(handle, ssid_key.c_str(), ssid_list_[i].ssid.c_str()) == ESP_OK &&
                 nvs_set_str(handle, pass_key.c_str(), ssid_list_[i].password.c_str()) == ESP_OK;
        } else {
            const auto a = nvs_erase_key(handle, ssid_key.c_str());
            const auto b = nvs_erase_key(handle, pass_key.c_str());
            ok = (a == ESP_OK || a == ESP_ERR_NVS_NOT_FOUND) &&
                 (b == ESP_OK || b == ESP_ERR_NVS_NOT_FOUND);
        }
    }
    if (ok)
        ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool SsidManager::WritePortalBackup(const std::vector<SsidItem>& snapshot) {
    if (snapshot.size() > MAX_WIFI_SSID_COUNT)
        return false;
    PortalBackup backup;
    backup.count = snapshot.size();
    for (size_t i = 0; i < snapshot.size(); ++i) {
        if (snapshot[i].ssid.empty() || snapshot[i].ssid.size() > 32 ||
            snapshot[i].password.size() > 64)
            return false;
        std::memcpy(backup.entries[i].ssid, snapshot[i].ssid.data(), snapshot[i].ssid.size());
        std::memcpy(backup.entries[i].password, snapshot[i].password.data(),
                    snapshot[i].password.size());
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
        return false;
    auto result = nvs_set_blob(nvs, kPortalBackupKey, &backup, sizeof(backup));
    if (result == ESP_OK)
        result = nvs_commit(nvs);
    nvs_close(nvs);
    volatile auto* bytes = reinterpret_cast<volatile unsigned char*>(&backup);
    for (size_t i = 0; i < sizeof(backup); ++i)
        bytes[i] = 0;
    return result == ESP_OK;
}

bool SsidManager::ClearPortalBackup() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
        return false;
    auto result = nvs_erase_key(nvs, kPortalBackupKey);
    if (result == ESP_ERR_NVS_NOT_FOUND)
        result = ESP_OK;
    if (result == ESP_OK)
        result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}

bool SsidManager::RecoverPortalBackup() {
    nvs_handle_t nvs;
    auto result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (result != ESP_OK)
        return false;
    PortalBackup backup;
    size_t size = sizeof(backup);
    result = nvs_get_blob(nvs, kPortalBackupKey, &backup, &size);
    nvs_close(nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (result != ESP_OK || size != sizeof(backup) || backup.version != kPortalBackupVersion ||
        backup.count > MAX_WIFI_SSID_COUNT) {
        ssid_list_.clear();
        return false;
    }
    std::vector<SsidItem> recovered;
    for (size_t i = 0; i < backup.count; ++i) {
        const auto& entry = backup.entries[i];
        if (!std::memchr(entry.ssid, '\0', sizeof(entry.ssid)) ||
            !std::memchr(entry.password, '\0', sizeof(entry.password)) || !entry.ssid[0]) {
            ssid_list_.clear();
            return false;
        }
        recovered.push_back({entry.ssid, entry.password});
    }
    volatile auto* bytes = reinterpret_cast<volatile unsigned char*>(&backup);
    for (size_t i = 0; i < sizeof(backup); ++i)
        bytes[i] = 0;
    ssid_list_ = std::move(recovered);
    if (!SaveToNvsChecked())
        return false;
    return ClearPortalBackup();
}

bool SsidManager::SavePortalTransaction(const std::vector<SsidItem>& fallback) {
    if (!WritePortalBackup(fallback)) {
        ssid_list_ = fallback;
        return false;
    }
    if (SaveToNvsChecked() && ClearPortalBackup())
        return true;
    ssid_list_ = fallback;
    if (SaveToNvsChecked()) {
        if (!ClearPortalBackup())
            ESP_LOGE(TAG, "Unable to clear credential recovery marker");
    } else {
        ESP_LOGE(TAG, "Unable to restore Wi-Fi credential snapshot; recovery marker retained");
    }
    return false;
}

bool SsidManager::AddSsidChecked(const std::string& ssid, const std::string& password) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto before = ssid_list_;
    auto existing = std::find_if(ssid_list_.begin(), ssid_list_.end(),
                                 [&](const SsidItem& item) { return item.ssid == ssid; });
    if (existing != ssid_list_.end())
        ssid_list_.erase(existing);
    if (ssid_list_.size() >= MAX_WIFI_SSID_COUNT)
        ssid_list_.pop_back();
    ssid_list_.insert(ssid_list_.begin(), {ssid, password});
    return SavePortalTransaction(before);
}

bool SsidManager::RestoreListChecked(const std::vector<SsidItem>& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot.size() > MAX_WIFI_SSID_COUNT)
        return false;
    ssid_list_ = snapshot;
    // If restoration is interrupted, recovery must select the old network,
    // not the already-rejected candidate.
    return SavePortalTransaction(snapshot);
}

bool SsidManager::ClearChecked() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto before = ssid_list_;
    ssid_list_.clear();
    return SavePortalTransaction(before);
}

void SsidManager::AddSsid(const std::string& ssid, const std::string& password) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : ssid_list_) {
        ESP_LOGI(TAG, "compare [%s:%d] [%s:%d]", item.ssid.c_str(), item.ssid.size(), ssid.c_str(),
                 ssid.size());
        if (item.ssid == ssid) {
            ESP_LOGW(TAG, "SSID %s already exists, overwrite it", ssid.c_str());
            item.password = password;
            SaveToNvs();
            return;
        }
    }

    if (ssid_list_.size() >= MAX_WIFI_SSID_COUNT) {
        ESP_LOGW(TAG, "SSID list is full, pop one");
        ssid_list_.pop_back();
    }
    // Add the new ssid to the front of the list
    ssid_list_.insert(ssid_list_.begin(), {ssid, password});
    SaveToNvs();
}

void SsidManager::RemoveSsid(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return;
    }
    ssid_list_.erase(ssid_list_.begin() + index);
    SaveToNvs();
}

void SsidManager::SetDefaultSsid(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return;
    }
    // Move the ssid at index to the front of the list
    auto item = ssid_list_[index];
    ssid_list_.erase(ssid_list_.begin() + index);
    ssid_list_.insert(ssid_list_.begin(), item);
    SaveToNvs();
}
