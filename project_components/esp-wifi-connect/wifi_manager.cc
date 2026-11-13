/*
 * WiFi Manager Implementation
 */

#include "wifi_manager.h"
#include "ssid_manager.h"
#include "wifi_configuration_ap.h"
#include "wifi_station.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <algorithm>

#define TAG "WifiManager"

WifiManager& WifiManager::GetInstance() {
    static WifiManager instance;
    return instance;
}

WifiManager::WifiManager() = default;

WifiManager::~WifiManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (station_active_ && station_) {
        station_->Stop();
    }
    if ((config_mode_active_ || persistent_portal_active_) && config_ap_) {
        config_ap_->Stop();
    }
    if (persistent_sta_netif_) {
        esp_netif_destroy_default_wifi(persistent_sta_netif_);
        persistent_sta_netif_ = nullptr;
    }
    if (initialized_) {
        esp_wifi_deinit();
    }
}

void WifiManager::NotifyEvent(WifiEvent event, const std::string& data) {
    // Copy callback under lock, invoke without lock to avoid deadlock
    std::function<void(WifiEvent, const std::string&)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = event_callback_;
    }
    if (callback) {
        callback(event, data);
    }
}

bool WifiManager::Initialize(const WifiManagerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    config_ = config;
    ESP_LOGI(TAG, "Initializing...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize netif
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Create event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return false;
    }

    station_ = std::make_unique<WifiStation>();
    config_ap_ = std::make_unique<WifiConfigurationAp>();

    initialized_ = true;
    ESP_LOGI(TAG, "Initialized");
    return true;
}

bool WifiManager::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

// ==================== Station Mode ====================

void WifiManager::StartStation() {
    std::unique_lock<std::mutex> lifecycle(station_lifecycle_mutex_, std::defer_lock);
    if (config_.persistent_portal)
        lifecycle.lock();
    std::unique_lock<std::mutex> lock(mutex_);

    if (!initialized_) {
        ESP_LOGE(TAG, "Not initialized");
        return;
    }
    if (station_active_) {
        ESP_LOGW(TAG, "Station already active");
        return;
    }

    // Auto-stop config AP if active
    if (config_mode_active_) {
        ESP_LOGI(TAG, "Stopping config AP before starting station");
        config_ap_->Stop();
        config_mode_active_ = false;
        // Notify outside lock
        lock.unlock();
        NotifyEvent(WifiEvent::ConfigModeExit);
        lock.lock();
    }

    ESP_LOGI(TAG, "Starting station");

    // Apply configuration
    station_->SetScanIntervalRange(config_.station_scan_min_interval_seconds,
                                   config_.station_scan_max_interval_seconds);
    station_->SetFailureRetryCnt(config_.station_failure_retry_cnt);
    station_->SetHostname(config_.station_hostname);
    station_->SetPersistentAp(persistent_portal_active_);
    if (persistent_portal_active_) {
        station_->OnScanResults([this](const std::vector<wifi_ap_record_t>& records) {
            config_ap_->UpdateAccessPoints(records);
        });
    }

    // Setup callbacks
    station_->OnScanBegin([this]() { NotifyEvent(WifiEvent::Scanning); });
    station_->OnConnect(
        [this](const std::string& ssid) { NotifyEvent(WifiEvent::Connecting, ssid); });
    station_->OnConnected(
        [this](const std::string& ssid) { NotifyEvent(WifiEvent::Connected, ssid); });
    station_->OnDisconnected(
        [this](int reason) { NotifyEvent(WifiEvent::Disconnected, std::to_string(reason)); });

    station_->Start();
    station_active_ = true;
}

bool WifiManager::ConnectTo(const std::string& ssid, const std::string& password) {
    if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
        ESP_LOGE(TAG, "Invalid credentials for ConnectTo");
        return false;
    }
    if (!IsInitialized()) {
        ESP_LOGE(TAG, "Not initialized");
        return false;
    }

    bool need_start = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        need_start = !station_active_;
    }
    if (need_start) {
        StartStation();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!station_active_ || !station_) {
            ESP_LOGE(TAG, "Station is not active");
            return false;
        }
    }

    // Persist first so a reboot reconnects to the newly selected network even if
    // the immediate attempt fails.
    SsidManager::GetInstance().AddSsid(ssid, password);
    NotifyEvent(WifiEvent::Connecting, ssid);
    station_->ConnectNow(ssid, password);
    ESP_LOGI(TAG, "Connecting to %s on request", ssid.c_str());
    return true;
}

void WifiManager::StopStation() {
    std::unique_lock<std::mutex> lifecycle(station_lifecycle_mutex_, std::defer_lock);
    if (config_.persistent_portal)
        lifecycle.lock();
    std::unique_lock<std::mutex> lock(mutex_);
    if (!station_active_)
        return;
    ESP_LOGI(TAG, "Stopping station");
    station_active_ = false;
    auto* station = station_.get();
    // Unregistering Wi-Fi callbacks can wait for an in-flight callback that
    // takes mutex_ via NotifyEvent(). Never hold mutex_ across station->Stop().
    lock.unlock();
    if (station)
        station->Stop();
    ESP_LOGI(TAG, "Station stopped");
    NotifyEvent(WifiEvent::Disconnected);
}

bool WifiManager::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_active_ && station_ && station_->IsConnected();
}

esp_err_t WifiManager::ScanNetworks(std::vector<wifi_ap_record_t>& records, int timeout_ms) {
    WifiStation* station = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!station_active_ || !station_)
            return ESP_ERR_INVALID_STATE;
        station = station_.get();
    }
    return station->ScanNetworks(records, timeout_ms);
}

std::string WifiManager::GetSsid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_)
        return "";
    return station_->GetSsid();
}

std::string WifiManager::GetIpAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_)
        return "";
    return station_->GetIpAddress();
}

int WifiManager::GetRssi() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_ || !station_->IsConnected())
        return 0;
    return station_->GetRssi();
}

int WifiManager::GetChannel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_ || !station_->IsConnected())
        return 0;
    return station_->GetChannel();
}

std::string WifiManager::GetMacAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mac_address_.empty()) {
        return mac_address_;
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                 mac[4], mac[5]);
        mac_address_ = buf;
    }
    return mac_address_;
}

// ==================== Config AP Mode ====================

// Persistent AP ownership is independent from the application config state.
// Its lifecycle belongs to the Wi-Fi manager, including while STA reconnects.
void WifiManager::SetPortalRegistrar(
    std::function<bool(httpd_handle_t, WifiConfigurationAp*)> registrar) {
    std::lock_guard<std::mutex> lock(mutex_);
    portal_registrar_ = std::move(registrar);
}

void WifiManager::StartPersistentPortal() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !config_.persistent_portal || !portal_registrar_ ||
        persistent_portal_active_ || config_mode_active_ || station_active_) {
        return;
    }
    // Create the STA netif before esp_wifi_start so it receives STA_START and
    // DHCP remains available during station restarts and credential trials.
    persistent_sta_netif_ = esp_netif_create_default_wifi_sta();
    if (!persistent_sta_netif_) {
        ESP_LOGE(TAG, "Unable to create persistent station interface");
        return;
    }
    config_ap_->SetSsidPrefix(config_.ssid_prefix);
    config_ap_->SetLanguage(config_.language);
    config_ap_->SetShowOtaConfig(false);
    config_ap_->SetShowSleepConfig(false);
    config_ap_->SetPersistentMode(true);
    config_ap_->SetWebServerRegistrar(portal_registrar_);
    if (!config_ap_->Start()) {
        ESP_LOGE(TAG, "Unable to start persistent user portal");
        // Restore the component's non-persistent semantics so the legacy
        // exclusive provisioning fallback still works.
        config_ap_->SetPersistentMode(false);
        esp_netif_destroy_default_wifi(persistent_sta_netif_);
        persistent_sta_netif_ = nullptr;
        return;
    }
    persistent_portal_active_ = true;
}

bool WifiManager::IsPersistentPortalActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return persistent_portal_active_;
}

bool WifiManager::TestAndSavePortalNetwork(const std::string& ssid, const std::string& password,
                                           const std::function<void()>& saved_callback) {
    portal_failure_reason_.store(0);
    if (ssid.empty() || ssid.size() > 32 || password.size() > 64)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!persistent_portal_active_ || !config_ap_)
            return false;
    }
    auto& stored = SsidManager::GetInstance();
    auto snapshot = stored.GetSsidList();
    StopStation();
    auto* station_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool saved = false;
    int failure = 0;
    if (station_netif) {
        if (config_ap_->ConnectToWifi(ssid, password)) {
            saved = stored.AddSsidChecked(ssid, password);
            if (!saved)
                failure = -1;
        } else {
            failure = config_ap_->LastConnectFailureReason();
        }
        esp_wifi_disconnect();
    } else {
        failure = -2;
    }
    if (saved && saved_callback)
        saved_callback();
    StartStation();
    if (saved) {
        const int64_t deadline = esp_timer_get_time() + 60000000;
        while (esp_timer_get_time() < deadline && (!IsConnected() || GetSsid() != ssid))
            vTaskDelay(pdMS_TO_TICKS(500));
        if (!IsConnected() || GetSsid() != ssid) {
            StopStation();
            if (!stored.RestoreListChecked(snapshot))
                ESP_LOGE(TAG, "Unable to restore saved Wi-Fi credentials");
            StartStation();
            saved = false;
            failure = -2;
        }
    }
    portal_failure_reason_.store(saved ? 0 : failure);
    for (auto& item : snapshot)
        std::fill(item.password.begin(), item.password.end(), '\0');
    return saved;
}

int WifiManager::LastPortalFailureReason() const { return portal_failure_reason_.load(); }

void WifiManager::StartConfigAp() {
    std::unique_lock<std::mutex> lock(mutex_);

    if (!initialized_) {
        ESP_LOGE(TAG, "Not initialized");
        return;
    }
    if (config_mode_active_ || persistent_portal_active_) {
        ESP_LOGW(TAG, "Config AP already active");
        return;
    }

    // Auto-stop station if active
    if (station_active_) {
        ESP_LOGI(TAG, "Stopping station before starting config AP");
        station_->Stop();
        station_active_ = false;
        lock.unlock();
        NotifyEvent(WifiEvent::Disconnected);
        lock.lock();
    }

    // Release the manager lock while the board tears down its console AP:
    // httpd_stop waits for in-flight handlers, and a status handler may query
    // WifiManager (which would otherwise deadlock on this mutex).
    lock.unlock();
    if (before_config_ap_) {
        before_config_ap_();
    }
    lock.lock();

    ESP_LOGI(TAG, "Starting config AP");

    config_ap_->SetSsidPrefix(config_.ssid_prefix);
    config_ap_->SetLanguage(config_.language);
    config_ap_->SetShowOtaConfig(config_.show_ota_config);
    config_ap_->SetShowSleepConfig(config_.show_sleep_config);

    // Web handler calls this when user submits config
    config_ap_->OnExitRequested([this]() {
        ESP_LOGI(TAG, "Config exit requested from web");
        StopConfigAp();
    });

    if (!config_ap_->Start()) {
        ESP_LOGE(TAG, "Unable to start config portal");
        return;
    }
    config_mode_active_ = true;

    lock.unlock();
    NotifyEvent(WifiEvent::ConfigModeEnter);
    lock.lock();
}

void WifiManager::StopConfigAp() {
    std::unique_lock<std::mutex> lock(mutex_);

    if (!config_mode_active_) {
        return;
    }

    ESP_LOGI(TAG, "Stopping config AP");
    config_ap_->Stop();
    config_mode_active_ = false;

    lock.unlock();
    NotifyEvent(WifiEvent::ConfigModeExit);
    lock.lock();
}

bool WifiManager::IsConfigMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_mode_active_;
}

std::string WifiManager::GetApSsid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((!config_mode_active_ && !persistent_portal_active_) || !config_ap_)
        return "";
    return config_ap_->GetSsid();
}

std::string WifiManager::GetApWebUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((!config_mode_active_ && !persistent_portal_active_) || !config_ap_)
        return "";
    return config_ap_->GetWebServerUrl();
}

// ==================== Power ====================

void WifiManager::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_) {
        return;
    }
    station_->SetPowerSaveLevel(level);
}

// ==================== Event ====================

void WifiManager::SetEventCallback(std::function<void(WifiEvent, const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}

void WifiManager::SetBeforeConfigApHook(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    before_config_ap_ = std::move(hook);
}
