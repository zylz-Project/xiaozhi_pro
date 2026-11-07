#include "ali_enrollment.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

namespace voice_tts {
namespace {
constexpr char kUrl[] = "https://dashscope.aliyuncs.com/api/v1/services/audio/tts/customization";
constexpr char kCreateStart[] =
    "{\"model\":\"voice-enrollment\",\"input\":{\"action\":\"create_voice\",\"target_model\":"
    "\"qwen-audio-3.1-tts-flash\",\"prefix\":\"";
constexpr char kCreateAudio[] = "\",\"audio\":\"data:audio/wav;base64,";
constexpr char kVoiceIdPrefix[] = "qwen-audio-3.1-tts-flash-";
constexpr char kSuffix[] = "\",\"language_hints\":[\"zh\"],\"max_prompt_audio_length\":20}}";
struct ClientDelete {
    void operator()(esp_http_client* p) const { esp_http_client_cleanup(p); }
};
struct JsonDelete {
    void operator()(cJSON* p) const { cJSON_Delete(p); }
};
using Client = std::unique_ptr<esp_http_client, ClientDelete>;
using Json = std::unique_ptr<cJSON, JsonDelete>;

bool ValidPrefix(const std::string& prefix) {
    if (prefix.empty() || prefix.size() > 10)
        return false;
    for (const unsigned char c : prefix)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    return true;
}

bool OwnedVoice(const std::string& voice, const std::string& prefix) {
    const std::string expected = std::string(kVoiceIdPrefix) + prefix + "-";
    if (voice.size() <= expected.size() || voice.size() > 128 ||
        voice.compare(0, expected.size(), expected) != 0)
        return false;
    for (const unsigned char c : voice)
        if (c < 0x21 || c > 0x7e)
            return false;
    return true;
}

Client Open(const std::string& key, size_t length) {
    esp_http_client_config_t config{};
    config.url = kUrl;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 30000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    Client client(esp_http_client_init(&config));
    const std::string auth = "Bearer " + key;
    if (!client ||
        esp_http_client_set_header(client.get(), "Authorization", auth.c_str()) != ESP_OK ||
        esp_http_client_set_header(client.get(), "Content-Type", "application/json") != ESP_OK ||
        esp_http_client_open(client.get(), length) != ESP_OK)
        return {};
    return client;
}
bool Write(esp_http_client_handle_t client, const void* bytes, size_t count,
           const std::function<bool()>& cancelled) {
    const char* p = static_cast<const char*>(bytes);
    while (count && !cancelled()) {
        int n = esp_http_client_write(client, p, count);
        if (n <= 0)
            return false;
        p += n;
        count -= static_cast<size_t>(n);
    }
    return !cancelled();
}
Json Response(esp_http_client_handle_t client, const std::function<bool()>& cancelled,
              int* http_status) {
    if (cancelled() || esp_http_client_fetch_headers(client) < 0)
        return {};
    const int status = esp_http_client_get_status_code(client);
    if (http_status)
        *http_status = status;
    if (status != 200) {
        ESP_LOGW("AliEnrollment", "Request failed: HTTP=%d", status);
        return {};
    }
    std::string body;
    char buf[512];
    while (!cancelled()) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0)
            return {};
        if (!n)
            break;
        if (body.size() + n > 8192)
            return {};
        body.append(buf, n);
    }
    if (cancelled() || !esp_http_client_is_complete_data_received(client))
        return {};
    const char* end = nullptr;
    Json result(cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, &end, true));
    if (!result || end != body.c_str() + body.size())
        return {};
    return result;
}
void Put16(uint8_t* p, uint16_t v) {
    p[0] = v & 255;
    p[1] = v >> 8;
}
void Put32(uint8_t* p, uint32_t v) {
    Put16(p, v & 65535);
    Put16(p + 2, v >> 16);
}
}  // namespace

bool CreateAliVoice(const std::string& key, const int16_t* samples, size_t count,
                    const std::function<bool()>& cancelled, std::string& voice, int* http_status,
                    const std::string& prefix, bool* create_ack, bool* request_submitted) {
    voice.clear();
    if (create_ack)
        *create_ack = false;
    if (request_submitted)
        *request_submitted = false;
    if (http_status)
        *http_status = 0;
    if (key.empty() || key.size() > 256 || key.find_first_of("\r\n") != std::string::npos ||
        !ValidPrefix(prefix) || !samples || count < 5 * 16000 || count > 30 * 16000 || cancelled())
        return false;
    const size_t pcm_bytes = count * sizeof(int16_t);
    uint8_t header[44]{};
    std::memcpy(header, "RIFF", 4);
    Put32(header + 4, 36 + pcm_bytes);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    Put32(header + 16, 16);
    Put16(header + 20, 1);
    Put16(header + 22, 1);
    Put32(header + 24, 16000);
    Put32(header + 28, 32000);
    Put16(header + 32, 2);
    Put16(header + 34, 16);
    std::memcpy(header + 36, "data", 4);
    Put32(header + 40, pcm_bytes);
    const size_t raw_bytes = sizeof(header) + pcm_bytes;
    const size_t encoded_bytes = 4 * ((raw_bytes + 2) / 3);
    [[maybe_unused]] const int64_t upload_start = esp_timer_get_time();
    const std::string start = std::string(kCreateStart) + prefix + kCreateAudio;
    Client client = Open(key, start.size() + encoded_bytes + sizeof(kSuffix) - 1);
    if (!client || !Write(client.get(), start.data(), start.size(), cancelled))
        return false;
    // Larger TLS writes reduce per-write overhead. Keep a bounded stack
    // fallback if PSRAM is fragmented or nearly full.
    uint8_t fallback_chunk[768];
    unsigned char fallback_encoded[1025];
    constexpr size_t kChunk = 3072;
    constexpr size_t kEncoded = 4097;
    std::unique_ptr<uint8_t, decltype(&free)> scratch(
        static_cast<uint8_t*>(
            heap_caps_malloc(kChunk + kEncoded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        free);
    uint8_t* chunk = scratch ? scratch.get() : fallback_chunk;
    unsigned char* encoded = scratch ? scratch.get() + kChunk : fallback_encoded;
    const size_t chunk_capacity = scratch ? kChunk : sizeof(fallback_chunk);
    const size_t encoded_capacity = scratch ? kEncoded : sizeof(fallback_encoded);
    for (size_t offset = 0; offset < raw_bytes;) {
        const size_t n = std::min(chunk_capacity, raw_bytes - offset);
        size_t header_part = offset < sizeof(header) ? std::min(n, sizeof(header) - offset) : 0;
        if (header_part)
            std::memcpy(chunk, header + offset, header_part);
        if (n > header_part) {
            const size_t pcm_offset = offset + header_part - sizeof(header);
            std::memcpy(chunk + header_part, reinterpret_cast<const uint8_t*>(samples) + pcm_offset,
                        n - header_part);
        }
        size_t out = 0;
        if (mbedtls_base64_encode(encoded, encoded_capacity, &out, chunk, n) != 0 ||
            !Write(client.get(), encoded, out, cancelled))
            return false;
        offset += n;
    }
    // Once closing JSON bytes may reach the server, a timeout is ambiguous.
    if (cancelled())
        return false;
    if (request_submitted)
        *request_submitted = true;
    if (!Write(client.get(), kSuffix, sizeof(kSuffix) - 1, cancelled))
        return false;
    [[maybe_unused]] const int64_t upload_end = esp_timer_get_time();
    ESP_LOGI("AliEnrollment", "Reference upload complete: bytes=%u elapsed_ms=%u",
             static_cast<unsigned>(raw_bytes),
             static_cast<unsigned>((upload_end - upload_start) / 1000));
    Json response = Response(client.get(), cancelled, http_status);
    ESP_LOGI("AliEnrollment", "Create response: elapsed_ms=%u valid=%d",
             static_cast<unsigned>((esp_timer_get_time() - upload_end) / 1000),
             response != nullptr);
    auto* output = response ? cJSON_GetObjectItemCaseSensitive(response.get(), "output") : nullptr;
    auto* id = output ? cJSON_GetObjectItemCaseSensitive(output, "voice_id") : nullptr;
    if (!cJSON_IsString(id) || !id->valuestring || !OwnedVoice(id->valuestring, prefix))
        return false;
    const std::string candidate(id->valuestring);
    if (create_ack)
        *create_ack = true;
    client.reset();
    [[maybe_unused]] const int64_t poll_start = esp_timer_get_time();
    // A create timeout is ambiguous; never automatically retry the create operation.
    for (int attempt = 0; attempt < 30 && !cancelled(); ++attempt) {
        Json query(cJSON_CreateObject());
        auto* input = query ? cJSON_AddObjectToObject(query.get(), "input") : nullptr;
        if (!input || !cJSON_AddStringToObject(query.get(), "model", "voice-enrollment") ||
            !cJSON_AddStringToObject(input, "action", "query_voice") ||
            !cJSON_AddStringToObject(input, "voice_id", candidate.c_str()))
            return false;
        std::unique_ptr<char, decltype(&cJSON_free)> payload(cJSON_PrintUnformatted(query.get()),
                                                             cJSON_free);
        if (!payload)
            return false;
        client = Open(key, std::strlen(payload.get()));
        if (!client || !Write(client.get(), payload.get(), std::strlen(payload.get()), cancelled))
            return false;
        response = Response(client.get(), cancelled, http_status);
        output = response ? cJSON_GetObjectItemCaseSensitive(response.get(), "output") : nullptr;
        auto* status = output ? cJSON_GetObjectItemCaseSensitive(output, "status") : nullptr;
        if (!cJSON_IsString(status) || !status->valuestring)
            return false;
        if (std::strcmp(status->valuestring, "OK") == 0) {
            ESP_LOGI("AliEnrollment", "Voice ready: polls=%d elapsed_ms=%u", attempt + 1,
                     static_cast<unsigned>((esp_timer_get_time() - poll_start) / 1000));
            voice = candidate;
            return true;
        }
        if (std::strcmp(status->valuestring, "DEPLOYING") != 0)
            return false;
        client.reset();
        for (int i = 0; i < 10 && !cancelled(); ++i)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

bool ListAliVoices(const std::string& key, const std::string& prefix,
                   const std::function<bool()>& cancelled, std::vector<std::string>& voices) {
    voices.clear();
    if (key.empty() || key.size() > 256 || key.find_first_of("\r\n") != std::string::npos ||
        !ValidPrefix(prefix) || cancelled())
        return false;
    // A bounded number of pages makes malformed or unexpectedly broad server results fail closed.
    for (int page = 0; page < 32 && !cancelled(); ++page) {
        Json query(cJSON_CreateObject());
        auto* input = query ? cJSON_AddObjectToObject(query.get(), "input") : nullptr;
        if (!input || !cJSON_AddStringToObject(query.get(), "model", "voice-enrollment") ||
            !cJSON_AddStringToObject(input, "action", "list_voice") ||
            !cJSON_AddStringToObject(input, "prefix", prefix.c_str()) ||
            !cJSON_AddNumberToObject(input, "page_index", page) ||
            !cJSON_AddNumberToObject(input, "page_size", 20))
            return false;
        std::unique_ptr<char, decltype(&cJSON_free)> payload(cJSON_PrintUnformatted(query.get()),
                                                             cJSON_free);
        if (!payload)
            return false;
        Client client = Open(key, std::strlen(payload.get()));
        if (!client || !Write(client.get(), payload.get(), std::strlen(payload.get()), cancelled))
            return false;
        Json response = Response(client.get(), cancelled, nullptr);
        auto* output =
            response ? cJSON_GetObjectItemCaseSensitive(response.get(), "output") : nullptr;
        auto* list = output ? cJSON_GetObjectItemCaseSensitive(output, "voice_list") : nullptr;
        if (!cJSON_IsArray(list))
            return false;
        const int count = cJSON_GetArraySize(list);
        if (count < 0 || count > 20)
            return false;
        for (int i = 0; i < count; ++i) {
            const auto* item = cJSON_GetArrayItem(list, i);
            const auto* id = cJSON_GetObjectItemCaseSensitive(item, "voice_id");
            if (!cJSON_IsString(id) || !id->valuestring || std::strlen(id->valuestring) > 128)
                return false;
            if (OwnedVoice(id->valuestring, prefix)) {
                if (std::find(voices.begin(), voices.end(), id->valuestring) != voices.end())
                    return false;
                voices.emplace_back(id->valuestring);
                if (voices.size() == 11)
                    return true;
            }
        }
        // Do not assume the service honored the requested page size.
        if (count == 0)
            return true;
    }
    return false;
}

bool DeleteAliVoice(const std::string& key, const std::string& prefix, const std::string& voice,
                    const std::function<bool()>& cancelled) {
    if (key.empty() || key.size() > 256 || key.find_first_of("\r\n") != std::string::npos ||
        !ValidPrefix(prefix) || !OwnedVoice(voice, prefix) || cancelled())
        return false;
    Json query(cJSON_CreateObject());
    auto* input = query ? cJSON_AddObjectToObject(query.get(), "input") : nullptr;
    if (!input || !cJSON_AddStringToObject(query.get(), "model", "voice-enrollment") ||
        !cJSON_AddStringToObject(input, "action", "delete_voice") ||
        !cJSON_AddStringToObject(input, "voice_id", voice.c_str()))
        return false;
    std::unique_ptr<char, decltype(&cJSON_free)> payload(cJSON_PrintUnformatted(query.get()),
                                                         cJSON_free);
    if (!payload)
        return false;
    Client client = Open(key, std::strlen(payload.get()));
    if (!client || !Write(client.get(), payload.get(), std::strlen(payload.get()), cancelled))
        return false;
    Json response = Response(client.get(), cancelled, nullptr);
    return response && !cJSON_GetObjectItemCaseSensitive(response.get(), "code") &&
           cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(response.get(), "output"));
}
}  // namespace voice_tts
