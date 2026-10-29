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

// Audio Hub 云端同步客户端（参考工程 device_registry + sync_audio 的移植）。
//
// 全部工作在独立低优先级任务中进行：注册 → 轮询激活 → 心跳 → 增量同步。
// 不阻塞小智联网与对话；对话进行中（非 Idle 状态）推迟下载；服务不可达
// 时指数退避；下载失败保留旧声音；Hub 不可用时设备完整离线运行。
//
// 协议（Bearer token 认证，除 register/activate 外）：
//   POST /api/device/register           → {claim_token, activation_code}
//   POST /api/device/activate           → {api_token}（401/410 → 重新注册）
//   POST /api/device/v1/check-in        每 60s 心跳（401/403 → 回到激活轮询）
//   GET  /api/device/v1/files           → {revision, files:[{name,size,category}]}
//   GET  /api/device/v1/download/<index>（index = 清单数组下标）流式下载
// 设备 ID：ZYLZ- + 6 字节 STA MAC。Token 存 NVS：audio_hub（api_token/
// claim_token/act_code/server），audio_sync（revision）。
class AudioHub {
public:
    enum class Status {
        kDisabled,
        kWaitingNetwork,
        kRegistering,
        kWaitingActivation,
        kActive,
        kSyncing,
        kError,
    };

    static const char* StatusName(Status status);

    // 启动后台任务（Kconfig 关闭时返回 false 且状态为 kDisabled）。
    bool Start(AudioStore* store, std::function<bool()> interaction_busy = {});

    // 手动模式：只创建等待命令的任务，不注册、不心跳、不自动检查或下载。
    // 供本地管理页显式触发一次检查/更新。
    bool StartManual(AudioStore* store, std::function<bool()> interaction_busy = {});

    struct DiffItem {
        std::string name;
        std::string state;  // new / changed / same / local
        std::string category;
        uint32_t local_size = 0;
        uint32_t remote_size = 0;
    };

    bool manual() const { return manual_mode_; }
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
    // 手动触发一次同步（MCP sync_now）；在下一个循环周期生效。
    void RequestSyncNow() { sync_now_.store(true); }

private:
    struct RemoteFile {
        std::string name;       // 含 .opus 后缀（清单原样）
        std::string base_name;  // 去掉 .opus 后的目录名
        uint32_t size = 0;
        std::string category;
    };

    static void TaskEntry(void* arg);
    void Task();
    void ManualTask();
    void DoCheck();
    void DoUpdate();
    bool InitializeClient();
    void SetJobMessage(const std::string& message);
    void SleepBackoff(int* backoff_s);

    std::string ServerUrl() const;
    bool Register();
    bool TryActivate(int* status_code);
    bool CheckIn();
    bool FetchManifest(std::string* body);
    bool ParseManifest(const std::string& body, std::vector<RemoteFile>* files,
                       std::string* revision);
    bool DownloadFile(int index, const RemoteFile& file);
    bool SyncOnce();

    mutable std::mutex metadata_mutex_;
    AudioStore* store_ = nullptr;
    std::function<bool()> interaction_busy_;
    std::string device_id_;
    std::string server_;
    std::string api_token_;
    std::string claim_token_;
    std::string activation_code_;
    std::string revision_;
    // During a changed revision, equal size does not prove equal content.
    // Remember completed entries for in-process retries; verified Flash commits
    // remain the source of truth after a reboot, without changing the NVS format.
    std::string refresh_revision_;
    uint32_t refreshed_files_ = 0;
    std::atomic<Status> status_{Status::kWaitingNetwork};
    std::atomic<bool> activated_{false};
    std::atomic<bool> sync_now_{false};
    std::atomic<bool> syncing_{false};
    bool manual_mode_ = false;
    std::mutex init_mutex_;
    bool client_initialized_ = false;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> check_requested_{false};
    std::atomic<bool> update_requested_{false};
    std::atomic<bool> job_running_{false};
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
