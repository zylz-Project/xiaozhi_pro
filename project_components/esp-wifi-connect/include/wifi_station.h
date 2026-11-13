#ifndef _WIFI_STATION_H_
#define _WIFI_STATION_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi_types_generic.h>

// WiFi power save level enumeration
enum class WifiPowerSaveLevel {
    LOW_POWER,    // Maximum power saving (WIFI_PS_MAX_MODEM)
    BALANCED,     // Minimum power saving (WIFI_PS_MIN_MODEM)
    PERFORMANCE,  // No power saving (WIFI_PS_NONE) - full power
};

struct WifiApRecord {
    std::string ssid;
    std::string password;
    int channel;
    wifi_auth_mode_t authmode;
    uint8_t bssid[6];
};

/**
 * WifiStation - WiFi station mode handler
 *
 * This class handles connecting to WiFi access points in station mode.
 * Note: WiFi driver must be initialized before using this class.
 */
class WifiStation {
public:
    WifiStation();
    ~WifiStation();

    // Delete copy constructor and assignment operator
    WifiStation(const WifiStation&) = delete;
    WifiStation& operator=(const WifiStation&) = delete;

    void AddAuth(const std::string&& ssid, const std::string&& password);
    void Start();
    void Stop();
    // Cancel any pending scan/queued attempts and connect to this network now.
    // Does not invoke on_connect_; the manager emits the Connecting event.
    void ConnectNow(const std::string& ssid, const std::string& password);
    bool IsConnected();
    bool WaitForConnected(int timeout_ms = 10000);
    // The station event handler owns scan results; callers must not read the
    // driver's AP list directly. This bounded wait is for worker tasks only.
    esp_err_t ScanNetworks(std::vector<wifi_ap_record_t>& records, int timeout_ms = 12000);
    int8_t GetRssi();
    std::string GetSsid() const { return ssid_; }
    std::string GetIpAddress() const { return ip_address_; }
    uint8_t GetChannel();
    void SetPowerSaveLevel(WifiPowerSaveLevel level);

    void OnConnect(std::function<void(const std::string& ssid)> on_connect);
    void OnConnected(std::function<void(const std::string& ssid)> on_connected);
    void OnDisconnected(std::function<void(int reason)> on_disconnected);
    void OnScanBegin(std::function<void()> on_scan_begin);
    void OnScanResults(std::function<void(const std::vector<wifi_ap_record_t>&)> callback) {
        on_scan_results_ = std::move(callback);
    }
    void SetScanIntervalRange(int min_interval_seconds, int max_interval_seconds);
    void SetHostname(const std::string& hostname) { hostname_ = hostname; }
    void SetPersistentAp(bool enabled) { persistent_ap_ = enabled; }

    // How many times to retry the strongest same-SSID AP before falling back to
    // a weaker one. Only effective when remember_bssid is off (default). A value
    // of 3 means the driver will attempt the best AP up to 3 extra times before
    // giving up and trying the next-best BSSID.
    void SetFailureRetryCnt(uint8_t cnt) { failure_retry_cnt_ = cnt; }

private:
    EventGroupHandle_t event_group_;
    esp_timer_handle_t timer_handle_ = nullptr;
    esp_event_handler_instance_t instance_any_id_ = nullptr;
    esp_event_handler_instance_t instance_got_ip_ = nullptr;
    esp_event_handler_instance_t instance_lost_ip_ = nullptr;
    esp_netif_t* station_netif_ = nullptr;
    std::string ssid_;
    std::string password_;
    std::string ip_address_;
    std::string hostname_;
    int8_t max_tx_power_;
    uint8_t remember_bssid_;
    uint8_t failure_retry_cnt_ = 3;  // Retries on strongest AP before falling back
    int reconnect_count_ = 0;

    // Exponential backoff for scan interval
    int scan_min_interval_microseconds_ = 10 * 1000 * 1000;      // Default 10 seconds
    int scan_max_interval_microseconds_ = 300 * 1000 * 1000;     // Default 5 minutes
    int scan_current_interval_microseconds_ = 10 * 1000 * 1000;  // Current interval
    std::function<void(const std::string& ssid)> on_connect_;
    std::function<void(const std::string& ssid)> on_connected_;
    std::function<void(int reason)> on_disconnected_;
    std::function<void()> on_scan_begin_;
    std::function<void(const std::vector<wifi_ap_record_t>&)> on_scan_results_;
    std::vector<WifiApRecord> connect_queue_;
    bool persistent_ap_ = false;
    bool was_connected_ = false;  // Track if we were connected before disconnection
    std::mutex user_scan_mutex_;
    std::mutex user_scan_result_mutex_;
    std::atomic<bool> user_scan_pending_{false};
    std::vector<wifi_ap_record_t> user_scan_results_;
    esp_err_t user_scan_error_ = ESP_FAIL;

    void HandleScanResult();
    void CompleteUserScan();
    void StartConnect();
    void UpdateScanInterval();  // Exponential backoff for scan interval
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                 void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id,
                               void* event_data);
};

#endif  // _WIFI_STATION_H_
