#include "wzh_audio_hub.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <esp_app_desc.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>

#include "application.h"
#include "settings.h"
#include <wifi_manager.h>

#define TAG "WzhAudioHub"

namespace wzh {
namespace {

constexpr int kDownloadRetries = 3;
constexpr int kHttpTimeoutMs = 10000;
constexpr int kDownloadTimeoutMs = 180000;
constexpr int kCheckinIntervalMs = 60000;
constexpr int kSyncIntervalMs = 60 * 60 * 1000;  // 每小时对一次清单
constexpr int kMaxBackoffS = 300;

const char* StatusNameImpl(AudioHub::Status status)
{
    switch (status) {
        case AudioHub::Status::kDisabled: return "disabled";
        case AudioHub::Status::kWaitingNetwork: return "waiting_network";
        case AudioHub::Status::kRegistering: return "registering";
        case AudioHub::Status::kWaitingActivation: return "waiting_activation";
        case AudioHub::Status::kActive: return "active";
        case AudioHub::Status::kSyncing: return "syncing";
        case AudioHub::Status::kError: return "error";
        default: return "unknown";
    }
}

std::string FirmwareVersion()
{
    return esp_app_get_description()->version;
}

}  // namespace

const char* AudioHub::StatusName(Status status)
{
    return StatusNameImpl(status);
}

bool AudioHub::Start(AudioStore* store)
{
    store_ = store;
#if !CONFIG_WZH_AUDIO_SYNC_ENABLE
    status_.store(Status::kDisabled);
    ESP_LOGI(TAG, "Audio Hub 同步已在 Kconfig 中关闭");
    return false;
#else
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[32];
    snprintf(id, sizeof(id), "ZYLZ-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    device_id_ = id;

    // 服务地址：NVS 可覆盖 Kconfig 默认值（换服务器不用重烧固件）。
    Settings hub("audio_hub", true);
    server_ = hub.GetString("server", CONFIG_WZH_AUDIO_SYNC_SERVER);
    api_token_ = hub.GetString("api_token");
    claim_token_ = hub.GetString("claim_token");
    activation_code_ = hub.GetString("act_code");
    Settings sync("audio_sync", true);
    revision_ = sync.GetString("revision");

    if (xTaskCreate(TaskEntry, "wzh_hub", 8192, this, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "无法启动 Audio Hub 任务");
        return false;
    }
    ESP_LOGI(TAG, "Audio Hub 已启动: device=%s product=%s server=%s",
             device_id_.c_str(), Product().hub_product_id, server_.c_str());
    return true;
#endif
}

void AudioHub::TaskEntry(void* arg)
{
    static_cast<AudioHub*>(arg)->Task();
}

void AudioHub::SleepBackoff(int* backoff_s)
{
    ESP_LOGW(TAG, "失败退避 %d 秒", *backoff_s);
    vTaskDelay(pdMS_TO_TICKS(*backoff_s * 1000));
    *backoff_s = std::min(*backoff_s * 2, kMaxBackoffS);
}

std::string AudioHub::ServerUrl() const
{
    // 尾部不带斜杠。
    std::string base = server_;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

bool AudioHub::Register()
{
    status_.store(Status::kRegistering);
    const std::string url = ServerUrl() + "/api/device/register";
    std::string body = "{\"device_id\":\"" + device_id_ + "\",\"product_id\":\"" +
                       Product().hub_product_id + "\",\"firmware_version\":\"" +
                       FirmwareVersion() + "\"}";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 2048;
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
        if (esp_http_client_fetch_headers(client) >= 0 &&
            esp_http_client_get_status_code(client) == 200) {
            char buf[512];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                response.append(buf, len);
            }
            ok = true;
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
    if (cJSON_IsString(claim) && cJSON_IsString(code)) {
        claim_token_ = claim->valuestring;
        activation_code_ = code->valuestring;
        Settings settings("audio_hub", true);
        settings.SetString("claim_token", claim_token_);
        settings.SetString("act_code", activation_code_);
        ESP_LOGI(TAG, "注册成功，激活码: %s（等待用户在平台绑定）", activation_code_.c_str());
        ok = true;
    } else {
        ESP_LOGE(TAG, "注册响应缺少字段: %s", response.c_str());
        ok = false;
    }
    cJSON_Delete(root);
    return ok;
}

bool AudioHub::TryActivate(int* status_code)
{
    const std::string url = ServerUrl() + "/api/device/activate";
    std::string body = "{\"device_id\":\"" + device_id_ + "\",\"claim_token\":\"" +
                       claim_token_ + "\"}";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 2048;
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
                char buf[512];
                int len;
                while ((len = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                    response.append(buf, len);
                }
                http_ok = true;
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
    if (cJSON_IsString(token)) {
        api_token_ = token->valuestring;
        Settings settings("audio_hub", true);
        settings.SetString("api_token", api_token_);
        settings.EraseKey("claim_token");
        settings.EraseKey("act_code");
        claim_token_.clear();
        activated = true;
        ESP_LOGI(TAG, "绑定成功，Audio Hub 激活完成");
    }
    cJSON_Delete(root);
    return activated;
}

bool AudioHub::CheckIn()
{
    const std::string url = ServerUrl() + "/api/device/v1/check-in";
    std::string body = "{\"firmware_version\":\"" + FirmwareVersion() + "\"}";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 1024;
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

bool AudioHub::FetchManifest(std::string* body)
{
    const std::string url = ServerUrl() + "/api/device/v1/files";
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "Authorization", ("Bearer " + api_token_).c_str());

    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        if (esp_http_client_fetch_headers(client) >= 0 &&
            esp_http_client_get_status_code(client) == 200) {
            char buf[1024];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                body->append(buf, len);
            }
            ok = true;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

bool AudioHub::ParseManifest(const std::string& body, std::vector<RemoteFile>* files,
                             std::string* revision)
{
    // 与参考工程一致：任何一条非法记录都拒绝整份清单，避免半同步状态。
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr) {
        ESP_LOGE(TAG, "清单不是合法 JSON");
        return false;
    }
    cJSON* rev = cJSON_GetObjectItem(root, "revision");
    cJSON* items = cJSON_GetObjectItem(root, "files");
    bool ok = false;
    if (cJSON_IsString(rev) && cJSON_IsArray(items)) {
        ok = true;
        const int count = cJSON_GetArraySize(items);
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
            if (file.name.size() <= 5 ||
                file.name.compare(file.name.size() - 5, 5, ".opus") != 0 ||
                file.name.size() - 5 >= sizeof(AudioFileInfo::name)) {
                ok = false;
                break;
            }
            file.base_name = file.name.substr(0, file.name.size() - 5);
            file.size = static_cast<uint32_t>(size->valuedouble);
            if (file.size == 0) {
                ok = false;
                break;
            }
            file.category = category->valuestring;
            if (file.category != "animal" && file.category != "ambient") {
                ok = false;
                break;
            }
            files->push_back(std::move(file));
        }
        if (ok) {
            *revision = rev->valuestring;
        }
    }
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGE(TAG, "清单校验失败（非法条目或缺字段），跳过本次同步");
    }
    return ok;
}

bool AudioHub::DownloadFile(int index, const RemoteFile& file)
{
    const std::string url =
        ServerUrl() + "/api/device/v1/download/" + std::to_string(index);
    const std::string auth = "Bearer " + api_token_;

    for (int attempt = 0; attempt < kDownloadRetries; ++attempt) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(2000 << (attempt - 1)));  // 线性退避 2s/4s
        }
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.method = HTTP_METHOD_GET;
        config.timeout_ms = kDownloadTimeoutMs;
        config.buffer_size = 8192;
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
            auto reader = [client](uint8_t* buf, size_t len) -> size_t {
                const int got = esp_http_client_read(client, reinterpret_cast<char*>(buf), len);
                if (got < 0) {
                    return static_cast<size_t>(-1);
                }
                return static_cast<size_t>(got);
            };
            std::string error;
            // 参考工程节奏：sample_rate 固定 48000，时长未知写 0（播
            // 放时以 OGG 头里的真实采样率为准）。
            ok = store_->WriteFile(file.base_name, file.size, 48000, 0, file.category,
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
        if (ok) {
            ESP_LOGI(TAG, "已同步 %s（%lu 字节）", file.name.c_str(),
                     static_cast<unsigned long>(file.size));
            return true;
        }
    }
    return false;
}

bool AudioHub::SyncOnce()
{
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
        if (remote_revision == revision_) {
            ESP_LOGI(TAG, "清单 revision 未变化（%s）", remote_revision.c_str());
            ok = true;
            break;
        }
        ESP_LOGI(TAG, "清单 revision %s -> %s，共 %u 个文件", revision_.c_str(),
                 remote_revision.c_str(), static_cast<unsigned>(remote.size()));

        bool failed = false;
        for (size_t i = 0; i < remote.size(); ++i) {
            const int local = store_->FindByName(remote[i].base_name);
            if (local >= 0) {
                const auto* info = store_->PeekFile(local);
                if (info != nullptr && info->size == remote[i].size) {
                    continue;  // 等大小视为已存在
                }
                std::string error;
                store_->DeleteFile(remote[i].base_name, &error);  // 大小变化：删旧再下
            }
            // 对话开始时暂停下载，等下一轮再继续（保留未提交的旧目录）。
            if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
                ESP_LOGI(TAG, "对话进行中，暂停同步（revision 不更新）");
                failed = true;
                break;
            }
            if (!DownloadFile(static_cast<int>(i), remote[i])) {
                failed = true;
                break;  // 保留旧 revision，下轮重试
            }
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
                    store_->DeleteFile(local.name, &error);
                }
            }
            revision_ = remote_revision;
            Settings("audio_sync", true).SetString("revision", remote_revision);
        }
        ok = !failed;
    } while (false);

    syncing_.store(false);
    status_.store(ok ? Status::kActive : Status::kError);
    return ok;
}

void AudioHub::Task()
{
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
            status_.store(Status::kWaitingActivation);
            int status_code = 0;
            if (TryActivate(&status_code)) {
                activated_.store(true);
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
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        if (now_ms - last_checkin_ms >= kCheckinIntervalMs) {
            CheckIn();
            last_checkin_ms = now_ms;
        }
        if (sync_now_.exchange(false) || now_ms - last_sync_ms >= kSyncIntervalMs) {
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                SyncOnce();
                last_sync_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
                backoff_s = 5;
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
