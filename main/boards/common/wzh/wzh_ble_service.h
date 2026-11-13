#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

struct ble_gap_event;
struct ble_gatt_access_ctxt;
struct ble_gatt_svc_def;

namespace wzh {

class WebConsole;

// BLE transport for the mini program. Advertises the NUS service under the
// product name (prefix + last two MAC bytes) and exchanges newline-delimited
// JSON frames with a single connected phone.
class BleService {
public:
    struct Config {
        std::string name_prefix;   // e.g. "CrawlPanda"
        std::string product_id;    // e.g. "WZH-CP01"
        std::string product_name;  // e.g. "爬爬熊猫"
        std::string model;
        WebConsole* console = nullptr;
    };

    BleService() = default;
    ~BleService() = default;

    void Configure(Config config);
    bool Prepare();
    bool Start();
    bool started() const { return started_.load(); }

private:
    static void HostTask(void* arg);
    static void WorkerTask(void* arg);
    static void WifiWatchTask(void* arg);
    static void VoiceWatchTask(void* arg);
    static void ResetWatchTask(void* arg);
    static void OnSync();
    static void OnReset(int reason);
    static int GapEvent(struct ble_gap_event* event, void* arg);
    static int GattAccess(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt* ctxt, void* arg);
    static const struct ble_gatt_svc_def* ServiceTable();

    void StartAdvertising();
    void HandleLine(const std::string& line);
    bool ProcessIncoming();
    void ResetSession();

    void Notify(const std::string& payload);
    void SendResponse(int id, cJSON* data);
    void SendError(int id, int code, const char* message);
    void SendEvent(const char* event, cJSON* data);

    void HandleHello(int id);
    void HandleStatus(int id);
    void HandleChat(int id, bool start);
    void HandleSetVolume(int id, cJSON* data);
    void HandleSetVoice(int id, cJSON* data);
    void HandleAudition(int id, cJSON* data);
    void HandleVoiceList(int id);
    void HandleVoiceRename(int id, cJSON* data);
    void HandleVoiceDelete(int id, cJSON* data);
    void HandleVoiceRecord(int id, cJSON* data);
    void HandleUploadBegin(int id, cJSON* data);
    void HandleUploadChunk(int id, cJSON* data);
    void HandleUploadEnd(int id);
    void HandleUploadCancel(int id);
    void HandleWifiScan(int id);
    void HandleWifiStatus(int id);
    void HandleWifiConnect(int id, cJSON* data);
    void HandleResetFactory(int id);

    cJSON* BuildStatusObject();
    cJSON* BuildDeviceObject();
    cJSON* BuildVoiceList();
    std::string CurrentVoiceId();
    std::string CurrentVoiceName();
    bool ResolveVoice(cJSON* data, std::string& id, std::string& name);
    void ReleaseUpload();

    Config config_;
    std::string adv_name_;
    std::atomic<bool> started_{false};
    bool prepared_ = false;
    TaskHandle_t worker_ = nullptr;
    TaskHandle_t host_ = nullptr;
    std::atomic<uint16_t> conn_handle_{0xFFFF};
    std::atomic<uint16_t> mtu_{23};
    uint8_t own_addr_type_ = 0;
    StreamBufferHandle_t stream_ = nullptr;
    std::mutex tx_mutex_;
    std::mutex rx_mutex_;
    std::string rx_buffer_;
    std::atomic<bool> session_reset_{false};

    std::mutex upload_mutex_;
    uint8_t* upload_buffer_ = nullptr;
    size_t upload_size_ = 0;    // allocated bytes: 1 + name length + PCM bytes
    size_t upload_total_ = 0;   // PCM bytes expected
    size_t upload_offset_ = 0;  // PCM bytes received
    std::string upload_name_;
};

}  // namespace wzh
