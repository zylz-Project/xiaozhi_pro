#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <esp_http_server.h>
#include "wzh_user_web_policy.h"
#include "wzh_web_console.h"

class WifiConfigurationAp;

namespace wzh {
// User portal is a set of handlers on the provisioning server, not another
// listener or AP owner. Its lifetime is the lifetime of the board console.
class UserWebPortal {
public:
    explicit UserWebPortal(WebConsole::Options& options) : options_(options) {}
    bool Register(httpd_handle_t server, WifiConfigurationAp* portal);
    bool ResetActive() const { return reset_running_.load(); }
    // Transport-neutral entry points. The HTTP handler validates the
    // confirmation phrase and maps code/message onto a status response; the BLE
    // transport calls the bool version directly.
    bool StartFactoryReset(std::string& code, std::string& message);
    void FactoryResetStatus(cJSON* out);
    esp_err_t StartFactoryReset(httpd_req_t* req);
    esp_err_t FactoryResetStatus(httpd_req_t* req);

private:
    enum class JobPhase { kIdle, kConnecting, kOnline, kFailed };
    struct SetupJob {
        JobPhase phase = JobPhase::kIdle;
        uint32_t ip = 0;
        std::string id;
        std::string context;
        std::string ssid;
        std::string password;
        int failure_reason = 0;
    };
    static esp_err_t Dispatch(httpd_req_t* req);
    static void SetupTask(void* arg);
    static void ResetTask(void* arg);
    struct UploadJob;
    static void UploadTask(void* arg);
    esp_err_t Handle(httpd_req_t* req);
    esp_err_t SetupStatus(httpd_req_t* req, uint32_t ip, bool has_session);
    bool HasSession(httpd_req_t* req, uint32_t ip, uint64_t now);
    bool HasCompletion(httpd_req_t* req, uint32_t ip, uint64_t now);
    void RunSetup();
    void RunReset();

    WebConsole::Options& options_;
    WifiConfigurationAp* portal_ = nullptr;
    std::mutex mutex_;
    UserWebPolicy auth_;
    SetupJob setup_;
    std::atomic<bool> setup_running_{false};
    std::atomic<bool> reset_running_{false};
    std::atomic<bool> voice_upload_running_{false};
    std::string reset_phase_ = "idle";
    std::string reset_error_;
    size_t reset_current_ = 0;
    size_t reset_total_ = 0;
    uint64_t voice_last_request_ms_ = 0;
    std::atomic<uint64_t> last_online_ms_{0};
    std::atomic<uint64_t> last_scan_ms_{0};
};
}  // namespace wzh
