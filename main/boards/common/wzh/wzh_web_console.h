#ifndef WZH_WEB_CONSOLE_H_
#define WZH_WEB_CONSOLE_H_

#include <atomic>
#include <functional>
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

// 设备自建热点 + 本地管理页。
//
// 与配网门户分离：配网仍在 80 端口（WifiConfigurationAp），本服务在
// 8080 端口，并在 STA 已联网时额外开启一个 APSTA 热点。进入配网前由
// WifiManager 的 before-config 回调调用 Stop()，避免与配网 AP 争用
// WIFI_AP_DEF 网络接口。
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
    };

    WebConsole() = default;
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
    std::mutex lifecycle_mutex_;
    std::atomic<bool> started_{false};
    esp_netif_t* ap_netif_ = nullptr;
    httpd_handle_t server_ = nullptr;
    std::string ap_ssid_;
    std::string ap_password_;
    DeveloperSession developer_;
    std::atomic<bool> maintenance_active_{false};
    std::atomic<bool> hub_maintenance_reserved_{false};
    std::mutex job_mutex_;
    bool erase_ok_ = false;
    std::string erase_message_ = "尚未执行擦除";
};

}  // namespace wzh

#endif  // WZH_WEB_CONSOLE_H_
