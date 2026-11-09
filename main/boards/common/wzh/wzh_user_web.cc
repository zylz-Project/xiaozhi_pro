#include "wzh_user_web.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string_view>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "ssid_manager.h"
#include "voice_tts/voice_config.h"
#include "voice_tts/voice_identity.h"
#include "voice_tts/voice_manager.h"
#include "voice_tts/voice_names.h"
#include "voice_tts/voice_reservation.h"
#include "wifi_configuration_ap.h"
#include "wifi_manager.h"
#include "wzh_factory_reset_policy.h"
#include "wzh_web_peer.h"

extern const char wzh_user_web_html_start[] asm("_binary_wzh_user_web_html_start");
extern const char wzh_user_web_html_end[] asm("_binary_wzh_user_web_html_end");

namespace wzh {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
uint64_t Now() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }
std::array<uint8_t, 16> Random() {
    std::array<uint8_t, 16> result{};
    esp_fill_random(result.data(), result.size());
    return result;
}
std::string Token() {
    static constexpr char digits[] = "0123456789abcdef";
    auto random = Random();
    std::string token(32, '0');
    for (size_t i = 0; i < random.size(); ++i) {
        token[i * 2] = digits[random[i] >> 4];
        token[i * 2 + 1] = digits[random[i] & 15];
    }
    return token;
}
bool Peer(httpd_req_t* req, uint32_t& ip) { return WebPeerIpv4(httpd_req_to_sockfd(req), ip); }
std::string Header(httpd_req_t* req, const char* name, size_t limit = 512) {
    const size_t size = httpd_req_get_hdr_value_len(req, name);
    if (!size || size > limit)
        return {};
    std::string result(size + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, result.data(), result.size()) != ESP_OK)
        return {};
    result.resize(size);
    return result;
}
std::string Cookie(httpd_req_t* req, std::string_view name) {
    const auto header = Header(req, "Cookie");
    size_t pos = 0;
    while (pos < header.size()) {
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == ';'))
            ++pos;
        const size_t end = header.find(';', pos);
        const auto part =
            std::string_view(header).substr(pos, end == std::string::npos ? end : end - pos);
        const auto equals = part.find('=');
        if (equals != std::string_view::npos && part.substr(0, equals) == name)
            return std::string(part.substr(equals + 1));
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return {};
}
std::string Path(httpd_req_t* req) {
    const std::string uri = req->uri;
    return uri.substr(0, uri.find('?'));
}
Json ReadBody(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > 1024)
        return {nullptr, cJSON_Delete};
    std::string data(static_cast<size_t>(req->content_len), '\0');
    size_t received = 0;
    const uint64_t deadline = Now() + 5000;
    while (received < data.size() && Now() < deadline) {
        const int count = httpd_req_recv(req, data.data() + received, data.size() - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (count <= 0)
            return {nullptr, cJSON_Delete};
        received += static_cast<size_t>(count);
    }
    if (received != data.size())
        return {nullptr, cJSON_Delete};
    const char* end = nullptr;
    Json root(cJSON_ParseWithLengthOpts(data.c_str(), data.size() + 1, &end, true), cJSON_Delete);
    if (!cJSON_IsObject(root.get()) || end != data.c_str() + data.size())
        root.reset();
    return root;
}
const char* String(const cJSON* root, const char* field) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, field);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : nullptr;
}
esp_err_t JsonResponse(httpd_req_t* req, cJSON* root, const char* status = nullptr) {
    std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(root), cJSON_free);
    if (!text)
        return httpd_resp_send_500(req);
    if (status)
        httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, text.get(), HTTPD_RESP_USE_STRLEN);
}
esp_err_t Error(httpd_req_t* req, const char* status, const char* code, const char* message) {
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root)
        return httpd_resp_send_500(req);
    cJSON_AddBoolToObject(root.get(), "ok", false);
    auto* error = cJSON_AddObjectToObject(root.get(), "error");
    if (error) {
        cJSON_AddStringToObject(error, "code", code);
        cJSON_AddStringToObject(error, "message", message);
    }
    return JsonResponse(req, root.get(), status);
}
esp_err_t Ok(httpd_req_t* req, const char* status = nullptr) {
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root)
        return httpd_resp_send_500(req);
    cJSON_AddBoolToObject(root.get(), "ok", true);
    return JsonResponse(req, root.get(), status);
}
esp_err_t Redirect(httpd_req_t* req, const char* destination) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", destination);
    return httpd_resp_send(req, nullptr, 0);
}
esp_err_t Html(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, wzh_user_web_html_start,
                           wzh_user_web_html_end - wzh_user_web_html_start);
}
const char* DeviceStateName(DeviceState state) {
    switch (state) {
        case kDeviceStateIdle:
            return "idle";
        case kDeviceStateConnecting:
            return "connecting";
        case kDeviceStateListening:
            return "listening";
        case kDeviceStateSpeaking:
            return "speaking";
        case kDeviceStateUpgrading:
            return "upgrading";
        case kDeviceStateWifiConfiguring:
            return "wifi_configuring";
        default:
            return "busy";
    }
}
bool SetupGuideDone() {
    nvs_handle_t handle = 0;
    uint8_t value = 0;
    if (nvs_open("wzh_web", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "setup_guide", &value);
        nvs_close(handle);
    }
    return value != 0;
}
bool SetSetupGuide(bool complete) {
    nvs_handle_t handle = 0;
    if (nvs_open("wzh_web", NVS_READWRITE, &handle) != ESP_OK)
        return false;
    auto result =
        complete ? nvs_set_u8(handle, "setup_guide", 1) : nvs_erase_key(handle, "setup_guide");
    if (!complete && result == ESP_ERR_NVS_NOT_FOUND)
        result = ESP_OK;
    if (result == ESP_OK)
        result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}
[[maybe_unused]] bool DecodeHeader(std::string_view input, std::string& output) {
    output.clear();
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = input[i];
        if (c == '%') {
            if (i + 2 >= input.size())
                return false;
            const int high = nibble(input[++i]), low = nibble(input[++i]);
            if (high < 0 || low < 0)
                return false;
            c = static_cast<unsigned char>((high << 4) | low);
        }
        if (c == 0 || output.size() >= 48)
            return false;
        output.push_back(static_cast<char>(c));
    }
    return voice_tts::IsValidVoiceName(output);
}
[[maybe_unused]] bool KnownVoice(const std::string& id) {
    for (const auto& entry : voice_tts::kBuiltinVoices)
        if (id == entry.id)
            return true;
    std::vector<voice_tts::VoiceName> names;
    if (!voice_tts::LoadVoiceNames(names))
        return false;
    return std::any_of(names.begin(), names.end(),
                       [&](const auto& entry) { return entry.id == id; });
}
bool Captive(const std::string& path) {
    static constexpr std::string_view paths[] = {
        "/hotspot-detect.html",      "/generate_204", "/mobile/status.php",
        "/check_network_status.txt", "/ncsi.txt",     "/fwlink/",
        "/connectivity-check.html",  "/success.txt",  "/portal.html",
        "/library/test/success.html"};
    for (const auto& entry : paths)
        if (path == entry)
            return true;
    return false;
}
}  // namespace

bool UserWebPortal::Register(httpd_handle_t server, WifiConfigurationAp* portal) {
    portal_ = portal;
    const auto error_handler = [](httpd_req_t* req, httpd_err_code_t error) -> esp_err_t {
        uint32_t ip = 0;
        if (!Peer(req, ip) || !UserWebPolicy::IsApClient(ip))
            return Error(req, "403 Forbidden", "FORBIDDEN", "请连接设备热点");
        return httpd_resp_send_err(req, error, nullptr);
    };
    if (httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, error_handler) != ESP_OK ||
        httpd_register_err_handler(server, HTTPD_405_METHOD_NOT_ALLOWED, error_handler) != ESP_OK)
        return false;
    for (auto method : {HTTP_GET, HTTP_POST}) {
        httpd_uri_t uri{};
        uri.uri = "/*";
        uri.method = method;
        uri.handler = Dispatch;
        uri.user_ctx = this;
        if (httpd_register_uri_handler(server, &uri) != ESP_OK)
            return false;
    }
    return true;
}
esp_err_t UserWebPortal::Dispatch(httpd_req_t* req) {
    return static_cast<UserWebPortal*>(req->user_ctx)->Handle(req);
}
bool UserWebPortal::HasSession(httpd_req_t* req, uint32_t ip, uint64_t now) {
    const auto token = Cookie(req, "wzh_sess");
    std::lock_guard<std::mutex> lock(mutex_);
    return auth_.CheckSession(ip, token, now);
}
bool UserWebPortal::HasCompletion(httpd_req_t* req, uint32_t ip, uint64_t now) {
    const auto token = Cookie(req, "wzh_done");
    std::lock_guard<std::mutex> lock(mutex_);
    return auth_.CheckCompletion(ip, token, now);
}
void UserWebPortal::SetupTask(void* arg) {
    static_cast<UserWebPortal*>(arg)->RunSetup();
    vTaskDelete(nullptr);
}
void UserWebPortal::RunSetup() {
    std::string ssid, password;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ssid = setup_.ssid;
        password = std::move(setup_.password);
        setup_.password.clear();
    }
    auto& wifi = WifiManager::GetInstance();
    const bool ok = wifi.TestAndSavePortalNetwork(ssid, password, [this]() {
        std::lock_guard<std::mutex> lock(mutex_);
        setup_.context = auth_.CreateCompletion(setup_.ip, Random(), Now());
    });
    std::fill(password.begin(), password.end(), '\0');
    {
        std::lock_guard<std::mutex> lock(mutex_);
        setup_.phase = ok ? JobPhase::kOnline : JobPhase::kFailed;
        if (!ok)
            setup_.failure_reason = wifi.LastPortalFailureReason();
        if (ok && setup_.context.empty())
            setup_.context = auth_.CreateCompletion(setup_.ip, Random(), Now());
    }
    setup_running_.store(false);
}
struct UserWebPortal::UploadJob {
    UserWebPortal* portal;
    httpd_req_t* request;
    std::string name;
    size_t size;
};

void UserWebPortal::UploadTask(void* arg) {
    std::unique_ptr<UploadJob> job(static_cast<UploadJob*>(arg));
    auto* self = job->portal;
    auto* req = job->request;
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const size_t prefix = job->name.size() + 1;
    auto* data = static_cast<uint8_t*>(
        heap_caps_malloc(prefix + job->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) {
        Error(req, "503 Service Unavailable", "VOICE_MEMORY", "设备内存不足，请稍后重试");
    } else {
        data[0] = static_cast<uint8_t>(job->name.size());
        std::memcpy(data + 1, job->name.data(), job->name.size());
        size_t offset = 0;
        const auto deadline = Now() + 30000;
        while (offset < job->size && Now() < deadline) {
            const auto count = httpd_req_recv(req, reinterpret_cast<char*>(data + prefix + offset),
                                              std::min<size_t>(4096, job->size - offset));
            if (count == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            if (count <= 0)
                break;
            offset += static_cast<size_t>(count);
        }
        if (offset != job->size || self->reset_running_.load() || !self->options_.voice_upload ||
            !self->options_.voice_upload(data, prefix + job->size)) {
            free(data);
            Error(req, "409 Conflict", "VOICE_BUSY", "录音上传失败或设备正忙");
        } else {
            Ok(req, "202 Accepted");
        }
    }
    httpd_req_async_handler_complete(req);
    self->voice_upload_running_.store(false);
    job.reset();
    vTaskDelete(nullptr);
}

void UserWebPortal::ResetTask(void* arg) {
    static_cast<UserWebPortal*>(arg)->RunReset();
    vTaskDelete(nullptr);
}

void UserWebPortal::RunReset() {
    using Phase = FactoryResetPolicy::Phase;
    auto& wifi = WifiManager::GetInstance();
    FactoryResetPolicy::Operations operations;
    operations.validate = [this, &wifi]() {
        const auto deadline = Now() + 5000;
        while (options_.factory_reset_prepared && !options_.factory_reset_prepared() &&
               wifi.IsConnected() && Now() < deadline)
            vTaskDelay(pdMS_TO_TICKS(20));
        return wifi.IsConnected() && options_.factory_reset_prepared &&
               options_.factory_reset_prepared();
    };
    operations.reset_voices = [this, &wifi]() {
        if (!wifi.IsConnected())
            return false;
        if (voice_tts::LoadAliKey().empty()) {
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA
            return false;
#else
            // Other products historically had no clone service. If their
            // local records contain or may contain a clone, fail closed.
            std::vector<voice_tts::VoiceName> names;
            int reservation = -1;
            if (!voice_tts::LoadVoiceNames(names) || !names.empty() ||
                !voice_tts::LoadVoiceReservation(reservation) || reservation >= 0)
                return false;
            const auto current = voice_tts::LoadAliVoice();
            const auto previous = voice_tts::LoadPreviousAliVoice();
            const auto built_in_or_empty = [](const std::string& id) {
                if (id.empty())
                    return true;
                for (const auto& voice : voice_tts::kBuiltinVoices)
                    if (id == voice.id)
                        return true;
                return false;
            };
            if (!built_in_or_empty(current) || !built_in_or_empty(previous))
                return false;
            return voice_tts::ClearAliVoiceSelection();
#endif
        }
        auto progress = [this](size_t current, size_t total) {
            std::lock_guard<std::mutex> lock(mutex_);
            reset_current_ = current;
            reset_total_ = total;
        };
        auto local = [this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            reset_phase_ = "clearing_local";
        };
        return voice_tts::ResetManagedVoices([&wifi]() { return !wifi.IsConnected(); }, progress,
                                             local) == voice_tts::VoiceOperation::kOk;
    };
    operations.clear_wifi = [&wifi]() {
        return wifi.IsConnected() && SetSetupGuide(false) && wifi.IsConnected() &&
               SsidManager::GetInstance().ClearChecked();
    };
    operations.report = [this](const FactoryResetPolicy::Progress& progress) {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (progress.phase) {
            case Phase::kChecking:
                reset_phase_ = "checking";
                break;
            case Phase::kDeletingVoices:
                reset_phase_ = "deleting_cloud";
                break;
            case Phase::kClearingWifi:
                reset_phase_ = "clearing_wifi";
                break;
            case Phase::kDone:
                reset_phase_ = "rebooting";
                break;
            case Phase::kFailed:
                reset_phase_ = "failed";
                break;
        }
        reset_error_ = progress.error;
    };
    const bool ok = FactoryResetPolicy::Run(operations);
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(1200));
        esp_restart();
    }
    voice_tts::EndVoiceReset();
    if (options_.end_factory_reset)
        options_.end_factory_reset();
    reset_running_.store(false);
}

esp_err_t UserWebPortal::SetupStatus(httpd_req_t* req, uint32_t ip, bool has_session) {
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root)
        return httpd_resp_send_500(req);
    std::string context;
    JobPhase phase;
    int failure_reason;
    bool allowed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        allowed = setup_.ip == ip || has_session;
        phase = setup_.phase;
        failure_reason = setup_.failure_reason;
        // The completion token is only handed out after the device really has
        // an IP; a failed trial must not look like a successful submission.
        context = phase == JobPhase::kOnline ? setup_.context : std::string();
        if (!context.empty() && !auth_.CheckCompletion(ip, context, Now()))
            context.clear();
    }
    if (!allowed)
        return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请重新连接设备热点");
    cJSON_AddBoolToObject(root.get(), "ok", true);
    const char* name = phase == JobPhase::kOnline   ? "online"
                       : phase == JobPhase::kFailed ? "failed"
                                                    : "connecting";
    cJSON_AddStringToObject(root.get(), "phase", name);
    cJSON_AddStringToObject(root.get(), "net_state",
                            WifiManager::GetInstance().IsConnected() ? "ONLINE" : "OFFLINE");
    if (phase == JobPhase::kFailed) {
        const auto failure = UserWebPolicy::SetupFailure(failure_reason);
        auto* error = cJSON_AddObjectToObject(root.get(), "error");
        if (error) {
            cJSON_AddStringToObject(error, "code", failure.code);
            cJSON_AddStringToObject(error, "message", failure.message);
        }
    }
    if (!context.empty()) {
        cJSON_AddStringToObject(root.get(), "ctx", context.c_str());
        const auto cookie =
            "wzh_done=" + context + "; HttpOnly; SameSite=Strict; Path=/; Max-Age=600";
        httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
        return JsonResponse(req, root.get());
    }
    return JsonResponse(req, root.get());
}

esp_err_t UserWebPortal::FactoryResetStatus(httpd_req_t* req) {
    Json out(cJSON_CreateObject(), cJSON_Delete);
    if (!out)
        return httpd_resp_send_500(req);
    std::string phase, error_code;
    size_t current, total;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase = reset_phase_;
        error_code = reset_error_;
        current = reset_current_;
        total = reset_total_;
    }
    cJSON_AddBoolToObject(out.get(), "ok", true);
    cJSON_AddStringToObject(out.get(), "phase", phase.c_str());
    cJSON_AddNumberToObject(out.get(), "current", current);
    cJSON_AddNumberToObject(out.get(), "total", total);
    if (!error_code.empty()) {
        auto* error = cJSON_AddObjectToObject(out.get(), "error");
        if (error) {
            cJSON_AddStringToObject(error, "code", error_code.c_str());
            cJSON_AddStringToObject(error, "message",
                                    "恢复失败，已保留 Wi-Fi，请检查网络和语音服务后重试。");
        }
    }
    return JsonResponse(req, out.get());
}

esp_err_t UserWebPortal::StartFactoryReset(httpd_req_t* req) {
    if (!WifiManager::GetInstance().IsConnected())
        return Error(req, "409 Conflict", "NET_OFFLINE", "设备暂未联网");
    auto body = ReadBody(req);
    const char* confirm = body ? String(body.get(), "confirm") : nullptr;
    std::string phrase = confirm ? confirm : "";
    const auto first = phrase.find_first_not_of(" \t\r\n");
    const auto last = phrase.find_last_not_of(" \t\r\n");
    phrase = first == std::string::npos ? "" : phrase.substr(first, last - first + 1);
    if (phrase != "确定恢复出厂设置")
        return Error(req, "400 Bad Request", "INVALID_CONFIRMATION", "请输入完整的确认文字");
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA
    if (!options_.has_ali_key || !options_.has_ali_key())
        return Error(req, "409 Conflict", "VOICE_NO_KEY", "语音服务未配置，无法确认云端音色");
#endif
    if (setup_running_.load() || voice_upload_running_.load())
        return Error(req, "409 Conflict", "RESET_BUSY", "设备正在忙，请稍后重试");
    bool expected = false;
    if (!reset_running_.compare_exchange_strong(expected, true))
        return Error(req, "409 Conflict", "RESET_BUSY", "恢复出厂正在进行");
    if (setup_running_.load() || voice_upload_running_.load()) {
        reset_running_.store(false);
        return Error(req, "409 Conflict", "RESET_BUSY", "配网进行中，请稍后重试");
    }
    if (!voice_tts::BeginVoiceReset()) {
        reset_running_.store(false);
        return Error(req, "409 Conflict", "VOICE_BUSY", "声音服务正在处理，请稍后重试");
    }
    if (!options_.begin_factory_reset || !options_.begin_factory_reset()) {
        voice_tts::EndVoiceReset();
        reset_running_.store(false);
        return Error(req, "409 Conflict", "RESET_BUSY", "设备正在忙，请稍后重试");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_phase_ = "checking";
        reset_error_.clear();
        reset_current_ = reset_total_ = 0;
    }
    if (xTaskCreate(ResetTask, "wzh_reset", 12288, this, 2, nullptr) != pdPASS) {
        voice_tts::EndVoiceReset();
        reset_running_.store(false);
        if (options_.end_factory_reset)
            options_.end_factory_reset();
        return Error(req, "503 Service Unavailable", "RESET_UNAVAILABLE", "设备忙，请稍后重试");
    }
    return Ok(req, "202 Accepted");
}

esp_err_t UserWebPortal::Handle(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    uint32_t ip = 0;
    if (!Peer(req, ip) || !UserWebPolicy::IsApClient(ip))
        return Error(req, "403 Forbidden", "FORBIDDEN", "请连接设备热点");
    const auto path = Path(req);
    const auto now = Now();
    const bool session = HasSession(req, ip, now);
    auto& wifi = WifiManager::GetInstance();
    const bool connected = wifi.IsConnected();
    if (connected)
        last_online_ms_.store(now);
    const auto last_online = last_online_ms_.load();
    const bool online = connected || (last_online && now - last_online < 3000);
    const bool have_saved = !SsidManager::GetInstance().GetSsidList().empty();
    if (req->method == HTTP_POST &&
        !UserWebPolicy::ValidOrigin(Header(req, "Host", 128), Header(req, "Origin", 128)))
        return Error(req, "403 Forbidden", "BAD_ORIGIN", "请求来源不允许");

    if (req->method == HTTP_GET) {
        // Different phone vendors probe different paths (including "/").
        // Canonicalize their host first so the portal's forms have the correct
        // origin and unknown HTTP probes still discover the login page.
        const auto host = Header(req, "Host", 128);
        if (!host.empty() && host != "192.168.4.1" && host != "192.168.4.1:80")
            return Redirect(req, "http://192.168.4.1/");
        if (Captive(path))
            return Redirect(req, "http://192.168.4.1/");
        if (path == "/done.html")
            return Redirect(req, "/done");
        if (path == "/api/status") {
            Json out(cJSON_CreateObject(), cJSON_Delete);
            if (!out)
                return httpd_resp_send_500(req);
            cJSON_AddBoolToObject(out.get(), "ok", true);
            cJSON_AddStringToObject(out.get(), "product", options_.product_name.c_str());
            cJSON_AddStringToObject(out.get(), "build", options_.build_name.c_str());
            cJSON_AddStringToObject(out.get(), "version", esp_app_get_description()->version);
            cJSON_AddNumberToObject(out.get(), "uptime_s",
                                    static_cast<double>(esp_timer_get_time()) / 1000000.0);
            cJSON_AddStringToObject(out.get(), "state",
                                    DeviceStateName(Application::GetInstance().GetDeviceState()));
            cJSON_AddBoolToObject(out.get(), "saved_network", have_saved);
            cJSON_AddBoolToObject(out.get(), "setup_guide_done", SetupGuideDone());
            auto* wireless = cJSON_AddObjectToObject(out.get(), "wifi");
            if (wireless) {
                // Report the raw link state; routing keeps a 3 s debounce so a
                // transient dropout does not bounce an open page.
                cJSON_AddBoolToObject(wireless, "connected", connected);
                if (connected) {
                    cJSON_AddStringToObject(wireless, "ssid", wifi.GetSsid().c_str());
                    cJSON_AddStringToObject(wireless, "ip", wifi.GetIpAddress().c_str());
                    cJSON_AddNumberToObject(wireless, "rssi", wifi.GetRssi());
                }
            }
            int level = 0;
            bool charging = false, discharging = false;
            auto* battery = cJSON_AddObjectToObject(out.get(), "battery");
            if (battery) {
                const bool available =
                    Board::GetInstance().GetBatteryLevel(level, charging, discharging);
                cJSON_AddBoolToObject(battery, "available", available);
                if (available) {
                    cJSON_AddNumberToObject(battery, "percent", level);
                    cJSON_AddBoolToObject(battery, "charging", charging);
                }
            }
            float temperature = 0;
            if (Board::GetInstance().GetTemperature(temperature))
                cJSON_AddNumberToObject(out.get(), "temperature_c", temperature);
            auto* audio = cJSON_AddObjectToObject(out.get(), "audio");
            if (audio && Board::GetInstance().GetAudioCodec())
                cJSON_AddNumberToObject(audio, "volume",
                                        Board::GetInstance().GetAudioCodec()->output_volume());
            auto* capabilities = cJSON_AddObjectToObject(out.get(), "capabilities");
            if (capabilities) {
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
                cJSON_AddBoolToObject(capabilities, "voice", true);
#else
                cJSON_AddBoolToObject(capabilities, "voice", false);
#endif
            }
            return JsonResponse(req, out.get());
        }
        if (path == "/api/user/voices/device-record/status") {
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
            if (!session)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
            Json out(cJSON_CreateObject(), cJSON_Delete);
            if (!out)
                return httpd_resp_send_500(req);
            cJSON_AddBoolToObject(out.get(), "ok", true);
            cJSON_AddStringToObject(out.get(), "phase",
                                    Application::GetInstance().DeviceVoiceEnrollmentStatus());
            cJSON_AddStringToObject(
                out.get(), "failure",
                options_.voice_enrollment_failure ? options_.voice_enrollment_failure() : "");
            return JsonResponse(req, out.get());
#else
            return Error(req, "404 Not Found", "NOT_FOUND", "声音服务不可用");
#endif
        }
        if (path == "/api/user/voices" || path == "/api/user/voices/clone/status") {
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
            if (!session)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
            if (!online)
                return Error(req, "409 Conflict", "NET_OFFLINE", "设备暂未联网");
            if (reset_running_.load())
                return Error(req, "503 Service Unavailable", "RESET_BUSY", "恢复出厂进行中");
            if (!options_.voice_status_extra)
                return Error(req, "404 Not Found", "NOT_FOUND", "声音服务不可用");
            Json out(cJSON_CreateObject(), cJSON_Delete);
            if (!out)
                return httpd_resp_send_500(req);
            options_.voice_status_extra(out.get());
            const bool has_key = options_.has_ali_key && options_.has_ali_key();
            if (!has_key && path == "/api/user/voices") {
                cJSON_AddBoolToObject(out.get(), "ok", true);
                cJSON_AddBoolToObject(out.get(), "has_key", false);
                return JsonResponse(req, out.get());
            }
            const auto* busy = cJSON_GetObjectItemCaseSensitive(out.get(), "voice_management_busy");
            const auto* state =
                cJSON_GetObjectItemCaseSensitive(out.get(), "voice_management_status");
            if (path == "/api/user/voices/clone/status") {
                cJSON_AddBoolToObject(out.get(), "ok", true);
                cJSON_AddStringToObject(
                    out.get(), "phase",
                    cJSON_IsTrue(busy) ? "working"
                    : (cJSON_IsString(state) && std::strcmp(state->valuestring, "ok") == 0)
                        ? "done"
                        : "failed");
                return JsonResponse(req, out.get());
            }
            if (!cJSON_IsTrue(busy) && options_.voice_command &&
                (!voice_last_request_ms_ || now - voice_last_request_ms_ >= 30000)) {
                if (options_.voice_command("list", "", "")) {
                    voice_last_request_ms_ = now;
                    // The response was assembled before this async request;
                    // keep the browser polling until the new list is ready.
                    cJSON_ReplaceItemInObjectCaseSensitive(out.get(), "voice_management_busy",
                                                           cJSON_CreateBool(true));
                }
            }
            const auto current = options_.ali_voice ? options_.ali_voice() : "";
            const auto previous = options_.previous_ali_voice ? options_.previous_ali_voice() : "";
            auto* catalog = cJSON_DetachItemFromObjectCaseSensitive(out.get(), "voice_catalog");
            if (catalog) {
                cJSON* item = nullptr;
                cJSON_ArrayForEach (item, catalog) {
                    const char* id = String(item, "id");
                    cJSON_AddBoolToObject(item, "current", id && current == id);
                    cJSON_AddBoolToObject(item, "previous", id && previous == id);
                }
                cJSON_AddItemToObject(out.get(), "voices", catalog);
            }
            cJSON_AddBoolToObject(out.get(), "ok", true);
            cJSON_AddBoolToObject(out.get(), "has_key",
                                  options_.has_ali_key && options_.has_ali_key());
            return JsonResponse(req, out.get());
#else
            return Error(req, "404 Not Found", "NOT_FOUND", "声音服务不可用");
#endif
        }
        if (path == "/api/user/factory-reset/status") {
            if (!session)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
            return FactoryResetStatus(req);
        }
        if (path == "/api/setup/status") {
            char query[160] = {}, job[40] = {};
            if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
                httpd_query_key_value(query, "job", job, sizeof(job)) != ESP_OK)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请重新连接设备热点");
            bool matches;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                matches = setup_.ip == ip && setup_.id == job;
            }
            if (!matches)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "配网任务已失效");
            return SetupStatus(req, ip, session);
        }
        if (path == "/scan") {
            if (online && !session)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
            Json out(cJSON_CreateObject(), cJSON_Delete);
            if (!out)
                return httpd_resp_send_500(req);
            cJSON_AddBoolToObject(out.get(), "ok", true);
            auto* array = cJSON_AddArrayToObject(out.get(), "aps");
            if (array && portal_) {
                auto records = portal_->GetAccessPoints();
                const auto saved = SsidManager::GetInstance().GetSsidList();
                const auto current = connected ? wifi.GetSsid() : "";
                std::vector<std::string> found;
                std::sort(records.begin(), records.end(),
                          [](const auto& a, const auto& b) { return a.rssi > b.rssi; });
                for (const auto& record : records) {
                    const size_t length =
                        strnlen(reinterpret_cast<const char*>(record.ssid), sizeof(record.ssid));
                    if (!length)
                        continue;
                    std::string name(reinterpret_cast<const char*>(record.ssid), length);
                    if (std::find(found.begin(), found.end(), name) != found.end())
                        continue;
                    auto* item = cJSON_CreateObject();
                    if (!item)
                        break;
                    cJSON_AddStringToObject(item, "ssid", name.c_str());
                    cJSON_AddNumberToObject(item, "rssi", record.rssi);
                    cJSON_AddNumberToObject(item, "auth", record.authmode);
                    cJSON_AddBoolToObject(
                        item, "saved",
                        std::any_of(saved.begin(), saved.end(),
                                    [&](const SsidItem& entry) { return entry.ssid == name; }));
                    cJSON_AddBoolToObject(item, "current", !current.empty() && current == name);
                    cJSON_AddItemToArray(array, item);
                    found.push_back(name);
                }
                for (const auto& entry : saved) {
                    if (entry.ssid.empty() ||
                        std::find(found.begin(), found.end(), entry.ssid) != found.end())
                        continue;
                    auto* item = cJSON_CreateObject();
                    if (!item)
                        break;
                    cJSON_AddStringToObject(item, "ssid", entry.ssid.c_str());
                    cJSON_AddBoolToObject(item, "saved", true);
                    if (!current.empty() && current == entry.ssid)
                        cJSON_AddBoolToObject(item, "current", true);
                    else
                        cJSON_AddBoolToObject(item, "unavailable", true);
                    cJSON_AddItemToArray(array, item);
                }
            }
            const auto last_scan = last_scan_ms_.load();
            if (!setup_running_.load() && !reset_running_.load() &&
                (!last_scan || now - last_scan >= 5000)) {
                last_scan_ms_.store(now);
                esp_wifi_scan_start(nullptr, false);
            }
            return JsonResponse(req, out.get());
        }
        if (path == "/" || path == "/setup" || path == "/login" || path == "/manual" ||
            path == "/done") {
            using Network = UserWebPolicy::Network;
            using Route = UserWebPolicy::Route;
            const auto network = online                  ? Network::kOnline
                                 : !have_saved           ? Network::kUnprovisioned
                                 : setup_running_.load() ? Network::kConnecting
                                                         : Network::kOffline;
            const auto route = UserWebPolicy::RouteFor(
                path, network, session, path == "/done" && HasCompletion(req, ip, now));
            switch (route) {
                case Route::kDone:
                    return Html(req);
                case Route::kRoot:
                    return Redirect(req, "/");
                case Route::kSetupFirst:
                    return path == "/setup" ? Html(req) : Redirect(req, "/setup?mode=first");
                case Route::kSetupOffline:
                    return path == "/setup" ? Html(req) : Redirect(req, "/setup?mode=offline");
                case Route::kSetupConnecting:
                    return path == "/setup" ? Html(req)
                                            : Redirect(req, "/setup?mode=offline&connecting=1");
                case Route::kSetupSwitch:
                    return Html(req);
                case Route::kLogin:
                    if (path == "/login")
                        return Html(req);
                    if (path == "/setup")
                        return Redirect(req, "/login?next=/setup");
                    if (path == "/manual")
                        return Redirect(req, "/login?next=/manual");
                    return Redirect(req, "/login?next=/");
                case Route::kManual:
                    return Html(req);
                case Route::kDetails:
                    return path == "/login" ? Redirect(req, "/") : Html(req);
            }
        }
        return Error(req, "404 Not Found", "NOT_FOUND", "页面不存在");
    }
    if (reset_running_.load())
        return Error(req, "503 Service Unavailable", "RESET_BUSY", "恢复出厂进行中，请稍候");
    if (req->method != HTTP_POST)
        return Error(req, "405 Method Not Allowed", "METHOD_NOT_ALLOWED", "请求方式不支持");
    if (path == "/exit")
        return Error(req, "410 Gone", "REMOVED", "设备热点将继续运行");
    if (path == "/api/setup/status") {
        bool completion = HasCompletion(req, ip, now);
        if (!completion) {
            // The completion page normally carries the token in the body; the
            // HttpOnly cookie from the submit response is an equivalent proof.
            auto body = ReadBody(req);
            const char* value = body ? String(body.get(), "ctx") : nullptr;
            if (value && *value) {
                std::lock_guard<std::mutex> lock(mutex_);
                completion = auth_.CheckCompletion(ip, value, now);
            }
        }
        if (!completion && !session)
            return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "完成页已失效");
        return SetupStatus(req, ip, session);
    }
    if (path == "/api/login") {
        if (!online)
            return Error(req, "409 Conflict", "NET_OFFLINE", "设备暂未联网");
        auto body = ReadBody(req);
        const char* value = body ? String(body.get(), "password") : nullptr;
        if (!value || std::strlen(value) > 64)
            return Error(req, "400 Bad Request", "INVALID_INPUT", "请输入页面密码");
        uint8_t mac[6]{};
        if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK)
            return httpd_resp_send_500(req);
        std::string token;
        UserWebPolicy::LoginResult result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result = auth_.Login(ip, value, UserWebPolicy::Password(mac[4], mac[5]), Random(), now,
                                 token);
        }
        if (result == UserWebPolicy::LoginResult::kRateLimited) {
            httpd_resp_set_hdr(req, "Retry-After", "60");
            return Error(req, "429 Too Many Requests", "AUTH_RATE_LIMITED",
                         "尝试次数过多，请稍后重试");
        }
        if (result != UserWebPolicy::LoginResult::kOk)
            return Error(req, "401 Unauthorized", "AUTH_BAD_PASSWORD", "密码不正确");
        const auto cookie =
            "wzh_sess=" + token + "; HttpOnly; SameSite=Strict; Path=/; Max-Age=1800";
        httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
        return Ok(req);
    }
    if (path == "/api/enter") {
        auto body = ReadBody(req);
        const char* value = body ? String(body.get(), "ctx") : nullptr;
        const std::string completion =
            (value && *value) ? std::string(value) : Cookie(req, "wzh_done");
        if (completion.empty())
            return Error(req, "400 Bad Request", "INVALID_INPUT", "完成页已失效");
        std::string token;
        bool valid;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            valid = setup_.phase == JobPhase::kOnline && setup_.ip == ip &&
                    auth_.Enter(ip, completion, Random(), now, online, token);
        }
        if (!valid)
            return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "完成页已失效，请重新登录");
        const auto cookie =
            "wzh_sess=" + token + "; HttpOnly; SameSite=Strict; Path=/; Max-Age=1800";
        httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
        return Ok(req);
    }
    if (path == "/api/logout") {
        if (!session)
            return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
        const auto token = Cookie(req, "wzh_sess");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auth_.Logout(ip, token);
        }
        httpd_resp_set_hdr(req, "Set-Cookie",
                           "wzh_sess=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
        return Ok(req);
    }
    if (path == "/api/user/guide/setup-complete") {
        if (online && !session)
            return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
        if (!SetSetupGuide(true))
            return Error(req, "500 Internal Server Error", "SAVE_FAILED", "保存失败，请重试");
        return Ok(req);
    }
    if (path == "/submit") {
        if (online && !session)
            return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
        auto body = ReadBody(req);
        const char* ssid = body ? String(body.get(), "ssid") : nullptr;
        const char* password = body ? String(body.get(), "password") : nullptr;
        if (!ssid || !*ssid || std::strlen(ssid) > 32 || !password || std::strlen(password) > 64)
            return Error(req, "400 Bad Request", "INVALID_INPUT", "请检查网络名称和密码");
        bool expected = false;
        if (!setup_running_.compare_exchange_strong(expected, true))
            return Error(req, "409 Conflict", "SETUP_BUSY", "正在连接网络，请稍候");
        if (reset_running_.load() || voice_upload_running_.load()) {
            setup_running_.store(false);
            return Error(req, "503 Service Unavailable", "RESET_BUSY", "恢复出厂进行中，请稍候");
        }
        std::string job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            setup_ = {};
            setup_.phase = JobPhase::kConnecting;
            setup_.ip = ip;
            setup_.id = Token();
            setup_.ssid = ssid;
            setup_.password = password;
            job = setup_.id;
        }
        if (xTaskCreate(SetupTask, "wzh_setup", 8192, this, 3, nullptr) != pdPASS) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                setup_ = {};
                setup_running_.store(false);
            }
            return Error(req, "503 Service Unavailable", "SETUP_UNAVAILABLE", "设备忙，请稍后重试");
        }
        Json out(cJSON_CreateObject(), cJSON_Delete);
        if (!out)
            return httpd_resp_send_500(req);
        cJSON_AddBoolToObject(out.get(), "ok", true);
        cJSON_AddStringToObject(out.get(), "job", job.c_str());
        return JsonResponse(req, out.get(), "202 Accepted");
    }
    if (!session)
        return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
    if (!online)
        return Error(req, "409 Conflict", "NET_OFFLINE", "设备暂未联网");
    if (path.rfind("/api/user/voices/", 0) == 0) {
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
        if (!options_.has_ali_key || !options_.has_ali_key())
            return Error(req, "409 Conflict", "VOICE_NO_KEY", "语音服务未配置");
        const auto action = path.substr(std::strlen("/api/user/voices/"));
        if (action == "device-record") {
            auto body = ReadBody(req);
            const char* name = body ? String(body.get(), "name") : nullptr;
            if (!name || !voice_tts::IsValidVoiceName(name))
                return Error(req, "400 Bad Request", "VOICE_INVALID", "请输入有效的音色名称");
            if (setup_running_.load() || voice_upload_running_.load() || reset_running_.load() ||
                voice_tts::VoiceResetActive() || !options_.voice_device_record ||
                !options_.voice_device_record(name))
                return Error(req, "409 Conflict", "VOICE_BUSY", "设备正忙，请稍后重试");
            return Ok(req, "202 Accepted");
        }
        if (action == "clone") {
            constexpr size_t kMin = 5 * 16000 * 2, kMax = 20 * 16000 * 2;
            const size_t size = static_cast<size_t>(req->content_len);
            std::string name;
            if (size == 0 || size < kMin || size > kMax || size % 2 ||
                !DecodeHeader(Header(req, "X-Voice-Name", 150), name))
                return Error(req, "400 Bad Request", "VOICE_INVALID",
                             "请提供 5–20 秒的录音和有效名称");
            if (!options_.voice_upload)
                return Error(req, "409 Conflict", "VOICE_UNAVAILABLE", "声音服务不可用");
            if (options_.voice_status_extra) {
                Json status(cJSON_CreateObject(), cJSON_Delete);
                if (!status)
                    return httpd_resp_send_500(req);
                options_.voice_status_extra(status.get());
                const auto* loaded =
                    cJSON_GetObjectItemCaseSensitive(status.get(), "voice_management_loaded");
                const auto* catalog =
                    cJSON_GetObjectItemCaseSensitive(status.get(), "voice_catalog");
                int clones = 0;
                const cJSON* entry = nullptr;
                cJSON_ArrayForEach (entry, catalog) {
                    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "builtin")))
                        ++clones;
                }
                if (cJSON_IsTrue(loaded) && clones >= 10)
                    return Error(req, "409 Conflict", "VOICE_LIMIT", "专属音色已达上限，请先删除");
            }
            bool expected = false;
            if (!voice_upload_running_.compare_exchange_strong(expected, true))
                return Error(req, "409 Conflict", "VOICE_BUSY", "已有录音正在上传");
            if (reset_running_.load() || setup_running_.load()) {
                voice_upload_running_.store(false);
                return Error(req, "409 Conflict", "VOICE_BUSY", "设备正在忙，请稍后重试");
            }
            auto* job = new (std::nothrow) UploadJob{this, nullptr, std::move(name), size};
            if (!job) {
                voice_upload_running_.store(false);
                return Error(req, "503 Service Unavailable", "VOICE_MEMORY", "设备内存不足");
            }
            if (httpd_req_async_handler_begin(req, &job->request) != ESP_OK) {
                delete job;
                voice_upload_running_.store(false);
                return Error(req, "503 Service Unavailable", "VOICE_BUSY",
                             "设备正在忙，请稍后重试");
            }
            if (xTaskCreate(UploadTask, "wzh_upload", 8192, job, 3, nullptr) != pdPASS) {
                Error(job->request, "503 Service Unavailable", "VOICE_MEMORY", "设备内存不足");
                httpd_req_async_handler_complete(job->request);
                delete job;
                voice_upload_running_.store(false);
            }
            return ESP_OK;
        }
        auto body = ReadBody(req);
        const char* id = body ? String(body.get(), "id") : nullptr;
        const char* name = body ? String(body.get(), "name") : nullptr;
        if (!id || !*id || std::strlen(id) > 128)
            return Error(req, "400 Bad Request", "VOICE_INVALID", "音色无效");
        if (action == "audition") {
            if (!KnownVoice(id))
                return Error(req, "400 Bad Request", "VOICE_INVALID", "音色不存在");
            if (!Application::GetInstance().RequestExternalVoicePreview(id))
                return Error(req, "409 Conflict", "VOICE_BUSY", "设备正忙，请稍后试听");
            return Ok(req, "202 Accepted");
        }
        if (action != "select" && action != "rename" && action != "delete")
            return Error(req, "404 Not Found", "NOT_FOUND", "操作不存在");
        if (action == "rename" && (!name || !voice_tts::IsValidVoiceName(name)))
            return Error(req, "400 Bad Request", "VOICE_INVALID", "请输入有效名称");
        if (!options_.voice_command || !options_.voice_command(action, id, name ? name : ""))
            return Error(req, "409 Conflict", "VOICE_BUSY", "设备正忙，请稍后重试");
        return Ok(req, "202 Accepted");
#else
        return Error(req, "404 Not Found", "NOT_FOUND", "声音服务不可用");
#endif
    }
    if (path == "/api/user/factory-reset")
        return StartFactoryReset(req);
    if (path == "/api/user/volume") {
        auto body = ReadBody(req);
        const auto* value = body ? cJSON_GetObjectItemCaseSensitive(body.get(), "volume") : nullptr;
        if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble))
            return Error(req, "400 Bad Request", "INVALID_INPUT", "音量无效");
        const int volume = static_cast<int>(std::clamp(value->valuedouble, 0.0, 100.0));
        Application::GetInstance().Schedule([volume]() {
            if (auto* codec = Board::GetInstance().GetAudioCodec())
                codec->SetOutputVolume(volume);
        });
        Json out(cJSON_CreateObject(), cJSON_Delete);
        if (!out)
            return httpd_resp_send_500(req);
        cJSON_AddBoolToObject(out.get(), "ok", true);
        cJSON_AddNumberToObject(out.get(), "volume", volume);
        return JsonResponse(req, out.get());
    }
    if (path == "/api/user/chat/toggle") {
        const auto state = Application::GetInstance().GetDeviceState();
        if (state != kDeviceStateIdle && state != kDeviceStateListening &&
            state != kDeviceStateSpeaking)
            return Error(req, "409 Conflict", "DEVICE_BUSY", "设备正在忙");
        // Never run the audio/state-machine work on the HTTP task.
        Application::GetInstance().Schedule(
            [] { Application::GetInstance().ToggleConversation(); });
        return Ok(req);
    }
    return Error(req, "404 Not Found", "NOT_FOUND", "页面不存在");
}
}  // namespace wzh
