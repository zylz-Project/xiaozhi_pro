#include "wzh_audio_hub.h"
#include "wzh_sound_score.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

#include <esp_app_desc.h>
#include <esp_http_client.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>

#include <wifi_manager.h>
#include "application.h"
#include "settings.h"

#define TAG "WzhAudioHub"

namespace wzh {
namespace {

constexpr int kDownloadRetries = 3;
constexpr int kHttpTimeoutMs = 10000;
constexpr int kDownloadTimeoutMs = 180000;
constexpr int kCheckinIntervalMs = 60000;
constexpr int kSyncIntervalMs = 60 * 60 * 1000;  // 每小时对一次清单
constexpr int kMaxBackoffS = 300;
constexpr size_t kMaxControlResponse = 8192;
constexpr size_t kMaxManifestResponse = 65536;

void ConfigureHttpTrust(esp_http_client_config_t* config) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config->crt_bundle_attach = esp_crt_bundle_attach;
#else
    (void)config;
#endif
}

bool ReadResponse(esp_http_client_handle_t client, size_t limit, std::string* body) {
    body->clear();
    const int64_t declared = esp_http_client_get_content_length(client);
    if (declared > static_cast<int64_t>(limit))
        return false;
    char buffer[1024];
    int length = 0;
    while ((length = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        if (body->size() + static_cast<size_t>(length) > limit) {
            body->clear();
            return false;
        }
        body->append(buffer, length);
    }
    if (length < 0 || !esp_http_client_is_complete_data_received(client)) {
        body->clear();
        return false;
    }
    return true;
}

const char* StatusNameImpl(AudioHub::Status status) {
    switch (status) {
        case AudioHub::Status::kDisabled:
            return "disabled";
        case AudioHub::Status::kWaitingNetwork:
            return "waiting_network";
        case AudioHub::Status::kRegistering:
            return "registering";
        case AudioHub::Status::kWaitingActivation:
            return "waiting_activation";
        case AudioHub::Status::kActive:
            return "active";
        case AudioHub::Status::kSyncing:
            return "syncing";
        case AudioHub::Status::kError:
            return "error";
        default:
            return "unknown";
    }
}

std::string FirmwareVersion() { return esp_app_get_description()->version; }

}  // namespace

const char* AudioHub::StatusName(Status status) { return StatusNameImpl(status); }

bool AudioHub::InitializeClient() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[32];
    snprintf(id, sizeof(id), "ZYLZ-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    device_id_ = id;

    // 服务地址：NVS 可覆盖 Kconfig 默认值（换服务器不用重烧固件）。
    Settings hub("audio_hub", true);
    server_ = hub.GetString("server", CONFIG_WZH_AUDIO_SYNC_SERVER);
    api_token_ = hub.GetString("api_token");
    claim_token_ = hub.GetString("claim_token");
    activation_code_ = hub.GetString("act_code");
    Settings sync("audio_sync", true);
    revision_ = sync.GetString("revision");
    activated_.store(!api_token_.empty());
    return true;
}

bool AudioHub::EnsureInitialized() {
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (client_initialized_) {
        return true;
    }
    client_initialized_ = InitializeClient();
    return client_initialized_;
}

bool AudioHub::Start(AudioStore* store, std::function<bool()> interaction_busy) {
    store_ = store;
    interaction_busy_ = std::move(interaction_busy);
#if !CONFIG_WZH_AUDIO_SYNC_ENABLE
    status_.store(Status::kDisabled);
    ESP_LOGI(TAG, "Audio Hub 同步已在 Kconfig 中关闭");
    return false;
#else
    if (!InitializeClient()) {
        return false;
    }
    sync_now_.store(true);
    manual_mode_ = false;
    if (xTaskCreate(TaskEntry, "wzh_hub", 8192, this, 1, &task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "无法启动 Audio Hub 任务");
        return false;
    }
    ESP_LOGI(TAG, "Audio Hub 已启动: device=%s product=%s server=%s", device_id_.c_str(),
             Product().hub_product_id, server_.c_str());
    return true;
#endif
}

bool AudioHub::StartManual(AudioStore* store, std::function<bool()> interaction_busy) {
    store_ = store;
    interaction_busy_ = std::move(interaction_busy);
    if (manual_mode_ && task_handle_ != nullptr) {
        return true;
    }
    manual_mode_ = true;
    if (xTaskCreate(TaskEntry, "wzh_hub_manual", 8192, this, 1, &task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "无法启动 Audio Hub 手动任务");
        manual_mode_ = false;
        return false;
    }
    ESP_LOGI(TAG, "Audio Hub 手动模式就绪（等待管理页命令）");
    return true;
}

void AudioHub::TaskEntry(void* arg) { static_cast<AudioHub*>(arg)->Task(); }

void AudioHub::SleepBackoff(int* backoff_s) {
    ESP_LOGW(TAG, "失败退避 %d 秒", *backoff_s);
    vTaskDelay(pdMS_TO_TICKS(*backoff_s * 1000));
    *backoff_s = std::min(*backoff_s * 2, kMaxBackoffS);
}

std::string AudioHub::ServerUrl() const {
    // 尾部不带斜杠。
    std::string base = server_;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

bool AudioHub::Register() {
    status_.store(Status::kRegistering);
    const std::string url = ServerUrl() + "/api/device/register";
    std::string body = "{\"device_id\":\"" + device_id_ + "\",\"product_id\":\"" +
                       Product().hub_product_id + "\",\"firmware_version\":\"" + FirmwareVersion() +
                       "\"}";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 2048;
    ConfigureHttpTrust(&config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "xiaozhi-wzh");

    std::string response;
    bool ok = false;
    if (esp_http_client_open(client, body.size()) == ESP_OK) {
        esp_http_client_write(client, body.data(), body.size());
        if (esp_http_client_fetch_headers(client) >= 0) {
            const int status = esp_http_client_get_status_code(client);
            if (status == 200 || status == 201)
                ok = ReadResponse(client, kMaxControlResponse, &response);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok) {
        ESP_LOGW(TAG, "注册请求失败");
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(response.c_str(), response.size());
    if (root == nullptr) {
        return false;
    }
    cJSON* claim = cJSON_GetObjectItem(root, "claim_token");
    cJSON* code = cJSON_GetObjectItem(root, "activation_code");
    cJSON* token = cJSON_GetObjectItem(root, "api_token");
    if (cJSON_IsString(token) && token->valuestring[0] != '\0' &&
        std::strlen(token->valuestring) <= 512) {
        api_token_ = token->valuestring;
        activated_.store(true);
        claim_token_.clear();
        {
            std::lock_guard<std::mutex> lock(metadata_mutex_);
            activation_code_.clear();
        }
        Settings settings("audio_hub", true);
        settings.SetString("api_token", api_token_);
        settings.EraseKey("claim_token");
        settings.EraseKey("act_code");
        ok = true;
    } else if (cJSON_IsString(claim) && cJSON_IsString(code) && claim->valuestring[0] != '\0' &&
               code->valuestring[0] != '\0' && std::strlen(claim->valuestring) <= 512 &&
               std::strlen(code->valuestring) <= 64) {
        claim_token_ = claim->valuestring;
        {
            std::lock_guard<std::mutex> lock(metadata_mutex_);
            activation_code_ = code->valuestring;
        }
        Settings settings("audio_hub", true);
        settings.SetString("claim_token", claim_token_);
        settings.SetString("act_code", activation_code_);
        ESP_LOGI(TAG, "注册成功，激活码: %s（等待用户在平台绑定）", activation_code_.c_str());
        ok = true;
    } else {
        ESP_LOGE(TAG, "注册响应缺少有效字段");
        ok = false;
    }
    cJSON_Delete(root);
    return ok;
}

bool AudioHub::TryActivate(int* status_code) {
    const std::string url = ServerUrl() + "/api/device/activate";
    std::string body =
        "{\"device_id\":\"" + device_id_ + "\",\"claim_token\":\"" + claim_token_ + "\"}";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 2048;
    ConfigureHttpTrust(&config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "xiaozhi-wzh");

    std::string response;
    bool http_ok = false;
    if (esp_http_client_open(client, body.size()) == ESP_OK) {
        esp_http_client_write(client, body.data(), body.size());
        if (esp_http_client_fetch_headers(client) >= 0) {
            *status_code = esp_http_client_get_status_code(client);
            if (*status_code == 200) {
                http_ok = ReadResponse(client, kMaxControlResponse, &response);
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!http_ok) {
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(response.c_str(), response.size());
    if (root == nullptr) {
        return false;
    }
    cJSON* token = cJSON_GetObjectItem(root, "api_token");
    bool activated = false;
    if (cJSON_IsString(token) && token->valuestring[0] != '\0' &&
        std::strlen(token->valuestring) <= 512) {
        api_token_ = token->valuestring;
        Settings settings("audio_hub", true);
        settings.SetString("api_token", api_token_);
        settings.EraseKey("claim_token");
        settings.EraseKey("act_code");
        claim_token_.clear();
        {
            std::lock_guard<std::mutex> lock(metadata_mutex_);
            activation_code_.clear();
        }
        activated = true;
        ESP_LOGI(TAG, "绑定成功，Audio Hub 激活完成");
    }
    cJSON_Delete(root);
    return activated;
}

bool AudioHub::CheckIn() {
    const std::string url = ServerUrl() + "/api/device/v1/check-in";
    std::string body = "{\"firmware_version\":\"" + FirmwareVersion() + "\"}";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 1024;
    ConfigureHttpTrust(&config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", ("Bearer " + api_token_).c_str());

    bool ok = false;
    if (esp_http_client_open(client, body.size()) == ESP_OK) {
        esp_http_client_write(client, body.data(), body.size());
        if (esp_http_client_fetch_headers(client) >= 0) {
            const int status = esp_http_client_get_status_code(client);
            ok = status == 200;
            if (status == 401 || status == 403) {
                ESP_LOGW(TAG, "check-in 返回 %d，token 失效，回到激活流程", status);
                activated_.store(false);
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

bool AudioHub::FetchManifest(std::string* body) {
    const std::string url = ServerUrl() + "/api/device/v1/files";
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 4096;
    ConfigureHttpTrust(&config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "Authorization", ("Bearer " + api_token_).c_str());

    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        if (esp_http_client_fetch_headers(client) >= 0) {
            const int status = esp_http_client_get_status_code(client);
            if (status == 200) {
                ok = ReadResponse(client, kMaxManifestResponse, body);
            } else if (status == 401 || status == 403 || status == 410) {
                // The token no longer matches the server; bind again on the next check.
                activated_.store(false);
                claim_token_.clear();
                Settings("audio_hub", true).EraseKey("claim_token");
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

bool AudioHub::ParseManifest(const std::string& body, std::vector<RemoteFile>* files,
                             std::string* revision) {
    // 与参考工程一致：任何一条非法记录都拒绝整份清单，避免半同步状态。
    if (body.size() > kMaxManifestResponse)
        return false;
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr) {
        ESP_LOGE(TAG, "清单不是合法 JSON");
        return false;
    }
    cJSON* rev = cJSON_GetObjectItem(root, "revision");
    cJSON* items = cJSON_GetObjectItem(root, "files");
    bool ok = false;
    std::vector<RemoteFile> validated;
    if (cJSON_IsString(rev) && rev->valuestring[0] != '\0' &&
        std::strlen(rev->valuestring) <= 128 && cJSON_IsArray(items)) {
        const int count = cJSON_GetArraySize(items);
        // An empty/malformed response must never wipe factory recordings.
        ok = count > 0 && count <= 32;
        uint64_t total = 0;
        for (int i = 0; i < count && ok; ++i) {
            cJSON* item = cJSON_GetArrayItem(items, i);
            cJSON* name = cJSON_GetObjectItem(item, "name");
            cJSON* size = cJSON_GetObjectItem(item, "size");
            cJSON* category = cJSON_GetObjectItem(item, "category");
            if (!cJSON_IsString(name) || !cJSON_IsNumber(size) || !cJSON_IsString(category)) {
                ok = false;
                break;
            }
            RemoteFile file;
            file.name = name->valuestring;
            if (file.name.size() <= 5 || file.name.compare(file.name.size() - 5, 5, ".opus") != 0 ||
                file.name.size() - 5 >= sizeof(AudioFileInfo::name)) {
                ok = false;
                break;
            }
            file.base_name = file.name.substr(0, file.name.size() - 5);
            const double bytes = size->valuedouble;
            if (!std::isfinite(bytes) || bytes < 1 || bytes > 32ULL * 1024 * 1024 ||
                std::floor(bytes) != bytes) {
                ok = false;
                break;
            }
            file.size = static_cast<uint32_t>(bytes);
            total += file.size;
            if (store_ == nullptr || total > store_->capacity() ||
                file.base_name.find_first_of("/\\") != std::string::npos ||
                file.base_name.find("..") != std::string::npos ||
                std::any_of(file.base_name.begin(), file.base_name.end(),
                            [](unsigned char c) { return c < 0x20 || c == 0x7F; }) ||
                std::any_of(validated.begin(), validated.end(), [&](const RemoteFile& previous) {
                    return previous.base_name == file.base_name;
                })) {
                ok = false;
                break;
            }
            file.category = category->valuestring;
            if (file.category != "animal" && file.category != "ambient") {
                ok = false;
                break;
            }
            validated.push_back(std::move(file));
        }
        if (ok) {
            *files = std::move(validated);
            *revision = rev->valuestring;
        }
    }
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGE(TAG, "清单校验失败（非法条目或缺字段），跳过本次同步");
    }
    return ok;
}

bool AudioHub::DownloadFile(int index, const RemoteFile& file) {
    const std::string url = ServerUrl() + "/api/device/v1/download/" + std::to_string(index);
    const std::string auth = "Bearer " + api_token_;

    for (int attempt = 0; attempt < kDownloadRetries; ++attempt) {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
            (interaction_busy_ && interaction_busy_()))
            return false;
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(2000 << (attempt - 1)));  // 线性退避 2s/4s
        }
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.method = HTTP_METHOD_GET;
        config.timeout_ms = 1000;  // Bound a stalled read while a user starts interacting.
        config.buffer_size = 8192;
        ConfigureHttpTrust(&config);
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            continue;
        }
        esp_http_client_set_header(client, "Authorization", auth.c_str());
        esp_http_client_set_header(client, "User-Agent", "xiaozhi-wzh");

        bool ok = false;
        if (esp_http_client_open(client, 0) == ESP_OK &&
            esp_http_client_fetch_headers(client) >= 0 &&
            esp_http_client_get_status_code(client) == 200) {
            const int64_t started = esp_timer_get_time();
            auto reader = [this, client, started](uint8_t* buf, size_t len) -> size_t {
                if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
                    (interaction_busy_ && interaction_busy_()) ||
                    esp_timer_get_time() - started >= int64_t(kDownloadTimeoutMs) * 1000)
                    return static_cast<size_t>(-1);
                const int got = esp_http_client_read(client, reinterpret_cast<char*>(buf), len);
                if (got < 0) {
                    return static_cast<size_t>(-1);
                }
                return static_cast<size_t>(got);
            };
            std::string error;
            // The current pack has measured PCM lengths; older/unknown packs
            // retain zero-duration compatibility. Playback still uses Ogg's rate.
            const auto* score = FindSoundScore(file.base_name.c_str());
            const uint32_t duration = score ? score->duration_ms : 0;
            ok = store_->WriteFile(file.base_name, file.size, 48000, duration, file.category,
                                   reader, &error);
            if (!ok) {
                ESP_LOGW(TAG, "下载 %s 写入失败: %s", file.name.c_str(), error.c_str());
            }
        } else {
            ESP_LOGW(TAG, "下载 %s HTTP 失败（status=%d）", file.name.c_str(),
                     esp_http_client_get_status_code(client));
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
            (interaction_busy_ && interaction_busy_()))
            return false;
        if (ok) {
            ESP_LOGI(TAG, "已同步 %s（%lu 字节）", file.name.c_str(),
                     static_cast<unsigned long>(file.size));
            return true;
        }
    }
    return false;
}

bool AudioHub::SyncOnce() {
    if (syncing_.exchange(true)) {
        return true;
    }
    status_.store(Status::kSyncing);
    bool ok = false;
    std::string body;
    std::vector<RemoteFile> remote;
    std::string remote_revision;

    do {
        if (!FetchManifest(&body) || !ParseManifest(body, &remote, &remote_revision)) {
            break;
        }
        // Even with an unchanged revision, repair missing/local-reset files.
        // Metadata equality is checked below before skipping each download.
        ESP_LOGI(TAG, "清单 revision %s -> %s，共 %u 个文件", revision_.c_str(),
                 remote_revision.c_str(), static_cast<unsigned>(remote.size()));
        if (refresh_revision_ != remote_revision) {
            refresh_revision_ = remote_revision;
            refreshed_files_ = 0;
        }

        bool failed = false;
        for (size_t i = 0; i < remote.size(); ++i) {
            const int local = store_->FindByName(remote[i].base_name);
            if (local >= 0) {
                AudioFileInfo info = {};
                if (store_->GetFile(local, &info) && info.size == remote[i].size &&
                    (revision_ == remote_revision || (refreshed_files_ & (1U << i)))) {
                    continue;
                }
                // WriteFile appends and replaces only after a verified commit.
                // Never delete the working recording before a download succeeds.
            }
            // 对话开始时暂停下载，等下一轮再继续（保留未提交的旧目录）。
            if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
                (interaction_busy_ && interaction_busy_())) {
                ESP_LOGI(TAG, "对话进行中，暂停同步（revision 不更新）");
                failed = true;
                break;
            }
            if (!DownloadFile(static_cast<int>(i), remote[i])) {
                failed = true;
                break;  // 保留旧 revision，下轮重试
            }
            refreshed_files_ |= 1U << i;
        }

        // 清单中没有的本地文件删除（服务端删除的同步下来）。
        if (!failed) {
            for (const auto& local : store_->Snapshot()) {
                bool in_manifest = false;
                for (const auto& item : remote) {
                    if (item.base_name == local.name) {
                        in_manifest = true;
                        break;
                    }
                }
                if (!in_manifest) {
                    std::string error;
                    if (!store_->DeleteFile(local.name, &error)) {
                        failed = true;
                        break;
                    }
                }
            }
            if (!failed) {
                {
                    std::lock_guard<std::mutex> lock(metadata_mutex_);
                    revision_ = remote_revision;
                }
                Settings("audio_sync", true).SetString("revision", remote_revision);
            }
        }
        ok = !failed;
    } while (false);

    syncing_.store(false);
    status_.store(ok ? Status::kActive : Status::kError);
    return ok;
}

void AudioHub::SetJobMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(job_mutex_);
    job_message_ = message;
}

std::string AudioHub::job_message() const {
    std::lock_guard<std::mutex> lock(job_mutex_);
    return job_message_;
}

std::vector<AudioHub::DiffItem> AudioHub::diff() const {
    std::lock_guard<std::mutex> lock(job_mutex_);
    return diff_;
}

std::string AudioHub::checked_revision() const {
    std::lock_guard<std::mutex> lock(job_mutex_);
    return checked_revision_;
}

bool AudioHub::has_diff() const {
    std::lock_guard<std::mutex> lock(job_mutex_);
    return !diff_.empty();
}

bool AudioHub::RequestCheck() {
    std::lock_guard<std::mutex> lock(job_mutex_);
    if (local_maintenance_)
        return false;
    if (!manual_mode_ || task_handle_ == nullptr) {
        return false;
    }
    if (job_running_.exchange(true)) {
        return false;
    }
    check_requested_.store(true);
    xTaskNotifyGive(task_handle_);
    return true;
}

bool AudioHub::RequestUpdate(const std::vector<std::string>& names) {
    std::lock_guard<std::mutex> lock(job_mutex_);
    if (local_maintenance_)
        return false;
    if (!manual_mode_ || task_handle_ == nullptr || names.empty()) {
        return false;
    }
    if (job_running_.exchange(true)) {
        return false;
    }
    update_names_ = names;
    update_requested_.store(true);
    xTaskNotifyGive(task_handle_);
    return true;
}

bool AudioHub::BeginLocalMaintenance() {
    std::lock_guard<std::mutex> lock(job_mutex_);
    if (local_maintenance_ || job_running_.load())
        return false;
    local_maintenance_ = true;
    return true;
}

void AudioHub::EndLocalMaintenance(bool library_erased) {
    if (library_erased) {
        {
            std::lock_guard<std::mutex> lock(metadata_mutex_);
            revision_.clear();
            refresh_revision_.clear();
            refreshed_files_ = 0;
        }
        Settings("audio_sync", true).SetString("revision", "");
    }
    std::lock_guard<std::mutex> lock(job_mutex_);
    if (library_erased) {
        diff_.clear();
        remote_files_.clear();
        checked_revision_.clear();
        update_names_.clear();
    }
    local_maintenance_ = false;
}

void AudioHub::ManualTask() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (check_requested_.exchange(false)) {
            DoCheck();
        } else if (update_requested_.exchange(false)) {
            DoUpdate();
        } else {
            job_running_.store(false);
        }
    }
}

void AudioHub::DoCheck() {
    job_ok_.store(false);
    status_.store(Status::kSyncing);
    SetJobMessage("正在检查 Audio Hub 清单…");
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        diff_.clear();
        checked_revision_.clear();
        remote_files_.clear();
    }
    if (!EnsureInitialized()) {
        SetJobMessage("无法读取 Audio Hub 配置");
        status_.store(Status::kError);
        job_running_.store(false);
        return;
    }

    do {
        if (!WifiManager::GetInstance().IsConnected()) {
            SetJobMessage("设备未联网，无法检查 Audio Hub");
            status_.store(Status::kError);
            break;
        }
        if (!activated_.load()) {
            if (claim_token_.empty() && !Register()) {
                SetJobMessage("设备注册失败，请检查服务器地址");
                status_.store(Status::kError);
                break;
            }
            if (!activated_.load()) {
                int status_code = 0;
                if (TryActivate(&status_code)) {
                    activated_.store(true);
                } else {
                    const std::string code = activation_code();
                    SetJobMessage(code.empty() ? "等待平台绑定设备"
                                               : "等待平台绑定，激活码 " + code);
                    status_.store(Status::kWaitingActivation);
                    job_ok_.store(true);
                    break;
                }
            }
        }

        std::string body;
        std::vector<RemoteFile> remote;
        std::string revision;
        if (!FetchManifest(&body) || !ParseManifest(body, &remote, &revision)) {
            SetJobMessage("读取清单失败，请检查网络/服务器或重新绑定");
            status_.store(Status::kError);
            break;
        }

        std::vector<DiffItem> items;
        const auto local_files =
            store_ != nullptr ? store_->Snapshot() : std::vector<AudioFileInfo>{};
        for (const auto& rf : remote) {
            DiffItem item;
            item.name = rf.base_name;
            item.category = rf.category;
            item.remote_size = rf.size;
            item.state = "new";
            const int local = store_ != nullptr ? store_->FindByName(rf.base_name) : -1;
            if (local >= 0) {
                AudioFileInfo info = {};
                if (store_->GetFile(local, &info)) {
                    item.local_size = info.size;
                    item.state = info.size == rf.size ? "same" : "changed";
                }
            }
            items.push_back(std::move(item));
        }
        for (const auto& lf : local_files) {
            bool found = false;
            for (const auto& rf : remote) {
                if (rf.base_name == lf.name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                DiffItem item;
                item.name = lf.name;
                item.category = lf.category;
                item.local_size = lf.size;
                item.state = "local";
                items.push_back(std::move(item));
            }
        }

        const size_t remote_count = remote.size();
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            diff_ = std::move(items);
            checked_revision_ = revision;
            remote_files_ = std::move(remote);
        }
        status_.store(Status::kActive);
        job_ok_.store(true);
        SetJobMessage("检查完成：远端 " + std::to_string(remote_count) + " 个文件");
    } while (false);

    job_running_.store(false);
}

void AudioHub::DoUpdate() {
    std::vector<std::string> names;
    std::vector<RemoteFile> remote;
    std::string revision;
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        names = update_names_;
        remote = remote_files_;
        revision = checked_revision_;
    }
    job_ok_.store(false);
    if (names.empty() || remote.empty() || revision.empty()) {
        SetJobMessage("请先检查清单，再选择要更新的文件");
        job_running_.store(false);
        return;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
        (interaction_busy_ && interaction_busy_())) {
        SetJobMessage("对话或动作进行中，稍后再更新");
        job_running_.store(false);
        return;
    }

    status_.store(Status::kSyncing);
    size_t done = 0;
    bool failed = false;
    for (const auto& name : names) {
        int index = -1;
        for (size_t i = 0; i < remote.size(); ++i) {
            if (remote[i].base_name == name) {
                index = static_cast<int>(i);
                break;
            }
        }
        if (index < 0) {
            failed = true;
            SetJobMessage("清单中没有 " + name);
            break;
        }
        SetJobMessage("正在写入 " + name + "（" + std::to_string(done + 1) + "/" +
                      std::to_string(names.size()) + "）");
        if (!DownloadFile(index, remote[index])) {
            failed = true;
            SetJobMessage("更新中止：" + name);
            break;
        }
        ++done;
    }

    if (!failed && done == names.size()) {
        revision_ = revision;
        Settings("audio_sync", true).SetString("revision", revision_);
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            diff_.clear();
            checked_revision_.clear();
            remote_files_.clear();
            update_names_.clear();
        }
        status_.store(Status::kActive);
        job_ok_.store(true);
        SetJobMessage("更新完成：" + std::to_string(done) + " 个文件（本地独有文件保留）");
    } else {
        status_.store(Status::kError);
        if (!failed) {
            SetJobMessage("没有文件被更新");
        }
    }
    job_running_.store(false);
}

void AudioHub::Task() {
    if (manual_mode_) {
        ManualTask();
        return;
    }
    int backoff_s = 5;
    uint32_t last_checkin_ms = 0;
    uint32_t last_sync_ms = 0;

    while (true) {
        if (!WifiManager::GetInstance().IsConnected()) {
            status_.store(Status::kWaitingNetwork);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // 统一激活流程：从未绑定、token 失效都回到这里（401/410 会清掉
        // claim 触发重新注册，参考工程行为一致）。
        if (!activated_.load()) {
            if (claim_token_.empty()) {
                if (!Register()) {
                    status_.store(Status::kError);
                    SleepBackoff(&backoff_s);
                    continue;
                }
            }
            if (activated_.load()) {
                sync_now_.store(true);
                status_.store(Status::kActive);
                backoff_s = 5;
                continue;
            }
            status_.store(Status::kWaitingActivation);
            int status_code = 0;
            if (TryActivate(&status_code)) {
                activated_.store(true);
                sync_now_.store(true);
                backoff_s = 5;
                continue;
            }
            if (status_code == 401 || status_code == 410) {
                claim_token_.clear();
                Settings("audio_hub", true).EraseKey("claim_token");
            }
            // 等待用户在平台输入激活码完成绑定，每 3 秒轮询一次。
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // 已绑定：心跳 + 增量同步（全部在后台，不阻塞对话）。
        if (status_.load() == Status::kWaitingNetwork ||
            status_.load() == Status::kWaitingActivation)
            status_.store(Status::kActive);
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        if (now_ms - last_checkin_ms >= kCheckinIntervalMs) {
            CheckIn();
            last_checkin_ms = now_ms;
            if (!activated_.load())
                continue;
        }
        if ((sync_now_.load() || now_ms - last_sync_ms >= kSyncIntervalMs) &&
            Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
            (!interaction_busy_ || !interaction_busy_())) {
            sync_now_.store(false);
            if (SyncOnce()) {
                last_sync_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
                backoff_s = 5;
            } else {
                sync_now_.store(
                    true);  // Retry after backoff; a busy session never loses the request.
            }
        }
        if (status_.load() == Status::kError) {
            SleepBackoff(&backoff_s);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

}  // namespace wzh
