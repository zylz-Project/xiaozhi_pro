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
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <algorithm>
#include <charconv>
#include <cstring>
#include <sstream>
#include <vector>

#define TAG "Ota"

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
                const auto data = http->ReadAll();
                http->Close();
                if (!data.empty() && (expected == 0 || data.size() == expected)) {
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

    has_mqtt_config_ = false;
    cJSON* mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON* item = NULL;
        cJSON_ArrayForEach (item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "No mqtt section found !");
    }

    has_websocket_config_ = false;
    cJSON* websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON* item = NULL;
        cJSON_ArrayForEach (item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found!");
    }

    has_server_time_ = false;
    cJSON* server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON* timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON* timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");

        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;

            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000);  // 转换分钟为毫秒
            }

            tv.tv_sec = (time_t)(ts / 1000);                          // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }

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

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
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
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", *status_code,
                 http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
