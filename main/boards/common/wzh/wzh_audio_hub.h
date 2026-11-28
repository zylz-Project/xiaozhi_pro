#ifndef WZH_AUDIO_HUB_H_
#define WZH_AUDIO_HUB_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "wzh_audio_store.h"

namespace wzh {

// 工厂局域网音频工具。识别工厂服务后登记和上报，检查清单及写入由操作员发起。
//
// 协议（Bearer token 认证，除 register/activate 外）：
//   POST /api/device/register           → {claim_token, activation_code}
//   POST /api/device/activate           → {api_token}（401/410 → 重新注册）
//   POST /api/device/v1/check-in        刷新设备的型号和局域网地址
//   GET  /api/device/v1/files           → {revision, files:[{name,size,category}]}
//   GET  /api/device/v1/download/<index>（index = 清单数组下标）流式下载
// 设备 ID：ZYLZ- + 6 字节 STA MAC。Token 存 NVS：audio_hub（api_token/
// claim_token/act_code/server），audio_sync（revision）。
class AudioHub {
public:
    enum class Status {
        kIdle,
        kWaitingNetwork,
        kRegistering,
        kWaitingActivation,
        kActive,
        kSyncing,
        kError,
    };

    static const char* StatusName(Status status);

    void ConfigureForFactory(AudioStore* store, std::function<bool()> interaction_busy = {});
    void StartLanDiscovery();

    struct DiffItem {
        std::string name;
        std::string state;  // new / changed / same / local
        std::string category;
        uint32_t local_size = 0;
        uint32_t remote_size = 0;
    };

    // Loads NVS-backed server/token state on first use. Safe to call from the
    // console after Wi-Fi/NVS initialization; also used lazily by the worker.
    bool EnsureInitialized();
    bool RequestCheck();
    bool RequestUpdate(const std::vector<std::string>& names);
    bool job_running() const { return job_running_.load(); }
    bool job_ok() const { return job_ok_.load(); }
    std::string job_message() const;
    std::vector<DiffItem> diff() const;
    std::string checked_revision() const;
    bool has_diff() const;
    bool BeginLocalMaintenance();
    void EndLocalMaintenance(bool library_erased);

    Status status() const { return status_.load(); }
    bool activated() const { return activated_.load(); }
    std::string activation_code() const {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        return activation_code_;
    }
    const std::string& device_id() const { return device_id_; }
    std::string revision() const {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        return revision_;
    }

private:
    enum class Job { kCheck, kUpdate, kPresence };
    struct RemoteFile {
        std::string name;       // 含 .opus 后缀（清单原样）
        std::string base_name;  // 去掉 .opus 后的目录名
        uint32_t size = 0;
        std::string category;
    };

    static void TaskEntry(void* arg);
    static void LanDiscoveryTaskEntry(void* arg);
    bool ListenForHub();
    bool RequestJob(Job job, const std::vector<std::string>& names, const std::string& server = {});
    void DoPresence(const std::string& server);
    void DoCheck();
    void DoUpdate();
    bool EnsureRegistered();
    bool InitializeClient();
    void SetJobMessage(const std::string& message);
    void ClearRegistration();
    bool CanDownload() const;

    std::string ServerUrl() const;
    bool Register();
    bool TryActivate(int* status_code);
    bool CheckIn();
    bool FetchManifest(std::string* body);
    bool ParseManifest(const std::string& body, std::vector<RemoteFile>* files,
                       std::string* revision);
    bool DownloadFile(int index, const RemoteFile& file);

    mutable std::mutex metadata_mutex_;
    AudioStore* store_ = nullptr;
    std::function<bool()> interaction_busy_;
    std::string device_id_;
    std::string api_token_;
    std::string claim_token_;
    std::string activation_code_;
    std::string revision_;
    std::atomic<Status> status_{Status::kIdle};
    std::atomic<bool> activated_{false};
    std::mutex init_mutex_;
    bool client_initialized_ = false;
    std::atomic<bool> job_running_{false};
    std::atomic<bool> discovery_running_{false};
    Job pending_job_ = Job::kCheck;   // guarded by job_mutex_
    std::string offered_server_;     // guarded by job_mutex_
    int64_t next_presence_us_ = 0;    // guarded by job_mutex_
    bool local_maintenance_ = false;  // guarded by job_mutex_
    std::atomic<bool> job_ok_{false};
    mutable std::mutex job_mutex_;
    std::string job_message_;
    std::vector<std::string> update_names_;
    std::vector<DiffItem> diff_;
    std::string checked_revision_;
    std::vector<RemoteFile> remote_files_;
};

}  // namespace wzh

#endif  // WZH_AUDIO_HUB_H_
