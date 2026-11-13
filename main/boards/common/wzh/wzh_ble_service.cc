#include "wzh_ble_service.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <cJSON.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "voice_tts/voice_config.h"
#include "voice_tts/voice_identity.h"
#include "voice_tts/voice_manager.h"
#include "voice_tts/voice_names.h"
#include "voice_tts/voice_web.h"
#include "wifi_manager.h"
#include "wzh_voice_services.h"
#include "wzh_web_console.h"

extern "C" void ble_store_config_init(void);

namespace {

const char* const kTag = "WzhBle";

constexpr uint16_t kPreferredMtu = 247;
constexpr size_t kMaxFrame = 4096;
constexpr size_t kMaxChunkBytes = 2048;
constexpr int kVoiceCapacity = 10;
constexpr int kMinPcmBytes = 160000;  // 5 s at 16 kHz mono PCM16
constexpr int kMaxPcmBytes = 640000;  // 20 s

// Nordic UART Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E.
const ble_uuid128_t kServiceUuid = {{BLE_UUID_TYPE_128},
                                    {0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3,
                                     0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e}};
const ble_uuid128_t kRxUuid = {{BLE_UUID_TYPE_128},
                               {0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3,
                                0xb5, 0x02, 0x00, 0x40, 0x6e}};
const ble_uuid128_t kTxUuid = {{BLE_UUID_TYPE_128},
                               {0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3,
                                0xb5, 0x03, 0x00, 0x40, 0x6e}};

uint16_t g_rx_value_handle = 0;
uint16_t g_tx_value_handle = 0;
wzh::BleService* g_instance = nullptr;

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

bool Base64Decode(const std::string& input, std::vector<uint8_t>& output) {
    output.clear();
    output.reserve(input.size() * 3 / 4 + 3);
    uint32_t value = 0;
    int bits = -8;
    for (char c : input) {
        if (c == '=')
            break;
        if (c == '\r' || c == '\n' || c == ' ')
            continue;
        int digit = Base64Value(c);
        if (digit < 0)
            return false;
        value = (value << 6) | static_cast<uint32_t>(digit);
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<uint8_t>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return true;
}

std::string JsonString(cJSON* object, const char* key) {
    if (object == nullptr)
        return {};
    cJSON* item = cJSON_GetObjectItem(object, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr)
        return {};
    return item->valuestring;
}

int JsonInt(cJSON* object, const char* key, int fallback) {
    if (object == nullptr)
        return fallback;
    cJSON* item = cJSON_GetObjectItem(object, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

const char* StateName(DeviceState state) {
    switch (state) {
        case kDeviceStateIdle:
            return "idle";
        case kDeviceStateConnecting:
            return "connecting";
        case kDeviceStateListening:
            return "listening";
        case kDeviceStateSpeaking:
            return "speaking";
        case kDeviceStateStarting:
            return "starting";
        case kDeviceStateWifiConfiguring:
            return "wifi_configuring";
        case kDeviceStateUpgrading:
            return "upgrading";
        case kDeviceStateActivating:
            return "activating";
        case kDeviceStateAudioTesting:
            return "audio_testing";
        case kDeviceStateFatalError:
            return "fatal_error";
        default:
            return "idle";
    }
}

const char* SignalText(int rssi) {
    if (rssi >= -55)
        return "强";
    if (rssi >= -70)
        return "中";
    return "弱";
}

int SignalBars(int rssi) {
    if (rssi >= -55)
        return 4;
    if (rssi >= -67)
        return 3;
    if (rssi >= -78)
        return 2;
    return 1;
}

std::string BandText(int channel) { return channel > 14 ? "5G" : "2.4G"; }

std::string RuntimeText(int minutes) {
    if (minutes <= 0)
        return "0m";
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%dh%02d", minutes / 60, minutes % 60);
    return buffer;
}

using voice_tts::VoiceOperation;

int VoiceErrorCode(VoiceOperation result) {
    switch (result) {
        case VoiceOperation::kOk:
            return 0;
        case VoiceOperation::kBusy:
            return 1010;
        case VoiceOperation::kInvalid:
            return 1003;
        case VoiceOperation::kConfig:
            return 1014;
        case VoiceOperation::kNetwork:
            return 1005;
        case VoiceOperation::kFull:
            return 1007;
        case VoiceOperation::kCancelled:
            return 1000;
        case VoiceOperation::kUncertain:
            return 1009;
        case VoiceOperation::kSave:
            return 1000;
        default:
            return 1000;
    }
}

struct WifiWatchArgs {
    wzh::BleService* service;
    std::string ssid;
};

struct VoiceWatchArgs {
    wzh::BleService* service;
    std::string name;
};

}  // namespace

namespace wzh {

void BleService::Configure(Config config) { config_ = std::move(config); }

bool BleService::Prepare() {
    if (prepared_)
        return true;

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
    const std::string prefix = config_.name_prefix.empty() ? "WZH" : config_.name_prefix;
    adv_name_ = prefix + "-" + suffix;

    if (stream_ == nullptr) {
        stream_ = xStreamBufferCreate(8192, 1);
    }
    if (stream_ == nullptr) {
        ESP_LOGE(kTag, "BLE 接收缓冲创建失败");
        return false;
    }

    // The controller uses internal DMA memory and may assert inside the radio
    // task if an allocation fails. Refuse late/low-memory initialization.
    constexpr uint32_t kControllerCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    const size_t free_dma = heap_caps_get_free_size(kControllerCaps);
    const size_t largest_dma = heap_caps_get_largest_free_block(kControllerCaps);
    ESP_LOGI(kTag, "BLE 初始化前 DMA 内存: free=%u largest=%u", static_cast<unsigned>(free_dma),
             static_cast<unsigned>(largest_dma));
    if (free_dma < 80 * 1024 || largest_dma < 32 * 1024) {
        ESP_LOGE(kTag, "BLE 未初始化：内部 DMA 内存不足，保持设备运行");
        return false;
    }

    // This worker can write NVS, so its stack must remain in internal RAM.
    if (xTaskCreate(WorkerTask, "wzh_ble", 8192, this, 4, &worker_) != pdPASS) {
        ESP_LOGE(kTag, "BLE 工作任务创建失败 (internal=%u internal_largest=%u)",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        return false;
    }

    g_instance = this;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "NimBLE 初始化失败: %s (total=%u internal=%u internal_largest=%u)",
                 esp_err_to_name(err), static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        g_instance = nullptr;
        vTaskDelete(worker_);
        worker_ = nullptr;
        return false;
    }

    ble_hs_cfg.reset_cb = OnReset;
    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    const struct ble_gatt_svc_def* services = ServiceTable();
    ble_gatts_count_cfg(services);
    ble_gatts_add_svcs(services);
    ble_svc_gap_device_name_set(adv_name_.c_str());
    ble_att_set_preferred_mtu(kPreferredMtu);
    ble_store_config_init();
    // Reserve the NimBLE host stack while internal memory is available, but
    // do not run the host or advertise until the rest of the board is ready.
    if (xTaskCreatePinnedToCore(HostTask, "nimble_host", NIMBLE_HS_STACK_SIZE, nullptr,
                                configMAX_PRIORITIES - 4, &host_, NIMBLE_CORE) != pdPASS) {
        ESP_LOGE(kTag, "NimBLE 主机任务创建失败 (internal=%u internal_largest=%u)",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        nimble_port_deinit();
        g_instance = nullptr;
        vTaskDelete(worker_);
        worker_ = nullptr;
        return false;
    }
    prepared_ = true;
    ESP_LOGI(kTag, "BLE 控制器已预初始化");
    return true;
}

bool BleService::Start() {
    if (!prepared_) {
        ESP_LOGE(kTag, "BLE 未启动：控制器未在启动早期准备好");
        return false;
    }
    if (started_.exchange(true))
        return true;

    xTaskNotifyGive(host_);

    ESP_LOGI(kTag, "BLE 已启动，广播名=%s", adv_name_.c_str());
    return true;
}

const struct ble_gatt_svc_def* BleService::ServiceTable() {
    static const struct ble_gatt_chr_def characteristics[] = {
        {&kRxUuid.u, &BleService::GattAccess, nullptr, nullptr,
         BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP, 0, &g_rx_value_handle, nullptr},
        {&kTxUuid.u, &BleService::GattAccess, nullptr, nullptr, BLE_GATT_CHR_F_NOTIFY, 0,
         &g_tx_value_handle, nullptr},
        {nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr},
    };
    static const struct ble_gatt_svc_def services[] = {
        {BLE_GATT_SVC_TYPE_PRIMARY, &kServiceUuid.u, nullptr, characteristics},
        {0, nullptr, nullptr, nullptr},
    };
    return services;
}

void BleService::HostTask(void*) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    nimble_port_run();
    vTaskDelete(nullptr);
}

void BleService::OnReset(int reason) { ESP_LOGW(kTag, "NimBLE 复位，reason=%d", reason); }

void BleService::OnSync() {
    auto* self = g_instance;
    if (self == nullptr)
        return;
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(kTag, "BLE 地址初始化失败: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &self->own_addr_type_);
    if (rc != 0) {
        ESP_LOGE(kTag, "BLE 地址类型推断失败: %d", rc);
        return;
    }
    self->StartAdvertising();
}

void BleService::StartAdvertising() {
    if (!started_.load() || conn_handle_.load() != 0xFFFF)
        return;

    ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    // A 128-bit UUID plus the product name exceeds the 31-byte legacy
    // advertising payload, so the name is the only discovery key. The mini
    // program filters by name prefix and then discovers the NUS service.
    fields.name = reinterpret_cast<const uint8_t*>(adv_name_.c_str());
    fields.name_len = static_cast<uint8_t>(adv_name_.size());
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(kTag, "BLE 广播字段设置失败: %d", rc);
    }

    ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type_, nullptr, BLE_HS_FOREVER, &params, GapEvent, this);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(kTag, "BLE 广播启动失败: %d", rc);
    } else if (rc == 0) {
        ESP_LOGI(kTag, "BLE 广播已启动，名称=%s", adv_name_.c_str());
    }
}

int BleService::GapEvent(struct ble_gap_event* event, void* arg) {
    auto* self = static_cast<BleService*>(arg);
    if (self == nullptr)
        return 0;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                self->conn_handle_.store(event->connect.conn_handle);
                ESP_LOGI(kTag, "手机已连接，conn=%u", event->connect.conn_handle);
            } else {
                self->StartAdvertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(kTag, "手机断开，reason=%d", event->disconnect.reason);
            self->conn_handle_.store(0xFFFF);
            self->ResetSession();
            self->StartAdvertising();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            self->StartAdvertising();
            break;
        case BLE_GAP_EVENT_MTU:
            self->mtu_.store(event->mtu.value);
            ESP_LOGI(kTag, "MTU=%u", event->mtu.value);
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(kTag, "通知订阅=%d handle=%u", event->subscribe.cur_notify,
                     event->subscribe.attr_handle);
            break;
        default:
            break;
    }
    return 0;
}

int BleService::GattAccess(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    auto* self = g_instance;
    if (self == nullptr || ctxt == nullptr)
        return BLE_ATT_ERR_UNLIKELY;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    uint8_t buffer[512];
    uint16_t length = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buffer, sizeof(buffer), &length);
    if (rc != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (length == 0)
        return 0;
    if (xStreamBufferSend(self->stream_, buffer, length, 0) != length) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

void BleService::WorkerTask(void* arg) {
    auto* self = static_cast<BleService*>(arg);
    uint8_t buffer[256];
    while (true) {
        size_t received =
            xStreamBufferReceive(self->stream_, buffer, sizeof(buffer), portMAX_DELAY);
        if (received == 0)
            continue;
        {
            std::lock_guard<std::mutex> lock(self->rx_mutex_);
            self->rx_buffer_.append(reinterpret_cast<const char*>(buffer), received);
            if (self->rx_buffer_.size() > 32768)
                self->rx_buffer_.clear();
        }
        while (self->ProcessIncoming()) {
            // Yield between batches so a burst of tiny frames cannot starve the
            // NimBLE host task; the remaining newline-terminated lines are
            // drained on the next iteration.
            vTaskDelay(1);
        }
    }
}

bool BleService::ProcessIncoming() {
    std::vector<std::string> lines;
    bool more = false;
    {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        size_t position = 0;
        while ((position = rx_buffer_.find('\n')) != std::string::npos) {
            std::string line = rx_buffer_.substr(0, position);
            rx_buffer_.erase(0, position + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                lines.push_back(std::move(line));
            if (lines.size() >= 8) {
                more = rx_buffer_.find('\n') != std::string::npos;
                break;
            }
        }
    }
    for (const auto& line : lines) {
        if (line.size() > kMaxFrame) {
            SendError(0, 1001, "帧过长");
            continue;
        }
        HandleLine(line);
    }
    // HandleLine can take long enough for a new write to append a complete
    // line after the batch above was captured. Re-check under the lock so the
    // worker drains it instead of blocking in xStreamBufferReceive with a
    // complete frame stranded in rx_buffer_.
    std::lock_guard<std::mutex> lock(rx_mutex_);
    return more || rx_buffer_.find('\n') != std::string::npos;
}

void BleService::ResetSession() {
    {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        rx_buffer_.clear();
    }
    ReleaseUpload();
}

void BleService::ReleaseUpload() {
    std::lock_guard<std::mutex> lock(upload_mutex_);
    if (upload_buffer_ != nullptr) {
        free(upload_buffer_);
        upload_buffer_ = nullptr;
    }
    upload_size_ = 0;
    upload_total_ = 0;
    upload_offset_ = 0;
    upload_name_.clear();
}

void BleService::Notify(const std::string& payload) {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    uint16_t conn = conn_handle_.load();
    if (!started_.load() || conn == 0xFFFF)
        return;

    uint16_t mtu = mtu_.load();
    size_t fragment = mtu > 3 ? static_cast<size_t>(mtu - 3) : 20;
    if (fragment > 244)
        fragment = 244;
    if (fragment == 0)
        fragment = 20;

    for (size_t offset = 0; offset < payload.size(); offset += fragment) {
        size_t length = std::min(fragment, payload.size() - offset);
        struct os_mbuf* om = ble_hs_mbuf_from_flat(payload.data() + offset, length);
        if (om == nullptr) {
            ESP_LOGW(kTag, "BLE 通知缓冲不足");
            break;
        }
        int rc = ble_gatts_notify_custom(conn, g_tx_value_handle, om);
        if (rc != 0) {
            ESP_LOGW(kTag, "BLE 通知失败: %d", rc);
            break;
        }
        if (offset + length < payload.size()) {
            vTaskDelay(pdMS_TO_TICKS(8));
        }
    }
}

void BleService::SendResponse(int id, cJSON* data) {
    cJSON* out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "id", id);
    cJSON_AddBoolToObject(out, "ok", 1);
    cJSON_AddItemToObject(out, "data", data != nullptr ? data : cJSON_CreateObject());
    char* text = cJSON_PrintUnformatted(out);
    if (text != nullptr) {
        Notify(std::string(text) + "\n");
        cJSON_free(text);
    }
    cJSON_Delete(out);
}

void BleService::SendError(int id, int code, const char* message) {
    cJSON* out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "id", id);
    cJSON_AddBoolToObject(out, "ok", 0);
    cJSON* error = cJSON_CreateObject();
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message != nullptr ? message : "");
    cJSON_AddItemToObject(out, "error", error);
    char* text = cJSON_PrintUnformatted(out);
    if (text != nullptr) {
        Notify(std::string(text) + "\n");
        cJSON_free(text);
    }
    cJSON_Delete(out);
}

void BleService::SendEvent(const char* event, cJSON* data) {
    cJSON* out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "event", event);
    cJSON_AddItemToObject(out, "data", data != nullptr ? data : cJSON_CreateObject());
    char* text = cJSON_PrintUnformatted(out);
    if (text != nullptr) {
        Notify(std::string(text) + "\n");
        cJSON_free(text);
    }
    cJSON_Delete(out);
}

void BleService::HandleLine(const std::string& line) {
    cJSON* root = cJSON_Parse(line.c_str());
    if (root == nullptr) {
        SendError(0, 1001, "帧格式错误");
        return;
    }
    int id = JsonInt(root, "id", 0);
    std::string op = JsonString(root, "op");
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (data != nullptr && !cJSON_IsObject(data))
        data = nullptr;

    if (op.empty()) {
        SendError(id, 1002, "未知操作");
    } else if (op == "hello") {
        HandleHello(id);
    } else if (op == "ping") {
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "pong", 1);
        cJSON_AddNumberToObject(out, "uptime", esp_timer_get_time() / 1000000);
        SendResponse(id, out);
    } else if (op == "get_status") {
        HandleStatus(id);
    } else if (op == "chat_start") {
        HandleChat(id, true);
    } else if (op == "chat_stop") {
        HandleChat(id, false);
    } else if (op == "set_volume") {
        HandleSetVolume(id, data);
    } else if (op == "set_voice") {
        HandleSetVoice(id, data);
    } else if (op == "audition") {
        HandleAudition(id, data);
    } else if (op == "voice_list") {
        HandleVoiceList(id);
    } else if (op == "voice_rename") {
        HandleVoiceRename(id, data);
    } else if (op == "voice_delete") {
        HandleVoiceDelete(id, data);
    } else if (op == "voice_record") {
        HandleVoiceRecord(id, data);
    } else if (op == "voice_upload_begin") {
        HandleUploadBegin(id, data);
    } else if (op == "voice_upload_chunk") {
        HandleUploadChunk(id, data);
    } else if (op == "voice_upload_end") {
        HandleUploadEnd(id);
    } else if (op == "voice_upload_cancel") {
        HandleUploadCancel(id);
    } else if (op == "wifi_scan") {
        HandleWifiScan(id);
    } else if (op == "wifi_status") {
        HandleWifiStatus(id);
    } else if (op == "wifi_connect") {
        HandleWifiConnect(id, data);
    } else if (op == "reset_factory") {
        HandleResetFactory(id);
    } else if (op == "voice_create") {
        SendError(id, 1003, "请通过 voice_upload_begin/chunk/end 上传音频");
    } else {
        SendError(id, 1002, "未知操作");
    }

    cJSON_Delete(root);
}

void BleService::HandleHello(int id) {
    cJSON* out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "device", BuildDeviceObject());
    cJSON_AddItemToObject(out, "status", BuildStatusObject());
    cJSON* auth = cJSON_CreateObject();
    cJSON_AddBoolToObject(auth, "required", 0);
    cJSON_AddItemToObject(out, "auth", auth);
    SendResponse(id, out);
}

void BleService::HandleStatus(int id) { SendResponse(id, BuildStatusObject()); }

void BleService::HandleChat(int id, bool start) {
    auto& app = Application::GetInstance();
    if (start) {
        if (app.GetDeviceState() != kDeviceStateIdle) {
            SendError(id, 1010, "设备正在对话或连接中");
            return;
        }
        app.ToggleConversation();
    } else {
        app.StopInteraction(true);
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "state", StateName(app.GetDeviceState()));
    SendResponse(id, out);
}

void BleService::HandleSetVolume(int id, cJSON* data) {
    if (data == nullptr) {
        SendError(id, 1003, "缺少音量参数");
        return;
    }
    int volume = JsonInt(data, "volume", -1);
    if (volume < 0 || volume > 100) {
        SendError(id, 1003, "音量需在 0 到 100 之间");
        return;
    }
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        SendError(id, 1000, "音频服务不可用");
        return;
    }
    codec->SetOutputVolume(volume);
    cJSON* out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "volume", volume);
    SendResponse(id, out);
}

void BleService::HandleSetVoice(int id, cJSON* data) {
    std::string voice_id;
    std::string voice_name;
    if (!ResolveVoice(data, voice_id, voice_name)) {
        SendError(id, 1008, "音色不存在");
        return;
    }
    auto& app = Application::GetInstance();
    if (VoiceManagementBusy() || app.IsVoiceEnrollmentActive() ||
        app.IsExternalVoicePreviewActive()) {
        SendError(id, 1010, "声音服务正在处理，请稍后重试");
        return;
    }
    VoiceOperation result = voice_tts::SelectKnownVoice(voice_id, []() { return false; });
    if (result != VoiceOperation::kOk) {
        SendError(id, VoiceErrorCode(result), "切换音色失败");
        return;
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "id", voice_id.c_str());
    cJSON_AddStringToObject(out, "name", voice_name.c_str());
    SendResponse(id, out);
}

void BleService::HandleAudition(int id, cJSON* data) {
    std::string voice_id;
    std::string voice_name;
    if (!ResolveVoice(data, voice_id, voice_name)) {
        SendError(id, 1008, "音色不存在");
        return;
    }
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) {
        SendError(id, 1010, "设备正在对话或连接中");
        return;
    }
    if (!app.RequestExternalVoicePreview(voice_id)) {
        SendError(id, 1010, "试听暂不可用");
        return;
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "accepted", 1);
    SendResponse(id, out);
}

void BleService::HandleVoiceList(int id) {
    if (VoiceManagementBusy()) {
        SendError(id, 1010, "声音服务正在处理，请稍后重试");
        return;
    }
    SendResponse(id, BuildVoiceList());
}

void BleService::HandleVoiceRename(int id, cJSON* data) {
    std::string voice_id = JsonString(data, "id");
    std::string name = JsonString(data, "name");
    if (voice_id.empty() || !voice_tts::IsValidVoiceName(name)) {
        SendError(id, 1003, "音色名称不合法");
        return;
    }
    VoiceOperation result = voice_tts::RenameManagedVoice(voice_id, name);
    if (result != VoiceOperation::kOk) {
        SendError(id, VoiceErrorCode(result), "重命名失败");
        return;
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "id", voice_id.c_str());
    cJSON_AddStringToObject(out, "name", name.c_str());
    SendResponse(id, out);
}

void BleService::HandleVoiceDelete(int id, cJSON* data) {
    std::string voice_id = JsonString(data, "id");
    if (voice_id.empty()) {
        SendError(id, 1003, "缺少音色 id");
        return;
    }
    if (voice_id == CurrentVoiceId()) {
        VoiceOperation select =
            voice_tts::SelectKnownVoice(voice_tts::kBuiltinVoices[0].id, []() { return false; });
        if (select != VoiceOperation::kOk) {
            SendError(id, VoiceErrorCode(select), "无法切回默认音色");
            return;
        }
    }
    VoiceOperation result = voice_tts::DeleteManagedVoice(voice_id);
    if (result != VoiceOperation::kOk) {
        SendError(id, VoiceErrorCode(result), "删除失败");
        return;
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "id", voice_id.c_str());
    SendResponse(id, out);
}

void BleService::HandleVoiceRecord(int id, cJSON* data) {
    std::string name = JsonString(data, "name");
    if (!voice_tts::IsValidVoiceName(name)) {
        SendError(id, 1003, "音色名称不合法");
        return;
    }
    auto& app = Application::GetInstance();
    if (VoiceManagementBusy() || app.IsVoiceEnrollmentActive() ||
        app.IsExternalVoicePreviewActive()) {
        SendError(id, 1010, "声音服务正在处理，请稍后重试");
        return;
    }
    if (!app.RequestDeviceVoiceEnrollment(name)) {
        SendError(id, 1010, "设备当前无法录音");
        return;
    }
    auto* args = new VoiceWatchArgs{this, name};
    if (xTaskCreate(
            [](void* arg) {
                VoiceWatchTask(arg);
                vTaskDelete(nullptr);
            },
            "wzh_ble_voice", 3072, args, 3, nullptr) != pdPASS) {
        delete args;
        ESP_LOGW(kTag, "录音状态任务创建失败");
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "accepted", 1);
    cJSON_AddStringToObject(out, "name", name.c_str());
    SendResponse(id, out);
}

void BleService::HandleUploadBegin(int id, cJSON* data) {
    std::string name = JsonString(data, "name");
    int bytes = JsonInt(data, "bytes", 0);
    if (!voice_tts::IsValidVoiceName(name)) {
        SendError(id, 1003, "音色名称不合法");
        return;
    }
    if (bytes < kMinPcmBytes || bytes > kMaxPcmBytes || (bytes % 2) != 0) {
        SendError(id, 1003, "音频长度需为 5 到 20 秒的 16k 单声道 PCM");
        return;
    }
    if (VoiceManagementBusy()) {
        SendError(id, 1010, "声音服务正在处理，请稍后重试");
        return;
    }

    std::lock_guard<std::mutex> lock(upload_mutex_);
    if (upload_buffer_ != nullptr) {
        SendError(id, 1010, "已有上传任务进行中");
        return;
    }
    size_t size = 1 + name.size() + static_cast<size_t>(bytes);
    uint8_t* buffer =
        static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr)
        buffer = static_cast<uint8_t*>(malloc(size));
    if (buffer == nullptr) {
        SendError(id, 1000, "设备内存不足");
        return;
    }
    buffer[0] = static_cast<uint8_t>(name.size());
    memcpy(buffer + 1, name.data(), name.size());
    upload_buffer_ = buffer;
    upload_size_ = size;
    upload_total_ = static_cast<size_t>(bytes);
    upload_offset_ = 0;
    upload_name_ = name;

    cJSON* out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "bytes", bytes);
    cJSON_AddNumberToObject(out, "chunkLimit", kMaxChunkBytes);
    SendResponse(id, out);
}

void BleService::HandleUploadChunk(int id, cJSON* data) {
    std::string encoded = JsonString(data, "data");
    if (encoded.empty()) {
        SendError(id, 1003, "缺少音频数据");
        return;
    }
    std::vector<uint8_t> chunk;
    if (!Base64Decode(encoded, chunk) || chunk.empty()) {
        SendError(id, 1003, "音频数据格式错误");
        return;
    }
    if (chunk.size() > kMaxChunkBytes) {
        SendError(id, 1003, "单块数据过大");
        return;
    }

    std::lock_guard<std::mutex> lock(upload_mutex_);
    if (upload_buffer_ == nullptr) {
        SendError(id, 1003, "尚未开始上传");
        return;
    }
    if (upload_offset_ + chunk.size() > upload_total_) {
        SendError(id, 1003, "音频数据超出声明长度");
        return;
    }
    size_t header = 1 + upload_name_.size();
    memcpy(upload_buffer_ + header + upload_offset_, chunk.data(), chunk.size());
    upload_offset_ += chunk.size();

    cJSON* out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "received", upload_offset_);
    cJSON_AddNumberToObject(out, "total", upload_total_);
    SendResponse(id, out);
}

void BleService::HandleUploadEnd(int id) {
    uint8_t* buffer = nullptr;
    size_t size = 0;
    {
        std::lock_guard<std::mutex> lock(upload_mutex_);
        if (upload_buffer_ == nullptr) {
            SendError(id, 1003, "尚未开始上传");
            return;
        }
        if (upload_offset_ != upload_total_) {
            SendError(id, 1003, "音频数据不完整");
            return;
        }
        buffer = upload_buffer_;
        size = upload_size_;
        upload_buffer_ = nullptr;
        upload_size_ = 0;
        upload_total_ = 0;
        upload_offset_ = 0;
        upload_name_.clear();
    }

    bool accepted = VoiceManagement().Upload(buffer, size);
    if (!accepted) {
        free(buffer);
        SendError(id, 1010, "声音服务正在处理，请稍后重试");
        return;
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "accepted", 1);
    SendResponse(id, out);
}

void BleService::HandleUploadCancel(int id) {
    ReleaseUpload();
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "cancelled", 1);
    SendResponse(id, out);
}

void BleService::HandleWifiScan(int id) {
    std::vector<wifi_ap_record_t> records;
    esp_err_t err = WifiManager::GetInstance().ScanNetworks(records, 12000);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Wi-Fi 扫描失败: %s", esp_err_to_name(err));
        SendError(id, 1005, "Wi-Fi 扫描失败，请稍后重试");
        return;
    }

    std::vector<wifi_ap_record_t> unique;
    unique.reserve(records.size());
    for (const auto& record : records) {
        if (record.ssid[0] == '\0')
            continue;
        std::string ssid(reinterpret_cast<const char*>(record.ssid));
        auto existing =
            std::find_if(unique.begin(), unique.end(), [&ssid](const wifi_ap_record_t& item) {
                return ssid == reinterpret_cast<const char*>(item.ssid);
            });
        if (existing == unique.end()) {
            unique.push_back(record);
        } else if (record.rssi > existing->rssi) {
            *existing = record;
        }
    }
    std::sort(unique.begin(), unique.end(),
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });

    std::string current = WifiManager::GetInstance().GetSsid();
    cJSON* networks = cJSON_CreateArray();
    for (const auto& record : unique) {
        std::string ssid(reinterpret_cast<const char*>(record.ssid));
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", ssid.c_str());
        cJSON_AddNumberToObject(item, "rssi", record.rssi);
        cJSON_AddStringToObject(item, "band", BandText(record.primary).c_str());
        cJSON_AddBoolToObject(item, "secure", record.authmode != WIFI_AUTH_OPEN);
        cJSON_AddBoolToObject(item, "current", ssid == current);
        cJSON_AddNumberToObject(item, "bars", SignalBars(record.rssi));
        cJSON_AddItemToArray(networks, item);
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "networks", networks);
    SendResponse(id, out);
}

void BleService::HandleWifiStatus(int id) {
    auto& manager = WifiManager::GetInstance();
    int channel = manager.GetChannel();
    int rssi = manager.GetRssi();
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "connected", manager.IsConnected());
    cJSON_AddStringToObject(out, "ssid", manager.GetSsid().c_str());
    cJSON_AddStringToObject(out, "ip", manager.GetIpAddress().c_str());
    cJSON_AddNumberToObject(out, "rssi", rssi);
    cJSON_AddStringToObject(out, "band", BandText(channel).c_str());
    cJSON_AddStringToObject(out, "signal", SignalText(rssi));
    cJSON_AddNumberToObject(out, "bars", SignalBars(rssi));
    SendResponse(id, out);
}

void BleService::HandleWifiConnect(int id, cJSON* data) {
    std::string ssid = JsonString(data, "ssid");
    std::string password = JsonString(data, "password");
    if (ssid.empty() || ssid.size() > 32) {
        SendError(id, 1005, "Wi-Fi 名称不合法");
        return;
    }
    if (!password.empty() && (password.size() < 8 || password.size() > 63)) {
        SendError(id, 1005, "Wi-Fi 密码需为 8 到 63 位");
        return;
    }
    if (!WifiManager::GetInstance().ConnectTo(ssid, password)) {
        SendError(id, 1005, "Wi-Fi 切换失败，请稍后重试");
        return;
    }

    auto* args = new WifiWatchArgs{this, ssid};
    if (xTaskCreate(
            [](void* arg) {
                WifiWatchTask(arg);
                vTaskDelete(nullptr);
            },
            "wzh_ble_wifi", 3072, args, 3, nullptr) != pdPASS) {
        delete args;
        ESP_LOGW(kTag, "Wi-Fi 状态任务创建失败");
    }

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "accepted", 1);
    cJSON_AddStringToObject(out, "ssid", ssid.c_str());
    SendResponse(id, out);
}

void BleService::HandleResetFactory(int id) {
    if (config_.console == nullptr) {
        SendError(id, 1011, "恢复出厂服务不可用");
        return;
    }
    std::string code;
    std::string message;
    if (!config_.console->StartFactoryReset(code, message)) {
        int mapped = 1011;
        if (code == "NET_OFFLINE")
            mapped = 1005;
        else if (code == "VOICE_NO_KEY")
            mapped = 1014;
        else if (code == "RESET_BUSY" || code == "VOICE_BUSY")
            mapped = 1010;
        SendError(id, mapped, message.empty() ? "恢复出厂失败" : message.c_str());
        return;
    }
    if (xTaskCreate(
            [](void* arg) {
                ResetWatchTask(arg);
                vTaskDelete(nullptr);
            },
            "wzh_ble_reset", 4096, this, 3, nullptr) != pdPASS) {
        ESP_LOGW(kTag, "恢复出厂状态任务创建失败");
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "accepted", 1);
    SendResponse(id, out);
}

cJSON* BleService::BuildDeviceObject() {
    cJSON* device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "product", config_.product_name.c_str());
    cJSON_AddStringToObject(device, "productId", config_.product_id.c_str());
    cJSON_AddStringToObject(device, "name", adv_name_.c_str());
    cJSON_AddStringToObject(device, "model", config_.model.c_str());
    const esp_app_desc_t* desc = esp_app_get_description();
    cJSON_AddStringToObject(device, "firmware", desc != nullptr ? desc->version : "");
    cJSON* fixed = cJSON_CreateArray();
    for (size_t i = 0; i < voice_tts::kBuiltinVoiceCount; ++i) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", voice_tts::kBuiltinVoices[i].id);
        cJSON_AddStringToObject(item, "name", voice_tts::kBuiltinVoices[i].name);
        cJSON_AddStringToObject(item, "description", voice_tts::kBuiltinVoices[i].description);
        cJSON_AddItemToArray(fixed, item);
    }
    cJSON_AddItemToObject(device, "fixedVoices", fixed);

    std::vector<voice_tts::VoiceName> names;
    voice_tts::LoadVoiceNames(names);
    cJSON* clones = cJSON_CreateArray();
    for (const auto& voice : names) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", voice.id.c_str());
        cJSON_AddStringToObject(item, "name", voice.name.c_str());
        cJSON_AddItemToArray(clones, item);
    }
    cJSON_AddItemToObject(device, "clones", clones);
    cJSON_AddNumberToObject(device, "voiceCapacity", kVoiceCapacity);
    return device;
}

cJSON* BleService::BuildStatusObject() {
    cJSON* status = cJSON_CreateObject();
    if (config_.console != nullptr) {
        config_.console->BuildStatus(status);
        // The voice list travels over `voice_list`; keep the periodic status
        // frame small for BLE MTU and JSON parse cost.
        cJSON_DeleteItemFromObject(status, "voice_catalog");
    } else {
        cJSON_AddStringToObject(status, "state",
                                StateName(Application::GetInstance().GetDeviceState()));
    }

    auto& manager = WifiManager::GetInstance();
    bool connected = manager.IsConnected();
    int rssi = manager.GetRssi();
    int channel = manager.GetChannel();
    std::string ssid = manager.GetSsid();

    cJSON* wifi = cJSON_GetObjectItem(status, "wifi");
    if (wifi == nullptr || !cJSON_IsObject(wifi)) {
        wifi = cJSON_CreateObject();
        cJSON_AddItemToObject(status, "wifi", wifi);
    }
    cJSON_AddBoolToObject(wifi, "connected", connected);
    cJSON_AddStringToObject(wifi, "ssid", ssid.c_str());
    cJSON_AddNumberToObject(wifi, "rssi", rssi);
    cJSON_AddStringToObject(wifi, "band", BandText(channel).c_str());
    cJSON_AddStringToObject(wifi, "signal", SignalText(rssi));
    cJSON_AddNumberToObject(wifi, "bars", SignalBars(rssi));
    cJSON_AddNumberToObject(wifi, "latency", 0);

    int battery = 0;
    bool charging = false;
    cJSON* battery_item = cJSON_GetObjectItem(status, "battery");
    if (battery_item != nullptr && cJSON_IsObject(battery_item)) {
        battery = JsonInt(battery_item, "level", 0);
        cJSON* charging_item = cJSON_GetObjectItem(battery_item, "charging");
        charging = cJSON_IsTrue(charging_item);
        cJSON_DeleteItemFromObject(status, "battery");
    }
    cJSON_AddNumberToObject(status, "battery", battery);
    cJSON_AddBoolToObject(status, "charging", charging);

    double temperature = 0;
    cJSON* temperature_item = cJSON_GetObjectItem(status, "temperature_c");
    if (cJSON_IsNumber(temperature_item))
        temperature = temperature_item->valuedouble;
    cJSON_AddNumberToObject(status, "temp", temperature);
    cJSON_AddStringToObject(status, "tempState", temperature >= 55 ? "偏高" : "正常");

    int volume = 0;
    cJSON* audio = cJSON_GetObjectItem(status, "audio");
    if (audio != nullptr && cJSON_IsObject(audio)) {
        volume = JsonInt(audio, "volume", 0);
    }
    cJSON_AddNumberToObject(status, "volume", volume);

    int uptime = JsonInt(status, "uptime_s", 0);
    int runtime_minutes = uptime / 60;
    cJSON_AddNumberToObject(status, "runtimeMinutes", runtime_minutes);
    cJSON_AddStringToObject(status, "runtime", RuntimeText(runtime_minutes).c_str());

    cJSON_AddBoolToObject(status, "online", connected);
    cJSON_AddStringToObject(status, "ssid", ssid.c_str());
    cJSON_AddStringToObject(status, "band", BandText(channel).c_str());
    cJSON_AddStringToObject(status, "signal", SignalText(rssi));
    cJSON_AddNumberToObject(status, "rssi", rssi);
    cJSON_AddNumberToObject(status, "latency", 0);

    std::string voice_id = CurrentVoiceId();
    cJSON_AddStringToObject(status, "currentVoiceId", voice_id.c_str());
    cJSON_AddStringToObject(status, "currentVoice", CurrentVoiceName().c_str());

    std::vector<voice_tts::VoiceName> names;
    voice_tts::LoadVoiceNames(names);
    cJSON_AddNumberToObject(status, "voiceCount",
                            static_cast<int>(voice_tts::kBuiltinVoiceCount + names.size()));
    cJSON_AddNumberToObject(status, "voiceCapacity", kVoiceCapacity);
    return status;
}

cJSON* BleService::BuildVoiceList() {
    cJSON* root = cJSON_CreateObject();
    cJSON* voices = cJSON_CreateArray();
    for (size_t i = 0; i < voice_tts::kBuiltinVoiceCount; ++i) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", voice_tts::kBuiltinVoices[i].id);
        cJSON_AddStringToObject(item, "name", voice_tts::kBuiltinVoices[i].name);
        cJSON_AddStringToObject(item, "description", voice_tts::kBuiltinVoices[i].description);
        cJSON_AddBoolToObject(item, "builtin", 1);
        cJSON_AddItemToArray(voices, item);
    }
    std::vector<voice_tts::VoiceName> names;
    voice_tts::LoadVoiceNames(names);
    for (const auto& voice : names) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", voice.id.c_str());
        cJSON_AddStringToObject(item, "name", voice.name.c_str());
        cJSON_AddStringToObject(item, "description", "");
        cJSON_AddBoolToObject(item, "builtin", 0);
        cJSON_AddItemToArray(voices, item);
    }
    cJSON_AddItemToObject(root, "voices", voices);
    std::string current = CurrentVoiceId();
    cJSON_AddStringToObject(root, "currentVoiceId", current.c_str());
    cJSON_AddStringToObject(root, "currentVoice", CurrentVoiceName().c_str());
    return root;
}

std::string BleService::CurrentVoiceId() {
    std::string id = voice_tts::LoadAliVoice();
    if (id.empty() && voice_tts::kBuiltinVoiceCount > 0)
        return voice_tts::kBuiltinVoices[0].id;
    return id;
}

std::string BleService::CurrentVoiceName() {
    std::string id = CurrentVoiceId();
    for (size_t i = 0; i < voice_tts::kBuiltinVoiceCount; ++i) {
        if (id == voice_tts::kBuiltinVoices[i].id) {
            return voice_tts::kBuiltinVoices[i].name;
        }
    }
    std::vector<voice_tts::VoiceName> names;
    voice_tts::LoadVoiceNames(names);
    for (const auto& voice : names) {
        if (voice.id == id)
            return voice.name;
    }
    return id;
}

bool BleService::ResolveVoice(cJSON* data, std::string& id, std::string& name) {
    if (data == nullptr)
        return false;
    id = JsonString(data, "id");
    name = JsonString(data, "name");

    if (!id.empty()) {
        for (size_t i = 0; i < voice_tts::kBuiltinVoiceCount; ++i) {
            if (id == voice_tts::kBuiltinVoices[i].id) {
                name = voice_tts::kBuiltinVoices[i].name;
                return true;
            }
        }
        std::vector<voice_tts::VoiceName> names;
        voice_tts::LoadVoiceNames(names);
        for (const auto& voice : names) {
            if (voice.id == id) {
                name = voice.name;
                return true;
            }
        }
        return false;
    }

    if (name.empty())
        return false;
    for (size_t i = 0; i < voice_tts::kBuiltinVoiceCount; ++i) {
        if (name == voice_tts::kBuiltinVoices[i].name) {
            id = voice_tts::kBuiltinVoices[i].id;
            return true;
        }
    }
    std::vector<voice_tts::VoiceName> names;
    voice_tts::LoadVoiceNames(names);
    for (const auto& voice : names) {
        if (voice.name == name) {
            id = voice.id;
            return true;
        }
    }
    return false;
}

void BleService::WifiWatchTask(void* arg) {
    auto* args = static_cast<WifiWatchArgs*>(arg);
    auto* self = args->service;
    std::string ssid = args->ssid;
    delete args;

    cJSON* connecting = cJSON_CreateObject();
    cJSON_AddStringToObject(connecting, "state", "connecting");
    cJSON_AddStringToObject(connecting, "ssid", ssid.c_str());
    self->SendEvent("wifi_state", connecting);

    for (int i = 0; i < 40; ++i) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!self->started_.load())
            return;
        auto& manager = WifiManager::GetInstance();
        if (manager.IsConnected() && manager.GetSsid() == ssid) {
            cJSON* connected = cJSON_CreateObject();
            cJSON_AddStringToObject(connected, "state", "connected");
            cJSON_AddStringToObject(connected, "ssid", ssid.c_str());
            self->SendEvent("wifi_state", connected);
            return;
        }
    }

    cJSON* failed = cJSON_CreateObject();
    cJSON_AddStringToObject(failed, "state", "failed");
    cJSON_AddStringToObject(failed, "ssid", ssid.c_str());
    cJSON_AddStringToObject(failed, "message", "连接超时，请检查密码或信号");
    self->SendEvent("wifi_state", failed);
}

void BleService::VoiceWatchTask(void* arg) {
    auto* args = static_cast<VoiceWatchArgs*>(arg);
    auto* self = args->service;
    std::string name = args->name;
    delete args;
    std::string last;
    for (int i = 0; i < 90; ++i) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!self->started_.load())
            return;
        std::string status = Application::GetInstance().DeviceVoiceEnrollmentStatus();
        if (status != last) {
            last = status;
            cJSON* data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "status", status.c_str());
            cJSON_AddStringToObject(data, "name", name.c_str());
            if (status == "failed") {
                const char* reason = Application::GetInstance().DeviceVoiceEnrollmentFailure();
                if (reason != nullptr && reason[0] != '\0')
                    cJSON_AddStringToObject(data, "reason", reason);
            }
            self->SendEvent("voice_record", data);
        }
        if (status == "success" || status == "failed" || status == "cancelled") {
            return;
        }
    }
}

void BleService::ResetWatchTask(void* arg) {
    auto* self = static_cast<BleService*>(arg);
    std::string last;
    for (int i = 0; i < 240; ++i) {
        if (!self->started_.load() || self->config_.console == nullptr)
            return;
        cJSON* status = cJSON_CreateObject();
        self->config_.console->FactoryResetStatus(status);
        std::string phase = JsonString(status, "phase");
        if (phase != last) {
            last = phase;
            cJSON* data = cJSON_CreateObject();
            cJSON* phase_item = cJSON_GetObjectItem(status, "phase");
            cJSON* current_item = cJSON_GetObjectItem(status, "current");
            cJSON* total_item = cJSON_GetObjectItem(status, "total");
            cJSON* error_item = cJSON_GetObjectItem(status, "error");
            if (phase_item != nullptr) {
                cJSON_AddItemToObject(data, "phase", cJSON_Duplicate(phase_item, 1));
            }
            if (current_item != nullptr) {
                cJSON_AddItemToObject(data, "current", cJSON_Duplicate(current_item, 1));
            }
            if (total_item != nullptr) {
                cJSON_AddItemToObject(data, "total", cJSON_Duplicate(total_item, 1));
            }
            if (error_item != nullptr) {
                cJSON_AddItemToObject(data, "error", cJSON_Duplicate(error_item, 1));
            }
            self->SendEvent("reset", data);
        }
        cJSON_Delete(status);
        if (phase == "failed" || phase == "rebooting")
            return;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

}  // namespace wzh
