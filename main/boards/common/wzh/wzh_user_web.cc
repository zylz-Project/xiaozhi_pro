#include "wzh_user_web.h"
#include "wzh_system_prompts.h"
#include "wzh_web_assets.h"

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
#include "voice_tts/voice_http.h"
#include "voice_tts/voice_identity.h"
#include "voice_tts/voice_manager.h"
#include "voice_tts/voice_names.h"
#include "wifi_configuration_ap.h"
#include "wifi_manager.h"
#include "wzh_factory_reset_policy.h"
#include "wzh_onboarding.h"
#include "wzh_onboarding_store.h"
#include "wzh_web_peer.h"

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
    const auto path = Path(req);
    const char* asset = path == "/setup"    ? "setup.html"
                        : path == "/login"  ? "login.html"
                        : path == "/done"   ? "done.html"
                        : path == "/manual" ? "manual.html"
                                            : "index.html";
    return SendWebAsset(req, asset);
}
const char* DeviceStateName(DeviceState state) {
    switch (state) {
        case kDeviceStateIdle:
            return "idle";
        case kDeviceStateConnecting:
            return "connecting";
        case kDeviceStateActivating:
            return "activating";
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
    // Called from the HTTP task on every /api/status poll: read only, never
    // migrate or commit here.
    OnboardingProgress progress;
    return PeekOnboardingProgress(progress) && progress.Done();
}
// Only used to erase the legacy migration source during a factory reset. The
// completion bit itself is written by the tutorial store when both chapters
// have really finished playing.
[[maybe_unused]] bool SetSetupGuide(bool complete) {
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
bool ClearOnboarding() {
    StopOnboarding();
    const auto deadline = Now() + 8000;
    while (IsOnboardingWorkerRunning() && Now() < deadline)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (IsOnboardingWorkerRunning())
        return false;
    // Clear the legacy migration source first, so a power loss cannot
    // resurrect an erased progress record from the old completion flag.
    if (!SetSetupGuide(false))
        return false;
    nvs_handle_t handle = 0;
    if (nvs_open("onboarding", NVS_READWRITE, &handle) != ESP_OK)
        return false;
    auto result = nvs_erase_all(handle);
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
[[maybe_unused]] bool KnownVoice(const std::string& id,
                                 const std::function<void(cJSON*)>& add_voice_status) {
    for (const auto& entry : voice_tts::kBuiltinVoices)
        if (id == entry.id)
            return true;
    std::vector<voice_tts::VoiceName> names;
    if (voice_tts::LoadVoiceNames(names) &&
        std::any_of(names.begin(), names.end(), [&](const auto& entry) { return entry.id == id; }))
        return true;
    if (!add_voice_status)
        return false;
    // A recovered clone can be present in the device-verified cloud catalog
    // before it has a local label. Read the cache without requesting a list.
    Json status(cJSON_CreateObject(), cJSON_Delete);
    if (!status)
        return false;
    add_voice_status(status.get());
    const auto* catalog = cJSON_GetObjectItemCaseSensitive(status.get(), "voice_catalog");
    if (!cJSON_IsArray(catalog))
        return false;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach (item, catalog) {
        const char* known_id = String(item, "id");
        if (known_id && id == known_id)
            return true;
    }
    return false;
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
        if (offset != job->size || self->reset_running_.load() || !self->options_.voice_upload) {
            free(data);
            Error(req, "409 Conflict", "VOICE_BUSY", "录音上传失败或设备正忙");
        } else {
            Json out(cJSON_CreateObject(), cJSON_Delete);
            auto* request = out && cJSON_AddBoolToObject(out.get(), "ok", true)
                                ? cJSON_AddNumberToObject(out.get(), "request", 0)
                                : nullptr;
            if (!request) {
                free(data);
                httpd_resp_send_500(req);
            } else if (const auto ticket = self->options_.voice_upload(data, prefix + job->size)) {
                cJSON_SetNumberValue(request, ticket);
                JsonResponse(req, out.get(), "202 Accepted");
            } else {
                free(data);
                Error(req, "409 Conflict", "VOICE_BUSY", "声音服务正在处理，请稍后重试");
            }
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
    FactoryResetPolicy::Operations operations;
    operations.validate = [this]() {
        const auto deadline = Now() + 5000;
        while (options_.factory_reset_prepared && !options_.factory_reset_prepared() &&
               Now() < deadline)
            vTaskDelay(pdMS_TO_TICKS(20));
        return options_.factory_reset_prepared && options_.factory_reset_prepared();
    };
    operations.reset_voices = [this]() -> const char* {
        auto progress = [this](size_t current, size_t total) {
            std::lock_guard<std::mutex> lock(mutex_);
            reset_phase_ = "deleting_cloud";
            reset_current_ = current;
            reset_total_ = total;
        };
        auto local = [this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            reset_phase_ = "clearing_local";
        };
        const auto status = voice_tts::ResetManagedVoices(
            []() {
#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC && CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
                return !voice_tts::VoiceHttpNetworkAvailable();
#else
                return !Board::GetInstance().HasSystemNetwork();
#endif
            },
            progress, local);
        using Voice = voice_tts::VoiceOperation;
        switch (status) {
            case Voice::kOk:
                return nullptr;
            case Voice::kBusy:
                return "RESET_VOICE_BUSY";
            case Voice::kConfig:
                return "RESET_VOICE_CONFIG";
            case Voice::kCancelled:
                return "RESET_VOICE_OFFLINE";
            case Voice::kNetwork:
                return "RESET_VOICE_CLOUD_FAILED";
            case Voice::kUncertain:
                return "RESET_VOICE_UNCERTAIN";
            default:
                return "RESET_VOICE_SAVE_FAILED";
        }
    };
    operations.clear_onboarding = []() { return ClearOnboarding(); };
    operations.clear_wifi = []() { return SsidManager::GetInstance().ClearChecked(); };
    operations.report = [this](const FactoryResetPolicy::Progress& progress) {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (progress.phase) {
            case Phase::kChecking:
                reset_phase_ = "checking";
                break;
            case Phase::kCheckingVoiceRecords:
                reset_phase_ = "checking";
                break;
            case Phase::kClearingOnboarding:
                reset_phase_ = "clearing_local";
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
    // The reset stopped the tutorial worker before clearing NVS. If it failed
    // before the reboot, the machine keeps running, so give the tutorial back.
    ResumeOnboardingAfterAbort();
    voice_tts::EndVoiceReset();
    if (options_.end_factory_reset)
        options_.end_factory_reset();
    reset_running_.store(false);
}

esp_err_t UserWebPortal::SetupStatus(httpd_req_t* req, uint32_t ip, bool authorized,
                                     std::string_view job) {
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root)
        return httpd_resp_send_500(req);
    std::string context;
    JobPhase phase;
    int failure_reason;
    bool allowed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        allowed = job.empty() ? authorized
                              : UserWebPolicy::SameApClient(setup_.ip, ip) && setup_.id == job;
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
            const char* message = "恢复失败，已保留 Wi-Fi，请重试。";
            if (error_code == "RESET_NOT_ALLOWED")
                message = "设备未能暂停当前操作，已保留 Wi-Fi，请稍后重试。";
            else if (error_code == "RESET_VOICE_BUSY")
                message = "语音服务正在处理，已保留 Wi-Fi，请稍后重试。";
            else if (error_code == "RESET_VOICE_CONFIG")
                message = "有待清理的复刻音色，已保留 Wi-Fi。请检查语音服务 Key 后重试。";
            else if (error_code == "RESET_VOICE_OFFLINE")
                message = "清理复刻音色需要联网，已保留 Wi-Fi。请恢复网络后重试。";
            else if (error_code == "RESET_VOICE_CLOUD_FAILED")
                message = "云端音色清理失败，已保留 Wi-Fi。请检查语音服务后重试。";
            else if (error_code == "RESET_VOICE_UNCERTAIN")
                message = "仍有待确认的音色创建，已保留 Wi-Fi。请稍后重试。";
            else if (error_code == "RESET_VOICE_SAVE_FAILED")
                message = "本地音色记录处理失败，已保留 Wi-Fi，请重试。";
            else if (error_code == "RESET_ONBOARDING_FAILED")
                message = "新手教程进度清理失败，已保留 Wi-Fi，请重试。";
            else if (error_code == "RESET_WIFI_FAILED")
                message = "Wi-Fi 凭据清理失败，请重试。";
            else if (error_code == "RESET_UNAVAILABLE")
                message = "设备暂时无法启动恢复任务，请稍后重试。";
            cJSON_AddStringToObject(error, "message", message);
        }
    }
    return JsonResponse(req, out.get());
}

esp_err_t UserWebPortal::StartFactoryReset(httpd_req_t* req) {
    auto body = ReadBody(req);
    const char* confirm = body ? String(body.get(), "confirm") : nullptr;
    std::string phrase = confirm ? confirm : "";
    const auto first = phrase.find_first_not_of(" \t\r\n");
    const auto last = phrase.find_last_not_of(" \t\r\n");
    phrase = first == std::string::npos ? "" : phrase.substr(first, last - first + 1);
    if (phrase != "确定恢复出厂设置")
        return Error(req, "400 Bad Request", "INVALID_CONFIRMATION", "请输入完整的确认文字");
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reset_phase_ = "failed";
            reset_error_ = "RESET_UNAVAILABLE";
        }
        voice_tts::EndVoiceReset();
        if (options_.end_factory_reset)
            options_.end_factory_reset();
        reset_running_.store(false);
        return Error(req, "503 Service Unavailable", "RESET_UNAVAILABLE", "设备忙，请稍后重试");
    }
    return Ok(req, "202 Accepted");
}

esp_err_t UserWebPortal::Handle(httpd_req_t* req) {
    const auto path = Path(req);
    const bool static_asset =
        req->method == HTTP_GET && (path.rfind("/css/", 0) == 0 || path.rfind("/js/", 0) == 0 ||
                                    path.rfind("/assets/", 0) == 0);
    if (!static_asset)
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    uint32_t ip = 0;
    if (!Peer(req, ip) || !UserWebPolicy::IsApClient(ip))
        return Error(req, "403 Forbidden", "FORBIDDEN", "请连接设备热点");
    if (req->method == HTTP_GET) {
        // Captive probes and the page's assets do not depend on provisioning or NVS.
        const auto host = Header(req, "Host", 128);
        if ((!host.empty() && host != "192.168.4.1" && host != "192.168.4.1:80") || Captive(path))
            return Redirect(req, "http://192.168.4.1/");
        if (static_asset)
            return SendWebAsset(req, path.substr(1));
    }
    // Only a page navigation proves the phone reached the setup page. /api/*
    // polling and cached css/js/assets would make the tutorial's "打开页面"
    // step look satisfied before the user did anything.
    if (req->method == HTTP_GET && !static_asset &&
        (path == "/" || path == "/setup" || path == "/login" || path == "/done" ||
         path == "/manual"))
        NotifyPortalRequest();
    const auto now = Now();
    const bool session = HasSession(req, ip, now);
    auto& wifi = WifiManager::GetInstance();
    auto& board = Board::GetInstance();
    const bool connected = wifi.IsConnected();
    const bool cellular = board.UsesCellularNetwork();
    // The station may connect before the dinosaur has selected its transport.
    // Only the board knows whether that connection is usable for the product.
    const bool selected_connected = board.IsNetworkAvailable();
    const bool cellular_connected = cellular && selected_connected;
    if (selected_connected)
        last_online_ms_.store(now);
    const auto last_online = last_online_ms_.load();
    const bool online = selected_connected || (last_online && now - last_online < 3000);
    const bool have_saved = SsidManager::GetInstance().HasSavedNetworks();
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    constexpr bool retain_offline = true;
    const bool wifi_setup_available = !cellular && !board.IsNetworkProbing() &&
                                      board.GetNetworkTransportSwitchStatus().state !=
                                          NetworkTransportSwitchStatus::State::kPending;
#else
    const bool retain_offline = have_saved;
#endif
    const bool local_accessible =
        UserWebPolicy::LocalAccessible(online, cellular, session, retain_offline);
    const bool offline_login =
        UserWebPolicy::AllowOfflineLogin(retain_offline, online, cellular, session);
    if (req->method == HTTP_POST &&
        !UserWebPolicy::ValidOrigin(Header(req, "Host", 128), Header(req, "Origin", 128)))
        return Error(req, "403 Forbidden", "BAD_ORIGIN", "请求来源不允许");

    if (req->method == HTTP_GET) {
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
            cJSON_AddBoolToObject(out.get(), "authenticated", session);
            cJSON_AddBoolToObject(out.get(), "conversation_intro",
                                  IsOnboardingConversationIntroActive());
            cJSON_AddBoolToObject(out.get(), "onboarding_blocked", OnboardingBlocksConversation());
            cJSON_AddBoolToObject(out.get(), "conversation_recovering",
                                  Application::GetInstance().IsConversationRecovering());
            cJSON_AddBoolToObject(out.get(), "conversation_configured",
                                  Application::GetInstance().HasConversationServiceConfiguration());
            cJSON_AddBoolToObject(out.get(), "setup_guide_done", SetupGuideDone());
            cJSON_AddStringToObject(out.get(), "startup_notice", StartupProtectionMessage());
            if (cellular) {
                auto* cell = cJSON_AddObjectToObject(out.get(), "cellular");
                if (cell) {
                    cJSON_AddBoolToObject(cell, "connected", cellular_connected);
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                    cJSON_AddBoolToObject(
                        cell, "server_connected",
                        cellular_connected &&
                            Application::GetInstance().IsCellularServerConnected());
#endif
                }
            }
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
            AddNetworkTransportStatus(out.get(), board);
#endif
            auto* wireless = cJSON_AddObjectToObject(out.get(), "wifi");
            if (wireless) {
                cJSON_AddStringToObject(wireless, "ap_ssid", wifi.GetApSsid().c_str());
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
        if (path == "/api/network/transport") {
            if (!session)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
            Json out(cJSON_CreateObject(), cJSON_Delete);
            if (!out)
                return httpd_resp_send_500(req);
            cJSON_AddBoolToObject(out.get(), "ok", true);
            AddNetworkTransportStatus(out.get(), board);
            cJSON_AddBoolToObject(out.get(), "switchable", board.CanSwitchNetworkTransport());
            return JsonResponse(req, out.get());
        }
#endif
        if (path == "/api/user/onboarding/status") {
            if (local_accessible && !session)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
            // Read-only diagnostic: which chapter is done, which step is next.
            // Mutating this state stays with the device itself — the portal's
            // "guide/setup-complete" endpoint only mirrors the summary bit and
            // refuses to run ahead of actual playback.
            OnboardingProgress progress;
            if (!PeekOnboardingProgress(progress))
                return Error(req, "500 Internal Server Error", "READ_FAILED", "读取教程状态失败");
            Json out(cJSON_CreateObject(), cJSON_Delete);
            if (!out)
                return httpd_resp_send_500(req);
            cJSON_AddBoolToObject(out.get(), "ok", true);
            cJSON_AddBoolToObject(out.get(), "done", progress.Done());
            cJSON_AddBoolToObject(out.get(), "active", IsOnboardingActive());
            cJSON_AddBoolToObject(out.get(), "offline_chapter_done",
                                  (progress.flags & OnboardingProgress::kOfflineComplete) != 0);
            cJSON_AddBoolToObject(out.get(), "online_chapter_done",
                                  (progress.flags & OnboardingProgress::kOnlineComplete) != 0);
            cJSON_AddNumberToObject(out.get(), "next_step", progress.step);
            cJSON_AddNumberToObject(out.get(), "step_count", OnboardingProgress::kStepCount);
            cJSON_AddNumberToObject(out.get(), "attempts", progress.attempts);
            // The countdown is a Kconfig-compiled feature; report whether this
            // firmware actually has it so the portal never promises it blindly.
#if CONFIG_WZH_IDLE_POWER_ENABLE
            cJSON_AddTrueToObject(out.get(), "idle_countdown");
#else
            cJSON_AddFalseToObject(out.get(), "idle_countdown");
#endif
            return JsonResponse(req, out.get());
        }
        if (path == "/api/user/voices/audition/status" ||
            path == "/api/user/voices/device-record/status") {
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
            if (!session)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
            Json out(cJSON_CreateObject(), cJSON_Delete);
            if (!out)
                return httpd_resp_send_500(req);
            cJSON_AddBoolToObject(out.get(), "ok", true);
            if (path == "/api/user/voices/audition/status") {
                cJSON_AddStringToObject(out.get(), "phase",
                                        Application::GetInstance().ExternalVoicePreviewStatus());
                return JsonResponse(req, out.get());
            }
            cJSON_AddStringToObject(out.get(), "phase",
                                    Application::GetInstance().DeviceVoiceEnrollmentStatus());
            cJSON_AddNumberToObject(out.get(), "request",
                                    Application::GetInstance().DeviceVoiceEnrollmentRequest());
            cJSON_AddStringToObject(
                out.get(), "failure",
                options_.voice_enrollment_failure ? options_.voice_enrollment_failure() : "");
            return JsonResponse(req, out.get());
#else
            return Error(req, "404 Not Found", "NOT_FOUND", "声音服务不可用");
#endif
        }
        if (path == "/api/user/voices/manage/status") {
            if (!session)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
            if (!options_.voice_status_summary)
                return Error(req, "404 Not Found", "NOT_FOUND", "声音服务不可用");
            Json out(cJSON_CreateObject(), cJSON_Delete);
            if (!out)
                return httpd_resp_send_500(req);
            options_.voice_status_summary(out.get());
            cJSON_AddBoolToObject(out.get(), "ok", true);
            return JsonResponse(req, out.get());
        }
        if (path == "/api/user/voices" || path == "/api/user/voices/clone/status") {
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
            if (!session)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
            if (!online)
                return Error(req, "409 Conflict", "NET_OFFLINE", "设备暂未联网");
#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC && CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
            if (!voice_tts::VoiceHttpNetworkAvailable())
#else
            if (!Board::GetInstance().HasSystemNetwork())
#endif
                return Error(req, "409 Conflict", "NET_OFFLINE", "声音管理需要可用的数据网络");
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
                auto* voices = cJSON_AddArrayToObject(out.get(), "voices");
                auto* native = voices ? cJSON_CreateObject() : nullptr;
                if (!native)
                    return httpd_resp_send_500(req);
                cJSON_AddStringToObject(native, "id", "xiaozhi-native");
                cJSON_AddStringToObject(native, "name", "原声");
                cJSON_AddBoolToObject(native, "builtin", true);
                const auto selected = options_.ali_voice ? options_.ali_voice() : "";
                cJSON_AddBoolToObject(native, "current",
                                      selected.empty() || selected == "xiaozhi-native");
                cJSON_AddItemToArray(voices, native);
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
            if (!cJSON_IsTrue(busy) && !Application::GetInstance().IsExternalVoicePreviewActive() &&
                options_.voice_command &&
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
                    cJSON_AddBoolToObject(
                        item, "current",
                        id && (current == id ||
                               (current.empty() && std::strcmp(id, "xiaozhi-native") == 0)));
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
                httpd_query_key_value(query, "job", job, sizeof(job)) != ESP_OK || !*job)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请重新连接设备热点");
            return SetupStatus(req, ip, false, job);
        }
        if (path == "/scan") {
            if ((local_accessible || have_saved) && !session)
                return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
            Json out(cJSON_CreateObject(), cJSON_Delete);
            if (!out)
                return httpd_resp_send_500(req);
            cJSON_AddBoolToObject(out.get(), "ok", true);
            auto* array = cJSON_AddArrayToObject(out.get(), "aps");
            if (array && portal_
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                && wifi_setup_available
#endif
            ) {
                const auto saved = SsidManager::GetInstance().GetSsidList();
                const auto current = connected ? wifi.GetSsid() : "";
                portal_->VisitAccessPoints([&](const WifiConfigurationAp::PortalAccessPoint& point) {
                    auto* item = cJSON_CreateObject();
                    if (!item)
                        return false;
                    const std::string_view name(point.ssid);
                    cJSON_AddStringToObject(item, "ssid", point.ssid);
                    cJSON_AddNumberToObject(item, "rssi", point.rssi);
                    cJSON_AddNumberToObject(item, "auth", point.authmode);
                    cJSON_AddBoolToObject(
                        item, "saved",
                        std::any_of(saved.begin(), saved.end(),
                                    [&](const SsidItem& entry) { return entry.ssid == name; }));
                    cJSON_AddBoolToObject(item, "current", !current.empty() && current == name);
                    cJSON_AddItemToArray(array, item);
                    return true;
                });
                for (const auto& entry : saved) {
                    const cJSON* listed = nullptr;
                    cJSON_ArrayForEach(listed, array) {
                        const auto* name = cJSON_GetObjectItemCaseSensitive(listed, "ssid");
                        if (cJSON_IsString(name) && entry.ssid == name->valuestring)
                            break;
                    }
                    if (entry.ssid.empty() || listed)
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                wifi_setup_available &&
#endif
                (!last_scan || now - last_scan >= 2000)) {
                // Ask the manager rather than poking the driver directly. On a
                // first boot the station may not be running yet; the manager
                // starts it so its scan callback can fill the portal cache.
                if (wifi.RequestScan())
                    last_scan_ms_.store(now);
            }
            return JsonResponse(req, out.get());
        }
        if (path == "/" || path == "/setup" || path == "/login" || path == "/manual" ||
            path == "/done") {
            // A phone may obtain a new AP address after a channel change.
            // Allow it to authenticate locally even when the selected Wi-Fi
            // network is offline, so it can switch back to cellular.
            if (path == "/login" && offline_login)
                return Html(req);
            using Network = UserWebPolicy::Network;
            using Route = UserWebPolicy::Route;
            const auto network = local_accessible        ? Network::kOnline
                                 : !have_saved           ? Network::kUnprovisioned
                                 : setup_running_.load() ? Network::kConnecting
                                                         : Network::kOffline;
            const auto route = UserWebPolicy::RouteFor(
                path, network, session, path == "/done" && HasCompletion(req, ip, now),
                offline_login);
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    // Runtime 4G/Wi-Fi switch. Runs before the NET_OFFLINE gate below: the
    // selected transport may currently be offline, and switching is exactly
    // how the user recovers, so a switch must not require being online first.
    if (path == "/api/network/transport") {
        if (!session)
            return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
        if (setup_running_.load())
            return Error(req, "409 Conflict", "SETUP_BUSY", "正在连接 Wi-Fi，请稍后切换网络");
        auto body = ReadBody(req);
        const char* transport = body ? String(body.get(), "transport") : nullptr;
        const bool cellular = transport && std::strcmp(transport, "cellular") == 0;
        const bool wifi = transport && std::strcmp(transport, "wifi") == 0;
        if (!cellular && !wifi)
            return Error(req, "400 Bad Request", "BAD_TRANSPORT",
                         "transport 必须是 wifi 或 cellular");
        if (!board.CanSwitchNetworkTransport())
            return Error(req, "409 Conflict", "TRANSPORT_UNAVAILABLE", "当前版本只支持 Wi-Fi");
        Json out(cJSON_CreateObject(), cJSON_Delete);
        if (!out)
            return httpd_resp_send_500(req);
        if (!board.SelectNetworkTransport(cellular))
            return Error(req, "409 Conflict", "TRANSPORT_BUSY", "设备正在处理联网，请稍后重试");
        cJSON_AddBoolToObject(out.get(), "ok", true);
        AddNetworkTransportStatus(out.get(), board);
        return JsonResponse(req, out.get(), "202 Accepted");
    }
#endif
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
        return SetupStatus(req, ip, completion || session);
    }
    if (path == "/api/login") {
        if (!local_accessible && !retain_offline)
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
        if (session)
            return Ok(req);  // The previous response may have set the cookie before reconnect.
        auto body = ReadBody(req);
        const char* value = body ? String(body.get(), "ctx") : nullptr;
        const std::string completion =
            (value && *value) ? std::string(value) : Cookie(req, "wzh_done");
        if (completion.empty())
            return Error(req, "400 Bad Request", "INVALID_INPUT", "完成页已失效");
        std::string token;
        bool valid, waiting;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waiting = setup_.phase == JobPhase::kOnline && !online &&
                      auth_.CheckCompletion(ip, completion, now);
            valid = setup_.phase == JobPhase::kOnline &&
                    auth_.Enter(ip, completion, Random(), now, online, token);
        }
        if (waiting)
            return Error(req, "409 Conflict", "NET_RECONNECTING", "设备正在恢复网络，请稍候");
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
        if (local_accessible && !session)
            return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
        // The tutorial owns its own completion bit: only actual playback of both
        // chapters may set it. A portal hint must not fake progress, so this
        // endpoint acknowledges the click and reports the real state instead of
        // writing here.
        Json out(cJSON_CreateObject(), cJSON_Delete);
        if (!out)
            return httpd_resp_send_500(req);
        cJSON_AddBoolToObject(out.get(), "ok", true);
        cJSON_AddBoolToObject(out.get(), "guide_done", SetupGuideDone());
        return JsonResponse(req, out.get());
    }
    if (path == "/submit") {
        if ((local_accessible || have_saved) && !session)
            return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
        if (!wifi_setup_available)
            return Error(req, "409 Conflict", "WIFI_NOT_SELECTED", "请先切换到 Wi-Fi 模式后再连接");
#endif
        auto body = ReadBody(req);
        const char* ssid = body ? String(body.get(), "ssid") : nullptr;
        const char* password = body ? String(body.get(), "password") : nullptr;
        if (!ssid || !*ssid || std::strlen(ssid) > 32 || !password || std::strlen(password) > 64)
            return Error(req, "400 Bad Request", "INVALID_INPUT", "请检查网络名称和密码");
        // Accept a browser-generated token so a channel change that drops the
        // submit response does not prevent that browser from finding its job.
        const auto* job_id = cJSON_GetObjectItemCaseSensitive(body.get(), "job");
        const char* supplied = String(body.get(), "job");
        if (job_id && (!supplied || std::strlen(supplied) != 32 ||
                       std::strspn(supplied, "0123456789abcdef") != 32)) {
            return Error(req, "400 Bad Request", "INVALID_INPUT", "配网任务无效，请重试");
        }
        const std::string job = supplied ? supplied : Token();
        Json out(cJSON_CreateObject(), cJSON_Delete);
        if (!out || !cJSON_AddBoolToObject(out.get(), "ok", true) ||
            !cJSON_AddStringToObject(out.get(), "job", job.c_str()))
            return httpd_resp_send_500(req);
        bool expected = false;
        if (!setup_running_.compare_exchange_strong(expected, true))
            return Error(req, "409 Conflict", "SETUP_BUSY", "正在连接网络，请稍候");
        if (reset_running_.load() || voice_upload_running_.load()) {
            setup_running_.store(false);
            return Error(req, "503 Service Unavailable", "RESET_BUSY", "恢复出厂进行中，请稍候");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            setup_ = {};
            setup_.phase = JobPhase::kConnecting;
            setup_.ip = ip;
            setup_.id = job;
            setup_.ssid = ssid;
            setup_.password = password;
        }
        if (xTaskCreate(SetupTask, "wzh_setup", 8192, this, 3, nullptr) != pdPASS) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                setup_ = {};
                setup_running_.store(false);
            }
            return Error(req, "503 Service Unavailable", "SETUP_UNAVAILABLE", "设备忙，请稍后重试");
        }
        return JsonResponse(req, out.get(), "202 Accepted");
    }
    if (!session)
        return Error(req, "401 Unauthorized", "AUTH_REQUIRED", "请先登录");
    if (path == "/api/user/factory-reset")
        return StartFactoryReset(req);
    if (path == "/api/user/voices/device-record/cancel") {
        auto body = ReadBody(req);
        const auto* request =
            body ? cJSON_GetObjectItemCaseSensitive(body.get(), "request") : nullptr;
        if (!cJSON_IsNumber(request) || !std::isfinite(request->valuedouble) ||
            request->valuedouble < 1 || request->valuedouble > UINT32_MAX ||
            std::floor(request->valuedouble) != request->valuedouble)
            return Error(req, "400 Bad Request", "VOICE_INVALID", "录音任务无效");
        Application::GetInstance().RequestCancelDeviceVoiceEnrollment(
            static_cast<uint32_t>(request->valuedouble));
        return Ok(req, "202 Accepted");
    }
    if (path.rfind("/api/user/voices/", 0) == 0) {
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
        if (!online)
            return Error(req, "409 Conflict", "NET_OFFLINE", "设备暂未联网");
#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC && CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
        if (!voice_tts::VoiceHttpNetworkAvailable())
#else
        if (!Board::GetInstance().HasSystemNetwork())
#endif
            return Error(req, "409 Conflict", "NET_OFFLINE", "声音管理需要可用的数据网络");
        const auto action = path.substr(std::strlen("/api/user/voices/"));
        if (action == "native") {
            auto body = ReadBody(req);
            const char* id = body ? String(body.get(), "id") : nullptr;
            if (!id || std::strcmp(id, "xiaozhi-native") != 0)
                return Error(req, "400 Bad Request", "VOICE_INVALID", "音色无效");
            if (reset_running_.load() || !options_.save_ali_voice ||
                !options_.save_ali_voice("xiaozhi-native"))
                return Error(req, "409 Conflict", "VOICE_BUSY", "设备正忙，请稍后重试");
            return Ok(req);
        }
        if (action == "audition") {
            auto body = ReadBody(req);
            const char* id = body ? String(body.get(), "id") : nullptr;
            if (!id || !*id || std::strlen(id) > 128)
                return Error(req, "400 Bad Request", "VOICE_INVALID", "音色无效");
            if (std::strcmp(id, "xiaozhi-native") == 0)
                return Error(req, "409 Conflict", "VOICE_PREVIEW_NATIVE", "原声会在正常对话中播放");
            if (!options_.has_ali_key || !options_.has_ali_key())
                return Error(req, "409 Conflict", "VOICE_NO_KEY", "语音服务未配置");
            if (!KnownVoice(id, options_.voice_status_extra))
                return Error(req, "400 Bad Request", "VOICE_INVALID", "音色不存在");
            if (setup_running_.load() || voice_upload_running_.load() || reset_running_.load() ||
                !Application::GetInstance().RequestExternalVoicePreview(id))
                return Error(req, "409 Conflict", "VOICE_BUSY", "设备正忙，请稍后试听");
            return Ok(req, "202 Accepted");
        }
        if (!options_.has_ali_key || !options_.has_ali_key())
            return Error(req, "409 Conflict", "VOICE_NO_KEY", "语音服务未配置");
        if (action == "device-record") {
            auto body = ReadBody(req);
            const char* name = body ? String(body.get(), "name") : nullptr;
            if (!name || !voice_tts::IsValidVoiceName(name))
                return Error(req, "400 Bad Request", "VOICE_INVALID", "请输入有效的音色名称");
            Json out(cJSON_CreateObject(), cJSON_Delete);
            if (!out)
                return httpd_resp_send_500(req);
            if (setup_running_.load() || voice_upload_running_.load() || reset_running_.load() ||
                voice_tts::VoiceResetActive() || !options_.voice_device_record ||
                !options_.voice_device_record(name))
                return Error(req, "409 Conflict", "VOICE_BUSY", "设备正忙，请稍后重试");
            cJSON_AddBoolToObject(out.get(), "ok", true);
            cJSON_AddNumberToObject(out.get(), "request",
                                    Application::GetInstance().DeviceVoiceEnrollmentRequest());
            return JsonResponse(req, out.get(), "202 Accepted");
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
        if (action != "select" && action != "rename" && action != "delete")
            return Error(req, "404 Not Found", "NOT_FOUND", "操作不存在");
        if (action == "rename" && (!name || !voice_tts::IsValidVoiceName(name)))
            return Error(req, "400 Bad Request", "VOICE_INVALID", "请输入有效名称");
        Json out(cJSON_CreateObject(), cJSON_Delete);
        if (!out)
            return httpd_resp_send_500(req);
        const auto ticket =
            options_.voice_command ? options_.voice_command(action, id, name ? name : "") : 0;
        if (!ticket)
            return Error(req, "409 Conflict", "VOICE_BUSY", "设备正忙，请稍后重试");
        cJSON_AddBoolToObject(out.get(), "ok", true);
        cJSON_AddNumberToObject(out.get(), "request", ticket);
        return JsonResponse(req, out.get(), "202 Accepted");
#else
        return Error(req, "404 Not Found", "NOT_FOUND", "声音服务不可用");
#endif
    }
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
    if (path == "/api/user/chat/stop") {
        Application::GetInstance().Schedule([] {
            auto& app = Application::GetInstance();
            const auto state = app.GetDeviceState();
            if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
                state == kDeviceStateSpeaking || app.IsConversationRecovering())
                app.StopInteraction(true);
        });
        return Ok(req);
    }
    if (path == "/api/user/chat/start" || path == "/api/user/chat/toggle") {
        const bool start_only = path == "/api/user/chat/start";
        auto& app = Application::GetInstance();
        const auto state = app.GetDeviceState();
        if (start_only && (state == kDeviceStateConnecting || state == kDeviceStateListening ||
                           state == kDeviceStateSpeaking || app.IsConversationRecovering()))
            return Ok(req);
        if (!selected_connected)
            return Error(req, "409 Conflict", "NET_OFFLINE", "设备正在等待联网，可以先离线玩耍");
        // The offline tutorial chapter has no saved network, so opening a channel
        // from the portal would fail silently; the keyboard paths do the same.
        if (OnboardingBlocksConversation())
            return Error(req, "409 Conflict", "GUIDE_BUSY", "教程进行中，请先完成教程");
        if (state == kDeviceStateIdle &&
            !Application::GetInstance().HasConversationServiceConfiguration())
            return Error(req, "503 Service Unavailable", "CHAT_UNAVAILABLE",
                         "对话服务暂未就绪，可以先离线玩耍");
        if (state != kDeviceStateIdle && state != kDeviceStateListening &&
            state != kDeviceStateSpeaking)
            return Error(req, "409 Conflict", "DEVICE_BUSY", "设备正在忙");
        // Never run the audio/state-machine work on the HTTP task.
        Application::GetInstance().Schedule([start_only] {
            auto& app = Application::GetInstance();
            if (!start_only ||
                (app.GetDeviceState() == kDeviceStateIdle && !app.IsConversationRecovering()))
                app.ToggleConversation();
        });
        return Ok(req);
    }
    return Error(req, "404 Not Found", "NOT_FOUND", "页面不存在");
}
}  // namespace wzh
