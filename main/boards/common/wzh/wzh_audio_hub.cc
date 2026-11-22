#include "wzh_audio_hub.h"
#include "wzh_developer_session.h"
#include "wzh_sound_score.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <esp_app_desc.h>
#include <esp_http_client.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/md.h>

#include <cJSON.h>

#include "application.h"
#include "board.h"
#include "settings.h"

#define TAG "WzhAudioHub"

namespace wzh {
namespace {

constexpr int kDownloadRetries = 3;
constexpr int kHttpTimeoutMs = 10000;
constexpr int kDownloadTimeoutMs = 180000;
constexpr size_t kMaxControlResponse = 8192;
constexpr size_t kMaxManifestResponse = 65536;
constexpr uint16_t kDiscoveryPort = 39876;
constexpr int64_t kPresenceIntervalUs = 45000000;
constexpr int64_t kPresenceRetryUs = 10000000;
constexpr std::string_view kDiscoveryPrefix = "WZH-HUB/1 ";

std::string Hex(const uint8_t* bytes, size_t count) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output(count * 2, '0');
    for (size_t i = 0; i < count; ++i) {
        output[2 * i] = digits[bytes[i] >> 4];
        output[2 * i + 1] = digits[bytes[i] & 15];
    }
    return output;
}

bool ValidProof(std::string_view message, std::string_view proof) {
    uint8_t digest[32];
    const auto* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (proof.size() != sizeof(digest) * 2 || !sha256 ||
        mbedtls_md_hmac(sha256, reinterpret_cast<const uint8_t*>(kDeveloperPassword.data()),
                        kDeveloperPassword.size(), reinterpret_cast<const uint8_t*>(message.data()),
                        message.size(), digest) != 0)
        return false;
    const auto expected = Hex(digest, sizeof(digest));
    unsigned difference = 0;
    for (size_t i = 0; i < expected.size(); ++i)
        difference |= static_cast<unsigned char>(expected[i] ^ proof[i]);
    return difference == 0;
}

bool FactoryNetworkAvailable() {
    const auto& board = Board::GetInstance();
    return !board.UsesCellularNetwork() && board.HasSystemNetwork();
}

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

bool VerifyHub(std::string_view server) {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    const auto nonce = Hex(bytes, sizeof(bytes));
    const std::string url = std::string(server) + "/api/device/discovery?nonce=" + nonce;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 2000;
    config.buffer_size = 512;
    auto client = esp_http_client_init(&config);
    if (!client)
        return false;
    std::string response;
    const bool ok =
        esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0 &&
        esp_http_client_get_status_code(client) == 200 && ReadResponse(client, 64, &response) &&
        ValidProof(std::string(kDiscoveryPrefix) + "verify " + nonce, response);
    esp_http_client_cleanup(client);
    return ok;
}

const char* StatusNameImpl(AudioHub::Status status) {
    switch (status) {
        case AudioHub::Status::kIdle:
            return "idle";
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
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
        return false;
    char id[32];
    snprintf(id, sizeof(id), "ZYLZ-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    device_id_ = id;

    // 服务地址：NVS 可覆盖 Kconfig 默认值（换服务器不用重烧固件）。
    Settings hub("audio_hub");
    server_ = hub.GetString("server", CONFIG_WZH_AUDIO_SYNC_SERVER);
    api_token_ = hub.GetString("api_token");
    claim_token_ = hub.GetString("claim_token");
    activation_code_ = hub.GetString("act_code");
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        revision_ = Settings("audio_sync").GetString("revision");
    }
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

void AudioHub::ConfigureForFactory(AudioStore* store, std::function<bool()> interaction_busy) {
    store_ = store;
    interaction_busy_ = std::move(interaction_busy);
}

void AudioHub::StartLanDiscovery() {
    if (store_ == nullptr || !FactoryNetworkAvailable() || discovery_running_.exchange(true))
        return;
    if (xTaskCreate(LanDiscoveryTaskEntry, "wzh_hub_lan", 4096, this, 1, nullptr) != pdPASS)
        discovery_running_.store(false);
}

void AudioHub::LanDiscoveryTaskEntry(void* arg) {
    auto& hub = *static_cast<AudioHub*>(arg);
    const bool wait_for_network = hub.ListenForHub();
    hub.discovery_running_.store(false);
    if (wait_for_network)
        hub.StartLanDiscovery();  // The next address event may have arrived during cleanup.
    vTaskDelete(nullptr);
}

bool AudioHub::ListenForHub() {
    auto* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    ifreq interface{};
    if (!sta || esp_netif_get_netif_impl_name(sta, interface.ifr_name) != ESP_OK)
        return false;
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return false;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kDiscoveryPort);
    timeval timeout{1, 0};
    const int broadcast = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &interface, sizeof(interface)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        close(fd);
        return false;
    }
    uint32_t last_sta_address = 0;
    while (FactoryNetworkAvailable()) {
        char bytes[128];
        sockaddr_in peer{};
        socklen_t size = sizeof(peer);
        const auto count =
            recvfrom(fd, bytes, sizeof(bytes), 0, reinterpret_cast<sockaddr*>(&peer), &size);
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(fd);
            return false;
        }
        if (count <= 0 || count >= static_cast<ssize_t>(sizeof(bytes)) ||
            peer.sin_family != AF_INET || !FactoryNetworkAvailable())
            continue;
        esp_netif_ip_info_t ip{};
        const uint32_t source = peer.sin_addr.s_addr;
        const uint32_t host = ntohl(source);
        const bool private_address =
            (host >> 24) == 10 || (host >> 20) == 0xac1 || (host >> 16) == 0xc0a8;
        if (!private_address || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0 ||
            ip.netmask.addr == 0 || source == ip.ip.addr ||
            (source & ip.netmask.addr) != (ip.ip.addr & ip.netmask.addr))
            continue;
        char peer_ip[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip)))
            continue;
        const std::string prefix = std::string(kDiscoveryPrefix) + peer_ip + " ";
        const std::string_view packet(bytes, count);
        const auto proof_start = packet.rfind(' ');
        if (!packet.starts_with(prefix) || proof_start <= prefix.size() ||
            proof_start == std::string_view::npos)
            continue;
        const char* port_end = packet.data() + proof_start;
        unsigned port = 0;
        const auto parsed = std::from_chars(packet.data() + prefix.size(), port_end, port);
        if (parsed.ec != std::errc{} || parsed.ptr != port_end || port == 0 || port > 65535 ||
            !ValidProof(packet.substr(0, proof_start), packet.substr(proof_start + 1)))
            continue;
        if (last_sta_address != ip.ip.addr) {
            last_sta_address = ip.ip.addr;
            std::lock_guard<std::mutex> lock(job_mutex_);
            next_presence_us_ = 0;
        }
        if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
            !Board::GetInstance().IsAudioMaintenanceActive())
            RequestJob(Job::kPresence, {},
                       "http://" + std::string(peer_ip) + ":" + std::to_string(port));
    }
    close(fd);
    return true;
}

void AudioHub::TaskEntry(void* arg) {
    auto& hub = *static_cast<AudioHub*>(arg);
    Job job;
    std::string server;
    {
        std::lock_guard<std::mutex> lock(hub.job_mutex_);
        job = hub.pending_job_;
        if (job == Job::kPresence)
            server = hub.offered_server_;
    }
    if (job == Job::kPresence)
        hub.DoPresence(server);
    else if (job == Job::kCheck)
        hub.DoCheck();
    else
        hub.DoUpdate();
    hub.job_running_.store(false);
    vTaskDelete(nullptr);
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

void AudioHub::ClearRegistration() {
    activated_.store(false);
    api_token_.clear();
    claim_token_.clear();
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        activation_code_.clear();
    }
    Settings settings("audio_hub", true);
    settings.EraseKey("api_token");
    settings.EraseKey("claim_token");
    settings.EraseKey("act_code");
}

bool AudioHub::CheckIn() {
    const std::string url = ServerUrl() + "/api/device/v1/check-in";
    const std::string body = "{\"device_id\":\"" + device_id_ + "\",\"product_id\":\"" +
                             Product().hub_product_id + "\",\"firmware_version\":\"" +
                             FirmwareVersion() + "\"}";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 1024;
    ConfigureHttpTrust(&config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        SetJobMessage("设备资源不足，无法连接 Audio Hub");
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", ("Bearer " + api_token_).c_str());

    int status = 0;
    bool ok = false;
    std::string response;
    if (esp_http_client_open(client, body.size()) == ESP_OK) {
        esp_http_client_write(client, body.data(), body.size());
        if (esp_http_client_fetch_headers(client) >= 0) {
            status = esp_http_client_get_status_code(client);
            ok = status == 200 && ReadResponse(client, kMaxControlResponse, &response);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok) {
        if (status == 401 || status == 410)
            ClearRegistration();
        SetJobMessage(status == 403 ? "该设备已在 Audio Hub 停用"
                                    : "未能上报设备信息，请检查 Audio Hub 或局域网");
        return false;
    }

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLength(response.data(), response.size()), cJSON_Delete);
    const auto* device = cJSON_GetObjectItemCaseSensitive(root.get(), "device");
    const auto* product = cJSON_GetObjectItemCaseSensitive(device, "product_id");
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(), "ok")) ||
        !cJSON_IsString(product) ||
        std::strcmp(product->valuestring, Product().hub_product_id) != 0) {
        SetJobMessage("Audio Hub 未确认当前设备型号，请检查登记信息");
        return false;
    }
    return true;
}

bool AudioHub::FetchManifest(std::string* body) {
    const std::string url =
        ServerUrl() + "/api/device/v1/files?product=" + Product().hub_product_id;
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
            } else if (status == 401 || status == 410) {
                ClearRegistration();
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
    const auto* product = cJSON_GetObjectItemCaseSensitive(root, "product");
    bool ok = false;
    std::vector<RemoteFile> validated;
    if (cJSON_IsString(rev) && rev->valuestring[0] != '\0' &&
        std::strlen(rev->valuestring) <= 128 && cJSON_IsArray(items) && cJSON_IsString(product) &&
        std::strcmp(product->valuestring, Product().hub_product_id) == 0) {
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

bool AudioHub::CanDownload() const {
    return FactoryNetworkAvailable() &&
           Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
           (!interaction_busy_ || !interaction_busy_());
}

bool AudioHub::DownloadFile(int index, const RemoteFile& file) {
    const std::string url = ServerUrl() + "/api/device/v1/download/" + std::to_string(index) +
                            "?product=" + Product().hub_product_id;
    const std::string auth = "Bearer " + api_token_;

    for (int attempt = 0; attempt < kDownloadRetries; ++attempt) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(2000 << (attempt - 1)));  // 仅在当前手动任务内重试。
        }
        if (!CanDownload())
            return false;
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
                if (!CanDownload() ||
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
        if (!CanDownload())
            return false;
        if (ok) {
            ESP_LOGI(TAG, "已同步 %s（%lu 字节）", file.name.c_str(),
                     static_cast<unsigned long>(file.size));
            return true;
        }
    }
    return false;
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

bool AudioHub::RequestCheck() { return RequestJob(Job::kCheck, {}); }

bool AudioHub::RequestUpdate(const std::vector<std::string>& names) {
    return !names.empty() && RequestJob(Job::kUpdate, names);
}

bool AudioHub::RequestJob(Job job, const std::vector<std::string>& names,
                          const std::string& server) {
    std::lock_guard<std::mutex> lock(job_mutex_);
    if (store_ == nullptr || local_maintenance_ || job_running_.load())
        return false;
    if (job == Job::kPresence) {
        if (server == offered_server_ && esp_timer_get_time() < next_presence_us_)
            return false;
        offered_server_ = server;
        next_presence_us_ = esp_timer_get_time() + kPresenceRetryUs;
    } else {
        update_names_ = names;
        job_ok_.store(false);
    }
    pending_job_ = job;
    job_running_.store(true);
    if (xTaskCreate(TaskEntry, "wzh_hub_job", 8192, this, 1, nullptr) == pdPASS)
        return true;
    if (job != Job::kPresence) {
        job_message_ = "设备资源不足，无法开始 Audio Hub 任务";
        status_.store(Status::kError);
    }
    job_running_.store(false);
    return false;
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

bool AudioHub::EnsureRegistered() {
    if (activated_.load() && CheckIn())
        return true;
    // Authentication errors clear the old token. A disabled device keeps it
    // and must not register again while the server has disabled that record.
    if (activated_.load()) {
        status_.store(Status::kError);
        return false;
    }
    if (claim_token_.empty() && !Register()) {
        SetJobMessage("设备登记失败，请检查 Audio Hub 地址和局域网");
        status_.store(Status::kError);
        return false;
    }
    if (!activated_.load()) {
        int status_code = 0;
        if (TryActivate(&status_code)) {
            activated_.store(true);
        } else {
            if (status_code == 401 || status_code == 409 || status_code == 410) {
                ClearRegistration();
                SetJobMessage("登记凭据已失效，等待重新接入");
                status_.store(Status::kError);
            } else if (status_code == 200) {
                const std::string code = activation_code();
                SetJobMessage(code.empty() ? "等待在 Audio Hub 绑定设备"
                                           : "等待绑定，激活码 " + code);
                status_.store(Status::kWaitingActivation);
            } else {
                SetJobMessage(status_code == 403 ? "该设备已在 Audio Hub 停用"
                                                 : "读取绑定状态失败，请检查设备网络");
                status_.store(Status::kError);
            }
            return false;
        }
    }
    if (CheckIn())
        return true;
    status_.store(Status::kError);
    return false;
}

void AudioHub::DoPresence(const std::string& server) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    if (board.IsAudioMaintenanceActive() || app.GetDeviceState() != kDeviceStateIdle ||
        !FactoryNetworkAvailable() || !VerifyHub(server) || !EnsureInitialized() ||
        !FactoryNetworkAvailable() || board.IsAudioMaintenanceActive() ||
        app.GetDeviceState() != kDeviceStateIdle)
        return;

    if (server_ != server) {
        server_ = server;
        std::lock_guard<std::mutex> lock(job_mutex_);
        diff_.clear();
        checked_revision_.clear();
        remote_files_.clear();
    }
    if (!EnsureRegistered())
        return;

    status_.store(Status::kActive);
    std::lock_guard<std::mutex> lock(job_mutex_);
    next_presence_us_ = esp_timer_get_time() + kPresenceIntervalUs;
}

void AudioHub::DoCheck() {
    status_.store(Status::kSyncing);
    SetJobMessage("正在检查 Audio Hub 清单…");
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        diff_.clear();
        checked_revision_.clear();
        remote_files_.clear();
    }
    if (!FactoryNetworkAvailable()) {
        SetJobMessage("请先让设备连接 Audio Hub 所在的局域网 Wi-Fi");
        status_.store(Status::kWaitingNetwork);
        return;
    }
    if (!EnsureInitialized()) {
        SetJobMessage("无法读取 Audio Hub 配置");
        status_.store(Status::kError);
        return;
    }

    if (!EnsureRegistered()) {
        if (status_.load() == Status::kWaitingActivation)
            job_ok_.store(true);
        return;
    }

    do {
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
    if (names.empty() || remote.empty() || revision.empty()) {
        SetJobMessage("请先检查清单，再选择要更新的文件");
        return;
    }
    if (!FactoryNetworkAvailable()) {
        SetJobMessage("请先让设备连接 Audio Hub 所在的局域网 Wi-Fi");
        status_.store(Status::kWaitingNetwork);
        return;
    }
    if (!CanDownload()) {
        SetJobMessage("对话或动作进行中，稍后再更新");
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
        Settings("audio_sync", true).SetString("revision", revision);
        {
            std::lock_guard<std::mutex> lock(metadata_mutex_);
            revision_ = revision;
        }
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
}

}  // namespace wzh
