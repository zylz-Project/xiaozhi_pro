#include "ali_provider.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <utility>
#include "ali_sse.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "mbedtls/base64.h"

namespace voice_tts {
namespace {
constexpr char kUrl[] =
    "https://dashscope.aliyuncs.com/api/v1/services/audio/tts/SpeechSynthesizer";
constexpr size_t kMaxStreamBytes = 4 * 1024 * 1024;
struct ResponseConnection {
    esp_http_client_handle_t client;
    bool reusable = false;
    ~ResponseConnection() {
        if (!reusable)
            esp_http_client_close(client);
    }
};
struct JsonDeleter {
    void operator()(cJSON* p) const { cJSON_Delete(p); }
};
bool Cancelled(const Request& r) { return r.cancelled && r.cancelled(); }

}  // namespace

AliProvider::~AliProvider() { Close(); }

void AliProvider::Close() {
#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC && CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
    client_ = nullptr;
    client_owner_.reset();
#else
    if (auto client = std::exchange(client_, nullptr))
        esp_http_client_cleanup(client);
#endif
    reuse_connection_ = false;
}

Result AliProvider::Synthesize(const Request& request, Response& response) {
    response = {};
    if (request.api_key.empty() || request.api_key.size() > 256 || request.voice_id.empty() ||
        request.voice_id.size() > 128 || request.text.empty() || request.text.size() > 512 ||
        request.api_key.find_first_of("\r\n") != std::string::npos || !request.opus_sink)
        return Result::kInvalidInput;
    if (Cancelled(request))
        return Result::kCancelled;
    std::unique_ptr<cJSON, JsonDeleter> root(cJSON_CreateObject());
    if (!root)
        return Result::kNetworkError;
    auto* input = cJSON_AddObjectToObject(root.get(), "input");
    if (!input || !cJSON_AddStringToObject(root.get(), "model", "qwen-audio-3.1-tts-flash") ||
        !cJSON_AddStringToObject(input, "text", request.text.c_str()) ||
        !cJSON_AddStringToObject(input, "voice", request.voice_id.c_str()) ||
        !cJSON_AddStringToObject(input, "instruction",
                                 "按照文本原有语言，以自然亲切的交谈语气表达，随语意和标点自然变化"
                                 "语速、停顿和情绪，避免播报腔。") ||
        !cJSON_AddStringToObject(input, "format", "opus") ||
        !cJSON_AddNumberToObject(input, "sample_rate", 24000))
        return Result::kNetworkError;
    std::unique_ptr<char, decltype(&cJSON_free)> payload(cJSON_PrintUnformatted(root.get()),
                                                         cJSON_free);
    if (!payload)
        return Result::kNetworkError;
    if (!client_) {
        esp_http_client_config_t config{};
        config.url = kUrl;
        config.method = HTTP_METHOD_POST;
        config.timeout_ms = 5000;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.disable_auto_redirect = true;
#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC && CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
        client_owner_ = CreateVoiceHttpClient(config);
        client_ = client_owner_.get();
#else
        client_ = esp_http_client_init(&config);
#endif
        if (!client_)
            return Result::kNetworkError;
    }
    ResponseConnection connection{client_};
    const bool previously_reusable = std::exchange(reuse_connection_, false);
    const auto started_at = std::chrono::steady_clock::now();
    const std::string auth = "Bearer " + request.api_key;
    if (esp_http_client_set_header(client_, "Authorization", auth.c_str()) != ESP_OK ||
        esp_http_client_set_header(client_, "Content-Type", "application/json") != ESP_OK ||
        esp_http_client_set_header(client_, "X-DashScope-SSE", "enable") != ESP_OK)
        return Result::kNetworkError;
    const auto length = std::strlen(payload.get());
    if (esp_http_client_open(client_, length) != ESP_OK) {
        // A retained connection can be closed by the peer between sentences.
        // Retry before writing the body only.
        if (!previously_reusable)
            return Result::kNetworkError;
        esp_http_client_close(client_);
        if (Cancelled(request) || esp_http_client_open(client_, length) != ESP_OK)
            return Cancelled(request) ? Result::kCancelled : Result::kNetworkError;
    }
    size_t remaining = std::strlen(payload.get());
    const char* p = payload.get();
    while (remaining && !Cancelled(request)) {
        int n = esp_http_client_write(client_, p, remaining);
        if (n <= 0)
            return Result::kNetworkError;
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    if (Cancelled(request))
        return Result::kCancelled;
    if (esp_http_client_fetch_headers(client_) < 0)
        return Result::kNetworkError;
    response.header_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at).count();
    response.http_status = esp_http_client_get_status_code(client_);
    if (response.http_status != 200)
        return Result::kHttpError;
    bool done = false;
    bool got_audio = false;
    size_t total_audio = 0;
    // Retain one bounded decode buffer for the duration of this response;
    // allocating a new vector for every SSE event fragments the heap.
    std::vector<uint8_t> opus;
    AliSse sse([&](const std::string& event) {
        ++response.stream_events;
        if (event == "[DONE]") {
            done = true;
            return true;
        }
        const char* end = nullptr;
        std::unique_ptr<cJSON, JsonDeleter> json(
            cJSON_ParseWithLengthOpts(event.c_str(), event.size() + 1, &end, true));
        if (!json || end != event.c_str() + event.size()) {
            response.stream_detail = "event_json";
            return false;
        }
        const auto* error = cJSON_GetObjectItemCaseSensitive(json.get(), "code");
        if (cJSON_IsString(error) && error->valuestring && error->valuestring[0]) {
            response.stream_detail = "service_error";
            return false;
        }
        const auto* output = cJSON_GetObjectItemCaseSensitive(json.get(), "output");
        if (!cJSON_IsObject(output)) {
            response.stream_detail = "missing_output";
            return false;
        }
        const auto* finish = cJSON_GetObjectItemCaseSensitive(output, "finish_reason");
        if (cJSON_IsString(finish) && std::strcmp(finish->valuestring, "stop") == 0)
            done = true;
        const auto* audio = cJSON_GetObjectItemCaseSensitive(output, "audio");
        const auto* data = cJSON_GetObjectItemCaseSensitive(audio, "data");
        if (!data || cJSON_IsNull(data))
            return true;
        if (!cJSON_IsString(data) || !data->valuestring) {
            response.stream_detail = "audio_type";
            return false;
        }
        const size_t len = std::strlen(data->valuestring);
        if (!len)
            return true;
        opus.resize((len / 4 + 1) * 3);
        size_t written = 0;
        if (mbedtls_base64_decode(opus.data(), opus.size(), &written,
                                  reinterpret_cast<const unsigned char*>(data->valuestring),
                                  len) != 0) {
            response.stream_detail = "audio_base64";
            return false;
        }
        if (total_audio + written > kMaxStreamBytes) {
            response.stream_detail = "audio_limit";
            return false;
        }
        total_audio += written;
        got_audio = got_audio || written != 0;
        if (!request.opus_sink(opus.data(), written)) {
            response.stream_detail = "sink_stopped";
            return false;
        }
        return true;
    });
    char chunk[2048];
    while (!Cancelled(request)) {
        const auto read_started = std::chrono::steady_clock::now();
        int n = esp_http_client_read(client_, chunk, sizeof(chunk));
        const auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - read_started)
                                 .count();
        response.stream_max_read_ms =
            std::max(response.stream_max_read_ms, static_cast<unsigned>(read_ms));
        if (n < 0) {
            response.stream_detail = "read_error";
            return Result::kNetworkError;
        }
        if (!n)
            break;
        response.stream_bytes += static_cast<size_t>(n);
        const auto process_started = std::chrono::steady_clock::now();
        const bool accepted = sse.Feed(chunk, static_cast<size_t>(n));
        const auto process_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - process_started)
                                    .count();
        response.stream_max_process_ms =
            std::max(response.stream_max_process_ms, static_cast<unsigned>(process_ms));
        if (!accepted) {
            if (std::strcmp(response.stream_detail, "none") == 0)
                response.stream_detail = "framing_limit";
            return Cancelled(request) ? Result::kCancelled : Result::kInvalidResponse;
        }
    }
    if (Cancelled(request))
        return Result::kCancelled;
    if (!esp_http_client_is_complete_data_received(client_) || !sse.Complete() || !done ||
        !got_audio) {
        response.stream_detail = !esp_http_client_is_complete_data_received(client_)
                                     ? "http_incomplete"
                                 : !sse.Complete() ? "sse_incomplete"
                                 : !done           ? "missing_end"
                                                   : "no_audio";
        return Result::kInvalidResponse;
    }
    response.stream_detail = "ok";
    connection.reusable = reuse_connection_ = esp_http_client_is_persistent_connection(client_);
    return Result::kOk;
}
}  // namespace voice_tts
