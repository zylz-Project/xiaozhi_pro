#include "ota.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "system_info.h"

#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <sys/time.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <sstream>
#include <string_view>
#include <vector>

#define TAG "Ota"

namespace {

bool IsUsableProtocolAddress(std::string_view value, bool websocket) {
    if (value.find_first_of(" \t\r\n") != std::string_view::npos)
        return false;
    if (websocket) {
        const auto scheme_end = value.find("://");
        if (scheme_end == std::string_view::npos || scheme_end == 0)
            return false;
        value.remove_prefix(scheme_end + 3);
    } else if (value.find('/') != std::string_view::npos) {
        return false;
    }
    return !value.empty() && value.front() != ':' && value.front() != '/';
}

bool StoreProtocolSettings(const char* name, const char* address_key, const cJSON* config) {
    if (!cJSON_IsObject(config))
        return false;

    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(name, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open %s settings: %s", name, esp_err_to_name(result));
        return false;
    }

    // NVS permits up to 4000 bytes per string, including the terminator.
    constexpr size_t kNvsStringSize = 4000;
    bool wrote_setting = false;
    const bool websocket = std::string_view(address_key) == "url";
    const cJSON* item = nullptr;
    cJSON_ArrayForEach (item, config) {
        if (!item->string || item->string[0] == '\0' ||
            std::strlen(item->string) >= NVS_KEY_NAME_MAX_SIZE) {
            continue;
        }
        const bool address = std::strcmp(item->string, address_key) == 0;
        if (cJSON_IsString(item) && std::strlen(item->valuestring) < kNvsStringSize) {
            if (address && !IsUsableProtocolAddress(item->valuestring, websocket))
                continue;
            result = nvs_set_str(handle, item->string, item->valuestring);
        } else if (!address && cJSON_IsNumber(item) && item->valuedouble == item->valueint) {
            result = nvs_set_i32(handle, item->string, item->valueint);
        } else {
            continue;
        }
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Cannot write %s settings: %s", name, esp_err_to_name(result));
            break;
        }
        wrote_setting = true;
    }

    bool usable = false;
    if (wrote_setting) {
        const auto commit = nvs_commit(handle);
        if (commit != ESP_OK) {
            ESP_LOGW(TAG, "Cannot commit %s settings: %s", name, esp_err_to_name(commit));
        } else if (result == ESP_OK) {
            // Partial responses can reuse an endpoint previously stored for
            // this protocol. Empty sections must not select a protocol.
            size_t length = 0;
            if (nvs_get_str(handle, address_key, nullptr, &length) == ESP_OK && length > 1 &&
                length <= kNvsStringSize) {
                std::string address(length, '\0');
                if (nvs_get_str(handle, address_key, address.data(), &length) == ESP_OK) {
                    usable = IsUsableProtocolAddress(address.c_str(), websocket);
                }
            }
        }
    }
    nvs_close(handle);
    return usable;
}

bool ApplyServerTime(const cJSON* config) {
    if (!cJSON_IsObject(config))
        return false;

    auto* timestamp = cJSON_GetObjectItem(config, "timestamp");
    // Keep floating-point conversion and the offset calculation in range.
    constexpr int64_t kMaxTimestampMs = 253402300799999LL;  // End of year 9999.
    if (!cJSON_IsNumber(timestamp) ||
        !(timestamp->valuedouble >= 0 && timestamp->valuedouble <= kMaxTimestampMs)) {
        return false;
    }

    int64_t local_ms = static_cast<int64_t>(timestamp->valuedouble);
    if (auto* offset = cJSON_GetObjectItem(config, "timezone_offset"); offset != nullptr) {
        constexpr int kMinutesPerDay = 24 * 60;
        if (!cJSON_IsNumber(offset) || offset->valuedouble != offset->valueint ||
            offset->valueint < -kMinutesPerDay || offset->valueint > kMinutesPerDay) {
            return false;
        }
        local_ms += static_cast<int64_t>(offset->valueint) * 60000;
    }
    if (local_ms < 0 || local_ms > kMaxTimestampMs ||
        local_ms / 1000 > std::numeric_limits<time_t>::max()) {
        return false;
    }
    const timeval time{.tv_sec = static_cast<time_t>(local_ms / 1000),
                       .tv_usec = static_cast<suseconds_t>(local_ms % 1000) * 1000};
    return settimeofday(&time, nullptr) == 0;
}

}  // namespace

Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

Ota::~Ota() {}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) {
        ESP_LOGE(TAG, "Failed to create OTA HTTP client");
        return nullptr;
    }
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", user_agent.c_str(),
                 serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

esp_err_t Ota::FetchVersionResponse(const std::string& url, const std::string& payload,
                                    cJSON*& response) {
    constexpr size_t kMaxConfigResponseSize = 16 * 1024;
    auto& board = Board::GetInstance();
    // ML307 can report a ready PDP context before the first DNS request works.
    // Retry transport failures briefly on the activation worker, before the
    // application's longer backoff. An HTTP error status is definitive.
    const int attempts = board.UsesCellularNetwork() ? 3 : 1;
    const std::string method = payload.empty() ? "GET" : "POST";
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0 && !board.IsNetworkAvailable())
            return ESP_FAIL;
        auto http = SetupHttp();
        if (!http)
            return ESP_FAIL;
        if (board.UsesCellularNetwork())
            http->SetHeader("Connection", "close");
        http->SetContent(std::string(payload));
        esp_err_t error = ESP_FAIL;
        if (auto opened = http->Open(method, url); !opened) {
            ESP_LOGW(TAG, "Version check transport open failed: %s",
                     opened.error().ToString().c_str());
        } else {
            auto status = http->GetStatusCode();
            if (!status) {
                ESP_LOGW(TAG, "Version check transport status failed: %s",
                         status.error().ToString().c_str());
            } else if (*status == 200) {
                const size_t expected = http->GetBodyLength();
                const size_t limit = expected == 0 ? kMaxConfigResponseSize : expected;
                std::string data;
                bool complete = false;
                if (limit <= kMaxConfigResponseSize) {
                    std::string buffer(512, '\0');
                    while (true) {
                        const size_t remaining = limit - data.size();
                        const size_t requested = std::min(buffer.size(), remaining + 1);
                        const auto count = http->Read(buffer.data(), requested);
                        if (!count || *count < 0 || static_cast<size_t>(*count) > remaining ||
                            static_cast<size_t>(*count) > requested) {
                            break;
                        }
                        if (*count == 0) {
                            complete = expected == 0 || data.size() == expected;
                            break;
                        }
                        data.append(buffer.data(), static_cast<size_t>(*count));
                    }
                }
                http->Close();
                if (complete && !data.empty()) {
                    const char* end = nullptr;
                    auto* root = cJSON_ParseWithLengthOpts(data.c_str(), data.size() + 1, &end, 1);
                    if (cJSON_IsObject(root) && end == data.c_str() + data.size()) {
                        response = root;
                        return ESP_OK;
                    }
                    cJSON_Delete(root);
                }
                // The response can include service credentials. Log lengths only.
                ESP_LOGW(TAG, "Invalid service configuration response: received=%u expected=%u",
                         static_cast<unsigned>(data.size()), static_cast<unsigned>(expected));
                error = ESP_ERR_INVALID_RESPONSE;
            } else {
                ESP_LOGE(TAG, "Failed to check version, status code: %d", *status);
                return static_cast<esp_err_t>(*status);
            }
        }
        // Release the modem HTTP ID before opening another request.
        http.reset();
        if (attempt + 1 == attempts)
            return error;
        const int delay_ms = (attempt + 1) * 1500;
        ESP_LOGI(TAG, "Retrying cellular version check in %d ms (%d/%d)", delay_ms, attempt + 1,
                 attempts - 1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    return ESP_FAIL;
}

/*
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::CheckVersion() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON* root = nullptr;
    const esp_err_t request_result = FetchVersionResponse(url, board.GetSystemInfoJson(), root);
    if (request_result != ESP_OK)
        return request_result;

    // Response: { "firmware": { "version": "1.0.0", "url": "http://" } }
    // Parse the JSON response and check if the version is newer
    // If it is, set has_new_version_ to true and store the new version and URL

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON* activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = StoreProtocolSettings("mqtt", "endpoint", cJSON_GetObjectItem(root, "mqtt"));
    has_websocket_config_ =
        StoreProtocolSettings("websocket", "url", cJSON_GetObjectItem(root, "websocket"));

    has_server_time_ = ApplyServerTime(cJSON_GetObjectItem(root, "server_time"));
    if (!has_server_time_)
        ESP_LOGW(TAG, "Server time unavailable");

#if CONFIG_ENABLE_ONLINE_UPDATES
    has_new_version_ = false;
    cJSON* firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON* version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON* url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            // Check if the version is newer, for example, 0.1.0 is newer than 0.0.1
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "Current is the latest version");
            }
            // If the force flag is set to 1, the given version is forced to be installed
            cJSON* force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }
#endif

    cJSON_Delete(root);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url,
                  std::function<void(int progress, size_t speed)> callback) {
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label,
             update_partition->address);
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) {
        ESP_LOGE(TAG, "Failed to create firmware HTTP client");
        return false;
    }
    if (auto opened = http->Open("GET", firmware_url); !opened) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", opened.error().ToString().c_str());
        return false;
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read firmware status: %s",
                 status_code.error().ToString().c_str());
        return false;
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", *status_code);
        return false;
    }

    const size_t content_length = http->GetBodyLength();
    constexpr size_t kImagePrefixSize =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (content_length < kImagePrefixSize || content_length > update_partition->size) {
        ESP_LOGE(TAG, "Invalid firmware Content-Length: %zu", content_length);
        return false;
    }

    constexpr size_t PAGE_SIZE = 4096;
    static_assert(kImagePrefixSize <= PAGE_SIZE);
    std::unique_ptr<char, decltype(&heap_caps_free)> buffer(
        static_cast<char*>(heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL)), &heap_caps_free);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return false;
    }

    size_t buffer_offset = 0;  // Current data size in buffer
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    bool update_started = false;
    while (total_read < content_length) {
        const size_t requested = std::min(PAGE_SIZE - buffer_offset, content_length - total_read);
        const auto ret = http->Read(buffer.get() + buffer_offset, requested);
        if (!ret || *ret <= 0 || static_cast<size_t>(*ret) > requested) {
            ESP_LOGE(TAG, "Firmware transfer ended early or failed at %zu/%zu bytes", total_read,
                     content_length);
            if (update_started)
                esp_ota_abort(update_handle);
            return false;
        }
        const size_t read_size = *ret;

        // Calculate speed and progress every second
        recent_read += read_size;
        total_read += read_size;
        buffer_offset += read_size;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || total_read == content_length) {
            const size_t progress = static_cast<uint64_t>(total_read) * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read,
                     content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (!update_started && buffer_offset >= kImagePrefixSize) {
            if (static_cast<uint8_t>(buffer.get()[0]) != ESP_IMAGE_HEADER_MAGIC) {
                ESP_LOGE(TAG, "Invalid firmware image header");
                return false;
            }
            const auto err =
                esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(err));
                return false;
            }
            update_started = true;
        }

        if (buffer_offset == PAGE_SIZE || total_read == content_length) {
            const auto err = esp_ota_write(update_handle, buffer.get(), buffer_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                return false;
            }

            buffer_offset = 0;
        }
    }
    http->Close();

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}

std::optional<std::vector<int>> Ota::ParseVersion(const std::string& version) {
    std::vector<int> numbers;
    size_t start = 0;
    while (true) {
        const size_t end = version.find('.', start);
        const auto segment = version.substr(start, end == std::string::npos ? end : end - start);
        if (segment.empty() || segment.front() < '0' || segment.front() > '9') {
            return std::nullopt;
        }
        int value = 0;
        const auto [ptr, ec] =
            std::from_chars(segment.data(), segment.data() + segment.size(), value);
        if (segment.empty() || ec != std::errc() || ptr != segment.data() + segment.size() ||
            value < 0) {
            return std::nullopt;
        }
        numbers.push_back(value);
        if (end == std::string::npos) {
            return numbers;
        }
        start = end + 1;
    }
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    const auto current = ParseVersion(currentVersion);
    const auto newer = ParseVersion(newVersion);
    if (!current || !newer) {
        ESP_LOGW(TAG, "Ignoring invalid firmware version comparison");
        return false;
    }

    for (size_t i = 0; i < std::min(current->size(), newer->size()); ++i) {
        if ((*newer)[i] > (*current)[i]) {
            return true;
        } else if ((*newer)[i] < (*current)[i]) {
            return false;
        }
    }

    return newer->size() > current->size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32];  // SHA-256 输出为32字节

    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(),
                                       activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.empty()) {
        ESP_LOGW(TAG, "Activation URL unavailable");
        return ESP_ERR_INVALID_ARG;
    }
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();
    if (!http)
        return ESP_FAIL;

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (auto opened = http->Open("POST", url); !opened) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", opened.error().ToString().c_str());
        return ESP_FAIL;
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read activation status: %s",
                 status_code.error().ToString().c_str());
        return ESP_FAIL;
    }
    if (*status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d", *status_code);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
