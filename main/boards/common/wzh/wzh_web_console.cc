#include "wzh_web_console.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "wifi_manager.h"
#include "wzh_flash.h"

#define TAG "WzhWebConsole"

extern const char wzh_console_html_start[] asm("_binary_wzh_console_html_start");
extern const char wzh_console_html_end[] asm("_binary_wzh_console_html_end");

namespace wzh {
namespace {

constexpr int kConsolePort = 8080;
constexpr int kConsoleCtrlPort = 32769;
constexpr size_t kMaxBody = 1024;
constexpr size_t kMaxUpdateFiles = 32;

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
        case kDeviceStateActivating:
            return "activating";
        case kDeviceStateUpgrading:
            return "upgrading";
        case kDeviceStateAudioTesting:
            return "audio_testing";
        case kDeviceStateFatalError:
            return "fatal_error";
        default:
            return "unknown";
    }
}

const char* LayoutName(AudioStore::Layout layout) {
    switch (layout) {
        case AudioStore::Layout::kV3:
            return "v3";
        case AudioStore::Layout::kLegacy:
            return "v1/v2";
        default:
            return "empty";
    }
}

const char* HttpStatusText(int status) {
    switch (status) {
        case 202:
            return "202 Accepted";
        case 400:
            return "400 Bad Request";
        case 403:
            return "403 Forbidden";
        case 404:
            return "404 Not Found";
        case 405:
            return "405 Method Not Allowed";
        case 409:
            return "409 Conflict";
        default:
            return "500 Internal Server Error";
    }
}

}  // namespace

WebConsole::~WebConsole() { Stop(); }

void WebConsole::Configure(Options options) { options_ = std::move(options); }

bool WebConsole::Start() {
    if (started_.exchange(true)) {
        return true;
    }
    if (xTaskCreate(StartTaskEntry, "wzh_console", 6144, this, 3, nullptr) != pdPASS) {
        started_.store(false);
        ESP_LOGE(TAG, "无法启动管理页任务");
        return false;
    }
    return true;
}

void WebConsole::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    started_.store(false);
    StopHttpServer();
    developer_.Lock();
    DestroyAp();
}

void WebConsole::StartTaskEntry(void* arg) {
    static_cast<WebConsole*>(arg)->StartTask();
    vTaskDelete(nullptr);
}

void WebConsole::StartTask() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!started_.load() || options_.store == nullptr) {
        started_.store(false);
        return;
    }
    if (WifiManager::GetInstance().IsConfigMode()) {
        // The provisioning portal owns WIFI_AP_DEF; retry on the next Connected event.
        started_.store(false);
        return;
    }
    if (!EnsureAp()) {
        started_.store(false);
        return;
    }
    if (options_.hub != nullptr) {
        // NVS/Wi-Fi are initialized by now; load server/token for the status page.
        options_.hub->EnsureInitialized();
    }
    if (!StartHttpServer()) {
        DestroyAp();
        started_.store(false);
        return;
    }
    ESP_LOGI(TAG, "管理页 http://192.168.4.1:%d/  热点=%s 密码=%s", kConsolePort, ap_ssid_.c_str(),
             ap_password_.c_str());
}

bool WebConsole::EnsureAp() {
    if (ap_netif_ != nullptr) {
        return true;
    }
    ap_netif_ = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif_ == nullptr) {
        ap_netif_ = esp_netif_create_default_wifi_ap();
    }
    if (ap_netif_ == nullptr) {
        ESP_LOGE(TAG, "无法创建 AP 网络接口");
        return false;
    }

    esp_netif_ip_info_t ip = {};
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif_);
    if (esp_netif_set_ip_info(ap_netif_, &ip) != ESP_OK) {
        ESP_LOGE(TAG, "设置管理热点 IP 失败");
        DestroyAp();
        return false;
    }
    esp_netif_dhcps_start(ap_netif_);

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        ESP_LOGW(TAG, "读取 SoftAP MAC 失败，使用默认后缀");
    }
    char suffix[5];
    snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
    const std::string prefix = options_.ap_prefix.empty() ? "ZYLZ" : options_.ap_prefix;
    ap_ssid_ = prefix + "-" + suffix;
    // Test-version password, logged for the operator; replace with a per-device
    // secret before shipping. 8 characters satisfies WPA2-PSK.
    ap_password_ = std::string("zylz") + suffix;

    wifi_config_t config = {};
    snprintf(reinterpret_cast<char*>(config.ap.ssid), sizeof(config.ap.ssid), "%s",
             ap_ssid_.c_str());
    config.ap.ssid_len = static_cast<uint8_t>(ap_ssid_.size());
    config.ap.channel = 0;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    snprintf(reinterpret_cast<char*>(config.ap.password), sizeof(config.ap.password), "%s",
             ap_password_.c_str());

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
        // The station is already running; some IDF versions report
        // ESP_ERR_WIFI_STATE for a redundant start. The AP config is applied.
        if (err == ESP_ERR_WIFI_STATE) {
            err = ESP_OK;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动管理热点失败: %s", esp_err_to_name(err));
        DestroyAp();
        return false;
    }
    return true;
}

void WebConsole::DestroyAp() {
    if (ap_netif_ == nullptr) {
        return;
    }
    esp_netif_dhcps_stop(ap_netif_);
    esp_netif_destroy_default_wifi(ap_netif_);
    ap_netif_ = nullptr;
}

bool WebConsole::StartHttpServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kConsolePort;
    config.ctrl_port = kConsoleCtrlPort;
    config.max_uri_handlers = 24;
    config.recv_wait_timeout = 3;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "启动管理页 HTTP 服务失败");
        server_ = nullptr;
        return false;
    }

    auto add = [this](const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
        httpd_uri_t entry = {};
        entry.uri = uri;
        entry.method = method;
        entry.handler = handler;
        entry.user_ctx = this;
        return httpd_register_uri_handler(server_, &entry) == ESP_OK;
    };

    const bool ok = add("/", HTTP_GET, HandleRoot) && add("/api/status", HTTP_GET, HandleStatus) &&
                    add("/api/audio", HTTP_GET, HandleAudio) &&
                    add("/api/audio/play", HTTP_POST, HandlePlay) &&
                    add("/api/audio/stop", HTTP_POST, HandleStop) &&
                    add("/api/chat/toggle", HTTP_POST, HandleChat) &&
                    add("/api/hub/status", HTTP_GET, HandleHubStatus) &&
                    add("/api/hub/check", HTTP_POST, HandleHubCheck) &&
                    add("/api/hub/diff", HTTP_GET, HandleHubDiff) &&
                    add("/api/hub/update", HTTP_POST, HandleHubUpdate) &&
                    add("/api/developer/login", HTTP_POST, HandleDeveloperLogin) &&
                    add("/api/developer/logout", HTTP_POST, HandleDeveloperLogout) &&
                    add("/api/developer/status", HTTP_GET, HandleDeveloperStatus) &&
                    add("/api/developer/volume", HTTP_POST, HandleDeveloperVolume) &&
                    add("/api/developer/center", HTTP_POST, HandleDeveloperCenter) &&
                    add("/api/developer/demo", HTTP_POST, HandleDeveloperDemo) &&
                    add("/api/developer/flash/erase", HTTP_POST, HandleDeveloperErase);
    if (!ok) {
        ESP_LOGE(TAG, "注册管理页路由失败");
        httpd_stop(server_);
        server_ = nullptr;
    }
    return ok;
}

void WebConsole::StopHttpServer() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

bool WebConsole::ReadJsonBody(httpd_req_t* req, cJSON** root) {
    *root = nullptr;
    if (req->content_len <= 0 || req->content_len > static_cast<int>(kMaxBody)) {
        return false;
    }
    std::string body(static_cast<size_t>(req->content_len), '\0');
    size_t received = 0;
    const int64_t deadline = esp_timer_get_time() + 6000000;
    while (received < body.size()) {
        if (esp_timer_get_time() > deadline)
            return false;
        const int count = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (count <= 0)
            return false;
        received += count;
    }
    const char* end = nullptr;
    *root = cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, &end, true);
    if (!cJSON_IsObject(*root) || end != body.c_str() + body.size()) {
        cJSON_Delete(*root);
        *root = nullptr;
        return false;
    }
    return true;
}

esp_err_t WebConsole::SendJson(httpd_req_t* req, cJSON* root, const char* status) {
    char* text = cJSON_PrintUnformatted(root);
    if (text == nullptr) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (status != nullptr) {
        httpd_resp_set_status(req, status);
    }
    const esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

esp_err_t WebConsole::SendError(httpd_req_t* req, int status, const char* code) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", code);
    const esp_err_t err = SendJson(req, root, HttpStatusText(status));
    cJSON_Delete(root);
    return err;
}

void WebConsole::AddStatus(cJSON* root) {
    auto& app = Application::GetInstance();
    cJSON_AddStringToObject(root, "product", options_.product_name.c_str());
    cJSON_AddStringToObject(root, "build", options_.build_name.c_str());
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(root, "uptime_s",
                            static_cast<double>(esp_timer_get_time()) / 1000000.0);
    cJSON_AddStringToObject(root, "state", StateName(app.GetDeviceState()));

    int level = 0;
    bool charging = false;
    bool discharging = false;
    cJSON* battery = cJSON_AddObjectToObject(root, "battery");
    if (battery != nullptr) {
        const bool available = Board::GetInstance().GetBatteryLevel(level, charging, discharging);
        cJSON_AddBoolToObject(battery, "available", available);
        if (available) {
            cJSON_AddNumberToObject(battery, "level", level);
            cJSON_AddBoolToObject(battery, "charging", charging);
            cJSON_AddBoolToObject(battery, "discharging", discharging);
        }
    }
    float temperature = 0.0f;
    if (Board::GetInstance().GetTemperature(temperature)) {
        cJSON_AddNumberToObject(root, "temperature_c", temperature);
    }

    auto& wifi = WifiManager::GetInstance();
    cJSON* wifi_json = cJSON_AddObjectToObject(root, "wifi");
    if (wifi_json != nullptr) {
        cJSON_AddBoolToObject(wifi_json, "connected", wifi.IsConnected());
        cJSON_AddStringToObject(wifi_json, "ssid", wifi.GetSsid().c_str());
        cJSON_AddStringToObject(wifi_json, "ip", wifi.GetIpAddress().c_str());
        cJSON_AddNumberToObject(wifi_json, "rssi", wifi.GetRssi());
        cJSON_AddStringToObject(wifi_json, "ap_ssid", ap_ssid_.c_str());
        cJSON_AddStringToObject(wifi_json, "ap_ip", "192.168.4.1");
        cJSON_AddNumberToObject(wifi_json, "ap_port", kConsolePort);
    }

    if (options_.store != nullptr) {
        auto* store = options_.store;
        cJSON* flash = cJSON_AddObjectToObject(root, "flash");
        if (flash != nullptr) {
            cJSON_AddStringToObject(flash, "type", wzh_flash_type_name());
            cJSON_AddStringToObject(flash, "layout", LayoutName(store->layout()));
            cJSON_AddNumberToObject(flash, "files", store->file_count());
            cJSON_AddNumberToObject(flash, "used", static_cast<double>(store->used_bytes()));
            cJSON_AddNumberToObject(flash, "free", static_cast<double>(store->free_bytes()));
            cJSON_AddNumberToObject(flash, "capacity", static_cast<double>(store->capacity()));
            cJSON_AddBoolToObject(flash, "writing", store->IsWriting());
        }
    }

    cJSON* audio = cJSON_AddObjectToObject(root, "audio");
    if (audio != nullptr && options_.player != nullptr) {
        const auto motion = options_.player->GetMotionData();
        cJSON_AddBoolToObject(audio, "playing", options_.player->Playing());
        cJSON_AddNumberToObject(audio, "elapsed_ms", motion.elapsed_ms);
        cJSON_AddNumberToObject(audio, "duration_ms", motion.duration_ms);
        cJSON_AddStringToObject(audio, "current", options_.player->CurrentName().c_str());
        cJSON_AddNumberToObject(audio, "index", motion.sound_index);
        cJSON_AddNumberToObject(audio, "level", options_.player->GetTtsMotionData().level);
        cJSON_AddNumberToObject(audio, "attack", options_.player->GetTtsMotionData().attack);
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (audio != nullptr && codec != nullptr) {
        cJSON_AddNumberToObject(audio, "volume", codec->output_volume());
        cJSON_AddBoolToObject(audio, "wake_running", app.GetAudioService().IsWakeWordRunning());
        cJSON_AddBoolToObject(audio, "input_enabled", codec->input_enabled());
        cJSON_AddBoolToObject(audio, "speaker_enabled", codec->output_enabled());
        cJSON_AddBoolToObject(audio, "capture_running",
                              app.GetAudioService().IsAudioProcessorRunning());
        cJSON_AddBoolToObject(audio, "playback_idle", app.GetAudioService().IsPlaybackIdle());
        const auto buffer = app.GetAudioService().GetPlaybackBufferStatus();
        cJSON_AddNumberToObject(audio, "decode_packets", buffer.decode_packets);
        cJSON_AddNumberToObject(audio, "pcm_frames", buffer.pcm_frames);
        cJSON_AddNumberToObject(audio, "buffered_ms", buffer.buffered_ms);
        cJSON_AddNumberToObject(audio, "rebuffer_count", buffer.rebuffer_count);
        cJSON_AddNumberToObject(audio, "queue_full_count", buffer.queue_full_count);
    }
    cJSON_AddBoolToObject(root, "maintenance", maintenance_active_.load());
    AddHubStatus(root);

    if (options_.status_extra) {
        options_.status_extra(root);
    }
}

void WebConsole::AddHubStatus(cJSON* root) {
    cJSON* hub_json = cJSON_AddObjectToObject(root, "hub");
    if (hub_json == nullptr) {
        return;
    }
    auto* hub = options_.hub;
    if (hub == nullptr) {
        cJSON_AddBoolToObject(hub_json, "available", false);
        return;
    }
    cJSON_AddBoolToObject(hub_json, "available", true);
    cJSON_AddStringToObject(hub_json, "status", AudioHub::StatusName(hub->status()));
    cJSON_AddBoolToObject(hub_json, "manual", hub->manual());
    cJSON_AddBoolToObject(hub_json, "activated", hub->activated());
    cJSON_AddStringToObject(hub_json, "device_id", hub->device_id().c_str());
    cJSON_AddStringToObject(hub_json, "revision", hub->revision().c_str());
    cJSON_AddStringToObject(hub_json, "activation_code",
                            hub->activated() ? "" : hub->activation_code().c_str());
    cJSON_AddBoolToObject(hub_json, "job_running", hub->job_running());
    cJSON_AddBoolToObject(hub_json, "job_ok", hub->job_ok());
    cJSON_AddStringToObject(hub_json, "job_message", hub->job_message().c_str());
    cJSON_AddBoolToObject(hub_json, "has_diff", hub->has_diff());
    cJSON_AddStringToObject(hub_json, "checked_revision", hub->checked_revision().c_str());
}

esp_err_t WebConsole::HandleRoot(httpd_req_t* req) {
    const size_t length = static_cast<size_t>(wzh_console_html_end - wzh_console_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, wzh_console_html_start, length);
}

esp_err_t WebConsole::HandleStatus(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_500(req);
    }
    self->AddStatus(root);
    const esp_err_t err = SendJson(req, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t WebConsole::HandleAudio(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (self->options_.store == nullptr) {
        return SendError(req, 500, "flash_unavailable");
    }
    auto* store = self->options_.store;
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(root, "flash_type", wzh_flash_type_name());
    cJSON_AddStringToObject(root, "layout", LayoutName(store->layout()));
    cJSON_AddNumberToObject(root, "used", static_cast<double>(store->used_bytes()));
    cJSON_AddNumberToObject(root, "free", static_cast<double>(store->free_bytes()));
    cJSON_AddBoolToObject(root, "writing", store->IsWriting());
    cJSON* pairs = cJSON_AddArrayToObject(root, "paired");
    if (pairs) {
        for (size_t i = 0; i < user_pair_count; ++i) {
            const auto& pair = user_pairs[i];
            cJSON* item = cJSON_CreateObject();
            if (!item)
                continue;
            cJSON_AddNumberToObject(item, "index", kUserPairIndexBase + i);
            cJSON_AddStringToObject(item, "name", pair.name);
            cJSON_AddStringToObject(item, "category", "animal");
            cJSON_AddBoolToObject(item, "paired", true);
            cJSON_AddStringToObject(item, "source", "firmware_user_pair");
            cJSON_AddNumberToObject(item, "size", pair.audio_size);
            cJSON_AddNumberToObject(item, "duration_ms", pair.audio_ms);
            cJSON_AddNumberToObject(item, "motion_ms", pair.motion_ms);
            cJSON_AddNumberToObject(item, "motion_frames", pair.frame_count);
            cJSON_AddItemToArray(pairs, item);
        }
    }
    cJSON* files = cJSON_AddArrayToObject(root, "files");
    if (files != nullptr) {
        const auto snapshot = store->Snapshot();
        for (size_t i = 0; i < snapshot.size(); ++i) {
            const auto& file = snapshot[i];
            cJSON* item = cJSON_CreateObject();
            if (item == nullptr) {
                continue;
            }
            cJSON_AddNumberToObject(item, "index", static_cast<double>(i));
            cJSON_AddStringToObject(item, "name", file.name);
            cJSON_AddStringToObject(item, "category", file.category);
            cJSON_AddNumberToObject(item, "size", file.size);
            cJSON_AddNumberToObject(item, "duration_ms", file.duration_ms);
            cJSON_AddItemToArray(files, item);
        }
    }
    const esp_err_t err = SendJson(req, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t WebConsole::HandlePlay(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (self->maintenance_active())
        return SendError(req, 409, "maintenance_active");
    if (self->options_.store == nullptr || self->options_.player == nullptr) {
        return SendError(req, 500, "audio_unavailable");
    }
    cJSON* root = nullptr;
    if (!ReadJsonBody(req, &root)) {
        return SendError(req, 400, "invalid_json");
    }
    int target = -1;
    cJSON* name = cJSON_GetObjectItem(root, "name");
    cJSON* index = cJSON_GetObjectItem(root, "index");
    if (cJSON_IsString(name) && name->valuestring != nullptr) {
        target = self->options_.player->FindSoundIndex(name->valuestring);
    } else if (cJSON_IsNumber(index) && std::isfinite(index->valuedouble) &&
               index->valuedouble >= 0 && index->valuedouble < 32 + user_pair_count &&
               std::floor(index->valuedouble) == index->valuedouble) {
        target = static_cast<int>(index->valuedouble);
    }
    cJSON_Delete(root);
    if (target < 0) {
        return SendError(req, 404, "not_found");
    }
    if (!self->options_.player->PlayIndex(target)) {
        return SendError(req, 409, "busy_or_conversation");
    }
    cJSON* out = cJSON_CreateObject();
    if (out == nullptr) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "index", target);
    const esp_err_t err = SendJson(req, out, "202 Accepted");
    cJSON_Delete(out);
    return err;
}

esp_err_t WebConsole::HandleStop(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
        self->maintenance_active())
        return SendError(req, 409, "busy_or_conversation");
    // Serialize with conversation entry and recheck the local owner on the main
    // task. Stopping an absent preview must never reset a cloud reply decoder.
    Application::GetInstance().Schedule([self]() {
        if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
            !self->maintenance_active() && self->options_.player != nullptr &&
            self->options_.player->Playing())
            self->options_.player->StopPlayback();
    });
    cJSON* out = cJSON_CreateObject();
    if (out == nullptr) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    const esp_err_t err = SendJson(req, out);
    cJSON_Delete(out);
    return err;
}

esp_err_t WebConsole::HandleChat(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (self->maintenance_active())
        return SendError(req, 409, "maintenance_active");
    Application::GetInstance().ToggleConversation();
    cJSON* out = cJSON_CreateObject();
    if (out == nullptr) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    const esp_err_t err = SendJson(req, out);
    cJSON_Delete(out);
    return err;
}

esp_err_t WebConsole::HandleHubStatus(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_500(req);
    }
    self->AddHubStatus(root);
    const esp_err_t err = SendJson(req, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t WebConsole::HandleHubCheck(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (self->maintenance_active())
        return SendError(req, 409, "maintenance_active");
    if (self->options_.hub == nullptr || !self->options_.hub->RequestCheck()) {
        return SendError(req, 409, "job_running_or_unavailable");
    }
    cJSON* out = cJSON_CreateObject();
    if (out == nullptr) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    const esp_err_t err = SendJson(req, out, "202 Accepted");
    cJSON_Delete(out);
    return err;
}

esp_err_t WebConsole::HandleHubDiff(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_500(req);
    }
    auto* hub = self->options_.hub;
    const bool ready = hub != nullptr && hub->has_diff();
    cJSON_AddBoolToObject(root, "ready", ready);
    if (ready) {
        cJSON_AddStringToObject(root, "revision", hub->checked_revision().c_str());
        cJSON* items = cJSON_AddArrayToObject(root, "items");
        if (items != nullptr) {
            for (const auto& entry : hub->diff()) {
                cJSON* item = cJSON_CreateObject();
                if (item == nullptr) {
                    continue;
                }
                cJSON_AddStringToObject(item, "name", entry.name.c_str());
                cJSON_AddStringToObject(item, "state", entry.state.c_str());
                cJSON_AddStringToObject(item, "category", entry.category.c_str());
                cJSON_AddNumberToObject(item, "local", entry.local_size);
                cJSON_AddNumberToObject(item, "remote", entry.remote_size);
                cJSON_AddItemToArray(items, item);
            }
        }
    }
    const esp_err_t err = SendJson(req, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t WebConsole::HandleHubUpdate(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (self->maintenance_active())
        return SendError(req, 409, "maintenance_active");
    if (self->options_.hub == nullptr) {
        return SendError(req, 409, "hub_unavailable");
    }
    cJSON* root = nullptr;
    if (!ReadJsonBody(req, &root)) {
        return SendError(req, 400, "invalid_json");
    }
    std::vector<std::string> names;
    cJSON* items = cJSON_GetObjectItem(root, "names");
    const int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    bool valid = count > 0 && count <= static_cast<int>(kMaxUpdateFiles);
    for (int i = 0; valid && i < count; ++i) {
        const auto* item = cJSON_GetArrayItem(items, i);
        valid = cJSON_IsString(item) && item->valuestring != nullptr &&
                item->valuestring[0] != '\0' && std::strlen(item->valuestring) < 64;
        if (valid) {
            valid = std::find(names.begin(), names.end(), item->valuestring) == names.end();
            if (valid)
                names.emplace_back(item->valuestring);
        }
    }
    cJSON_Delete(root);
    if (!valid)
        return SendError(req, 400, "invalid_selection");
    if (!self->options_.hub->RequestUpdate(names)) {
        return SendError(req, 409, "job_running_or_unavailable");
    }
    cJSON* out = cJSON_CreateObject();
    if (out == nullptr) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "count", static_cast<double>(names.size()));
    const esp_err_t err = SendJson(req, out, "202 Accepted");
    cJSON_Delete(out);
    return err;
}

bool WebConsole::RequireDeveloper(httpd_req_t* req) {
    const size_t size = httpd_req_get_hdr_value_len(req, "X-Developer-Token");
    char token[33] = {};
    if (size != 32 ||
        httpd_req_get_hdr_value_str(req, "X-Developer-Token", token, sizeof(token)) != ESP_OK ||
        !developer_.Check(token, esp_timer_get_time() / 1000)) {
        SendError(req, 403, "developer_locked");
        return false;
    }
    return true;
}

esp_err_t WebConsole::HandleDeveloperLogin(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    cJSON* body = nullptr;
    if (!ReadJsonBody(req, &body))
        return SendError(req, 400, "invalid_json");
    const auto* password = cJSON_GetObjectItem(body, "password");
    std::array<uint8_t, 16> random{};
    esp_fill_random(random.data(), random.size());
    const bool ok =
        cJSON_IsString(password) &&
        self->developer_.Unlock(password->valuestring, random, esp_timer_get_time() / 1000);
    cJSON_Delete(body);
    if (!ok)
        return SendError(req, 403, "invalid_password");
    cJSON* out = cJSON_CreateObject();
    if (!out)
        return httpd_resp_send_500(req);
    cJSON_AddStringToObject(out, "token", self->developer_.token());
    cJSON_AddNumberToObject(out, "expires_in_s", DeveloperSession::kLifetimeMs / 1000);
    const auto result = SendJson(req, out);
    cJSON_Delete(out);
    return result;
}

esp_err_t WebConsole::HandleDeveloperLogout(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (!self->RequireDeveloper(req))
        return ESP_OK;
    self->developer_.Lock();
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebConsole::HandleDeveloperStatus(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (!self->RequireDeveloper(req))
        return ESP_OK;
    cJSON* out = cJSON_CreateObject();
    if (!out)
        return httpd_resp_send_500(req);
    self->AddStatus(out);
    cJSON_AddNumberToObject(out, "heap_free", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(out, "heap_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddStringToObject(out, "compiled", esp_app_get_description()->date);
    cJSON_AddStringToObject(out, "compiled_time", esp_app_get_description()->time);
    cJSON* job = cJSON_AddObjectToObject(out, "erase");
    if (job) {
        std::lock_guard<std::mutex> lock(self->job_mutex_);
        cJSON_AddBoolToObject(job, "running", self->maintenance_active());
        cJSON_AddBoolToObject(job, "ok", self->erase_ok_);
        cJSON_AddStringToObject(job, "message", self->erase_message_.c_str());
    }
    const auto result = SendJson(req, out);
    cJSON_Delete(out);
    return result;
}

esp_err_t WebConsole::HandleDeveloperVolume(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (!self->RequireDeveloper(req))
        return ESP_OK;
    cJSON* body = nullptr;
    if (!ReadJsonBody(req, &body))
        return SendError(req, 400, "invalid_json");
    const auto* value = cJSON_GetObjectItem(body, "volume");
    const bool valid = cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
                       value->valuedouble >= 0 && value->valuedouble <= 100 &&
                       std::floor(value->valuedouble) == value->valuedouble;
    const int volume = valid ? static_cast<int>(value->valuedouble) : 0;
    cJSON_Delete(body);
    if (!valid)
        return SendError(req, 400, "invalid_volume");
    if (self->maintenance_active())
        return SendError(req, 409, "maintenance_active");
    Application::GetInstance().Schedule([volume]() {
        if (auto* codec = Board::GetInstance().GetAudioCodec())
            codec->SetOutputVolume(volume);
    });
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, "{\"accepted\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebConsole::HandleDeveloperCenter(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (!self->RequireDeveloper(req))
        return ESP_OK;
    if (self->maintenance_active() ||
        Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
        return SendError(req, 409, "busy_or_conversation");
    if (!self->options_.center)
        return SendError(req, 409, "center_unavailable");
    Application::GetInstance().Schedule([self]() {
        if (!self->maintenance_active() &&
            Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            if (self->options_.player && self->options_.player->Playing())
                self->options_.player->StopPlayback();
            self->options_.center();
        }
    });
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, "{\"accepted\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebConsole::HandleDeveloperDemo(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (!self->RequireDeveloper(req))
        return ESP_OK;
    if (self->maintenance_active() ||
        Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
        self->options_.player == nullptr || self->options_.player->Playing() ||
        self->options_.store == nullptr || self->options_.store->IsWriting())
        return SendError(req, 409, "busy_or_conversation");
    if (self->options_.store->file_count() == 0 && user_pair_count == 0)
        return SendError(req, 404, "audio_library_empty");
    Application::GetInstance().Schedule([self]() {
        if (!self->maintenance_active() &&
            Application::GetInstance().GetDeviceState() == kDeviceStateIdle)
            self->options_.player->PlayAutonomousSound(false);
    });
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, "{\"accepted\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebConsole::HandleDeveloperErase(httpd_req_t* req) {
    auto* self = static_cast<WebConsole*>(req->user_ctx);
    if (!self->RequireDeveloper(req))
        return ESP_OK;
    cJSON* body = nullptr;
    if (!ReadJsonBody(req, &body))
        return SendError(req, 400, "invalid_json");
    const auto* confirm = cJSON_GetObjectItem(body, "confirm");
    const bool confirmed =
        cJSON_IsString(confirm) && std::strcmp(confirm->valuestring, "ERASE") == 0;
    cJSON_Delete(body);
    if (!confirmed)
        return SendError(req, 400, "erase_confirmation_required");
    if (self->options_.store == nullptr || self->options_.store->capacity() == 0 ||
        self->options_.player == nullptr || !self->options_.maintenance)
        return SendError(req, 409, "flash_unavailable");
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
        self->options_.store->IsWriting() ||
        (self->options_.hub && self->options_.hub->job_running()))
        return SendError(req, 409, "busy_or_conversation");
    int battery = 0;
    bool charging = false, discharging = false;
    if (Board::GetInstance().GetBatteryLevel(battery, charging, discharging) && battery <= 15 &&
        !charging)
        return SendError(req, 409, "low_battery");
    if (self->maintenance_active_.exchange(true))
        return SendError(req, 409, "maintenance_active");
    {
        std::lock_guard<std::mutex> lock(self->job_mutex_);
        self->erase_ok_ = false;
        self->erase_message_ = "等待音频和动作停止…";
    }
    // The main task serializes the final idle check with button/wake entry.
    Application::GetInstance().Schedule([self]() { self->StartErase(); });
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, "{\"accepted\":true}", HTTPD_RESP_USE_STRLEN);
}

void WebConsole::StartErase() {
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
        options_.store->IsWriting()) {
        FinishErase(false, "设备正忙，未执行擦除");
        return;
    }
    if (options_.hub != nullptr) {
        if (!options_.hub->BeginLocalMaintenance()) {
            FinishErase(false, "Audio Hub 正在工作，未执行擦除");
            return;
        }
        hub_maintenance_reserved_.store(true);
    }
    options_.maintenance(true);
    options_.player->SetMaintenance(true);
    if (xTaskCreate(EraseTaskEntry, "wzh_dev_erase", 6144, this, 1, nullptr) != pdPASS)
        FinishErase(false, "无法启动擦除任务，未执行擦除");
}

void WebConsole::EraseTaskEntry(void* arg) {
    static_cast<WebConsole*>(arg)->EraseTask();
    vTaskDelete(nullptr);
}

void WebConsole::EraseTask() {
    const int64_t deadline = esp_timer_get_time() + 2000000;
    while (!options_.player->WorkerIdle() && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (!options_.player->WorkerIdle()) {
        FinishErase(false, "音频任务尚未停止，未执行擦除，请稍后重试");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        erase_message_ = "正在擦除外置音频 Flash，请保持供电；完成前无法取消";
    }
    std::string error;
    const bool ok = options_.store->EraseAll(&error);
    FinishErase(ok, ok ? "外置音频 Flash 已清空，可手动重新导入音频" : error);
}

void WebConsole::FinishErase(bool ok, const std::string& message) {
    if (hub_maintenance_reserved_.exchange(false))
        options_.hub->EndLocalMaintenance(ok);
    if (options_.player)
        options_.player->SetMaintenance(false);
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        erase_ok_ = ok;
        erase_message_ = message;
    }
    maintenance_active_.store(false);
    Application::GetInstance().Schedule([this]() {
        if (options_.maintenance)
            options_.maintenance(false);
    });
}

}  // namespace wzh
