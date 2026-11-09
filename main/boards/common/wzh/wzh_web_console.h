#ifndef WZH_WEB_CONSOLE_H_
#define WZH_WEB_CONSOLE_H_

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <esp_http_server.h>
#include <esp_netif.h>
#include <cJSON.h>

#include "wzh_audio_hub.h"
#include "wzh_audio_player.h"
#include "wzh_audio_store.h"
#include "wzh_developer_session.h"

namespace wzh {
class UserWebPortal;

// 设备用户门户（:80，常驻开放热点）与开发者管理页（:8080，局域网）。
//
// WZH 常驻门户模式下，:80 与 SoftAP 由 WifiConfigurationAp 持有，用户
// 页面通过 SetPortalRegistrar 注册到同一 server；本服务只负责 :8080，
// 并拒绝 AP 网段客户端。非持久路径保留旧行为：自建 APSTA 热点，进入
// 配网前由 WifiManager 的 before-config 回调调用 Stop()。
class WebConsole {
public:
    struct Options {
        AudioStore* store = nullptr;
        AudioPlayer* player = nullptr;
        AudioHub* hub = nullptr;
        std::string product_name;
        std::string build_name;
        std::string ap_prefix;
        // Board-specific status fields (conversation, servo power, ...).
        std::function<void(cJSON*)> status_extra;
        std::function<void(bool)> maintenance;
        std::function<bool()> center;
        // Optional board-specific credential storage; never include the key in status.
        std::function<bool(const std::string&)> save_ali_key;
        std::function<bool()> has_ali_key;
        std::function<bool(const std::string&)> save_ali_voice;
        std::function<std::string()> ali_voice;
        std::function<std::string()> previous_ali_voice;
        std::function<bool()> restore_ali_voice;
        std::function<const char*()> voice_enrollment_status;
        std::function<const char*()> voice_enrollment_failure;
        std::function<bool()> preview_ali_voice;
        std::function<bool()> cancel_voice;
        std::function<const char*()> voice_preview_status;
        std::function<bool(const std::string&, const std::string&, const std::string&)>
            voice_command;
        std::function<bool(uint8_t*, size_t)> voice_upload;
        std::function<bool(const std::string&)> voice_device_record;
        std::function<void(cJSON*)> voice_status_extra;
        std::function<bool()> begin_factory_reset;
        std::function<bool()> factory_reset_prepared;
        std::function<void()> end_factory_reset;
    };

    WebConsole();
    ~WebConsole();

    void Configure(Options options);
    bool Start();
    void Stop();
    bool started() const { return started_.load(); }
    const std::string& ap_ssid() const { return ap_ssid_; }
    bool maintenance_active() const { return maintenance_active_.load(); }

private:
    static void StartTaskEntry(void* arg);
    void StartTask();
    bool StartHttpServer();
    void StopHttpServer();
    bool EnsureAp();
    void DestroyAp();

    struct RouteSlot {
        WebConsole* owner = nullptr;
        esp_err_t (*handler)(httpd_req_t*) = nullptr;
    };
    static esp_err_t GuardedDispatch(httpd_req_t* req);
    static esp_err_t GuardedError(httpd_req_t* req, httpd_err_code_t error);
    static esp_err_t HandleRoot(httpd_req_t* req);
    static esp_err_t HandleStatus(httpd_req_t* req);
    static esp_err_t HandleAudio(httpd_req_t* req);
    static esp_err_t HandlePlay(httpd_req_t* req);
    static esp_err_t HandleStop(httpd_req_t* req);
    static esp_err_t HandleChat(httpd_req_t* req);
    static esp_err_t HandleHubStatus(httpd_req_t* req);
    static esp_err_t HandleHubCheck(httpd_req_t* req);
    static esp_err_t HandleHubDiff(httpd_req_t* req);
    static esp_err_t HandleHubUpdate(httpd_req_t* req);
    static esp_err_t HandleDeveloperLogin(httpd_req_t* req);
    static esp_err_t HandleDeveloperLogout(httpd_req_t* req);
    static esp_err_t HandleDeveloperStatus(httpd_req_t* req);
    static esp_err_t HandleDeveloperVolume(httpd_req_t* req);
    static esp_err_t HandleDeveloperOta(httpd_req_t* req);
    static esp_err_t HandleDeveloperFactoryReset(httpd_req_t* req);
    static esp_err_t HandleDeveloperVoiceConfig(httpd_req_t* req);
    static esp_err_t HandleDeveloperVoiceManage(httpd_req_t* req);
    static esp_err_t HandleDeveloperVoiceUpload(httpd_req_t* req);
    static esp_err_t HandleDeveloperCenter(httpd_req_t* req);
    static esp_err_t HandleDeveloperDemo(httpd_req_t* req);
    static esp_err_t HandleDeveloperErase(httpd_req_t* req);
    bool RequireDeveloper(httpd_req_t* req);
    void StartErase();
    static void EraseTaskEntry(void* arg);
    void EraseTask();
    void FinishErase(bool ok, const std::string& message);

    static bool ReadJsonBody(httpd_req_t* req, cJSON** root);
    static esp_err_t SendJson(httpd_req_t* req, cJSON* root, const char* status = nullptr);
    static esp_err_t SendError(httpd_req_t* req, int status, const char* code);
    void AddStatus(cJSON* root);
    void AddHubStatus(cJSON* root);

    Options options_;
    std::unique_ptr<UserWebPortal> user_portal_;
    std::array<RouteSlot, 24> routes_{};
    size_t route_count_ = 0;
    std::mutex lifecycle_mutex_;
    std::atomic<bool> started_{false};
    esp_netif_t* ap_netif_ = nullptr;
    httpd_handle_t server_ = nullptr;
    std::string ap_ssid_;
    std::string ap_password_;
    DeveloperSession developer_;
    std::atomic<bool> maintenance_active_{false};
    std::atomic<bool> factory_reset_prepared_{false};
    std::atomic<bool> hub_maintenance_reserved_{false};
    std::mutex job_mutex_;
    bool erase_ok_ = false;
    std::string erase_message_ = "尚未执行擦除";
};

}  // namespace wzh

#endif  // WZH_WEB_CONSOLE_H_
