#include "ali_provider.h"

#include <cstring>
#include <memory>
#include "ali_sse.h"
#include "ali_url.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "mbedtls/base64.h"

namespace voice_tts {
namespace {
constexpr char kUrl[] =
    "https://dashscope.aliyuncs.com/api/v1/services/audio/tts/SpeechSynthesizer";
constexpr size_t kMaxJson = 8192;
constexpr size_t kMaxWav = 1024 * 1024;
constexpr size_t kMaxStreamPcm = 4 * 1024 * 1024;
struct ClientDeleter {
    void operator()(esp_http_client* p) const { esp_http_client_cleanup(p); }
};
struct JsonDeleter {
    void operator()(cJSON* p) const { cJSON_Delete(p); }
};
bool Cancelled(const Request& r) { return r.cancelled && r.cancelled(); }

Result ReadBody(esp_http_client_handle_t client, size_t max, std::vector<uint8_t>& out,
                const Request& request) {
    char buffer[2048];
    while (!Cancelled(request)) {
        int n = esp_http_client_read(client, buffer, sizeof(buffer));
        if (n < 0)
            return Result::kNetworkError;
        if (!n)
            break;
        if (out.size() + static_cast<size_t>(n) > max)
            return Result::kTooLarge;
        out.insert(out.end(), buffer, buffer + n);
    }
    if (Cancelled(request))
        return Result::kCancelled;
    return esp_http_client_is_complete_data_received(client) ? Result::kOk : Result::kNetworkError;
}
}  // namespace

Result AliProvider::Synthesize(const Request& request, Response& response) {
    response = {};
    if (request.api_key.empty() || request.api_key.size() > 256 || request.voice_id.empty() ||
        request.voice_id.size() > 128 || request.text.empty() || request.text.size() > 512 ||
        request.api_key.find_first_of("\r\n") != std::string::npos)
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
        !cJSON_AddStringToObject(input, "format", request.pcm_sink ? "pcm" : "wav") ||
        !cJSON_AddNumberToObject(input, "sample_rate", 24000))
        return Result::kNetworkError;
    std::unique_ptr<char, decltype(&cJSON_free)> payload(cJSON_PrintUnformatted(root.get()),
                                                         cJSON_free);
    if (!payload)
        return Result::kNetworkError;
    esp_http_client_config_t config{};
    config.url = kUrl;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 30000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    std::unique_ptr<esp_http_client, ClientDeleter> client(esp_http_client_init(&config));
    std::string auth = "Bearer " + request.api_key;
    if (!client ||
        esp_http_client_set_header(client.get(), "Authorization", auth.c_str()) != ESP_OK ||
        esp_http_client_set_header(client.get(), "Content-Type", "application/json") != ESP_OK ||
        (request.pcm_sink &&
         esp_http_client_set_header(client.get(), "X-DashScope-SSE", "enable") != ESP_OK) ||
        esp_http_client_open(client.get(), std::strlen(payload.get())) != ESP_OK)
        return Result::kNetworkError;
    size_t remaining = std::strlen(payload.get());
    const char* p = payload.get();
    while (remaining && !Cancelled(request)) {
        int n = esp_http_client_write(client.get(), p, remaining);
        if (n <= 0)
            return Result::kNetworkError;
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    if (Cancelled(request))
        return Result::kCancelled;
    if (esp_http_client_fetch_headers(client.get()) < 0)
        return Result::kNetworkError;
    response.http_status = esp_http_client_get_status_code(client.get());
    if (response.http_status != 200)
        return Result::kHttpError;
    if (request.pcm_sink) {
        bool done = false;
        bool got_audio = false;
        size_t total_pcm = 0;
        // Retain one bounded decode buffer for the duration of this response;
        // allocating a new vector for every SSE event fragments the heap.
        std::vector<uint8_t> pcm;
        AliSse sse([&](const std::string& event) {
            if (event == "[DONE]") {
                done = true;
                return true;
            }
            const char* end = nullptr;
            std::unique_ptr<cJSON, JsonDeleter> json(
                cJSON_ParseWithLengthOpts(event.c_str(), event.size() + 1, &end, true));
            if (!json || end != event.c_str() + event.size())
                return false;
            const auto* error = cJSON_GetObjectItemCaseSensitive(json.get(), "code");
            if (cJSON_IsString(error) && error->valuestring && error->valuestring[0])
                return false;
            const auto* output = cJSON_GetObjectItemCaseSensitive(json.get(), "output");
            if (!cJSON_IsObject(output))
                return false;
            const auto* finish = cJSON_GetObjectItemCaseSensitive(output, "finish_reason");
            if (cJSON_IsString(finish) && std::strcmp(finish->valuestring, "stop") == 0)
                done = true;
            const auto* audio = cJSON_GetObjectItemCaseSensitive(output, "audio");
            const auto* data = cJSON_GetObjectItemCaseSensitive(audio, "data");
            if (!data || cJSON_IsNull(data))
                return true;
            if (!cJSON_IsString(data) || !data->valuestring)
                return false;
            const size_t len = std::strlen(data->valuestring);
            if (!len)
                return true;
            pcm.resize((len / 4 + 1) * 3);
            size_t written = 0;
            if (mbedtls_base64_decode(pcm.data(), pcm.size(), &written,
                                      reinterpret_cast<const unsigned char*>(data->valuestring),
                                      len) != 0 ||
                total_pcm + written > kMaxStreamPcm)
                return false;
            total_pcm += written;
            got_audio = got_audio || written != 0;
            return request.pcm_sink(pcm.data(), written);
        });
        char chunk[2048];
        while (!Cancelled(request)) {
            int n = esp_http_client_read(client.get(), chunk, sizeof(chunk));
            if (n < 0)
                return Result::kNetworkError;
            if (!n)
                break;
            if (!sse.Feed(chunk, static_cast<size_t>(n)))
                return Cancelled(request) ? Result::kCancelled : Result::kInvalidResponse;
        }
        if (Cancelled(request))
            return Result::kCancelled;
        if (!esp_http_client_is_complete_data_received(client.get()) || !sse.Complete() || !done ||
            !got_audio)
            return Result::kInvalidResponse;
        response.streamed = true;
        return Result::kOk;
    }
    std::vector<uint8_t> body;
    auto result = ReadBody(client.get(), kMaxJson, body, request);
    if (result != Result::kOk)
        return result;
    body.push_back(0);
    const char* end = nullptr;
    std::unique_ptr<cJSON, JsonDeleter> json(cJSON_ParseWithLengthOpts(
        reinterpret_cast<const char*>(body.data()), body.size(), &end, true));
    if (!json || end != reinterpret_cast<const char*>(body.data()) + body.size() - 1)
        return Result::kInvalidResponse;
    auto* output = cJSON_GetObjectItemCaseSensitive(json.get(), "output");
    auto* audio = cJSON_GetObjectItemCaseSensitive(output, "audio");
    auto* url = cJSON_GetObjectItemCaseSensitive(audio, "url");
    std::string https;
    if (!cJSON_IsString(url) || !url->valuestring || !AliAudioHttpsUrl(url->valuestring, https))
        return Result::kInvalidResponse;
    // Release the authenticated connection before opening a separate download.
    client.reset();
    config.url = https.c_str();
    config.method = HTTP_METHOD_GET;
    std::unique_ptr<esp_http_client, ClientDeleter> download(esp_http_client_init(&config));
    if (!download || esp_http_client_open(download.get(), 0) != ESP_OK ||
        esp_http_client_fetch_headers(download.get()) < 0)
        return Result::kNetworkError;
    response.http_status = esp_http_client_get_status_code(download.get());
    if (response.http_status != 200)
        return Result::kHttpError;
    result = ReadBody(download.get(), kMaxWav, response.wav, request);
    if (result != Result::kOk) {
        response.wav.clear();
        return result;
    }
    if (response.wav.size() < 12 || std::memcmp(response.wav.data(), "RIFF", 4) ||
        std::memcmp(response.wav.data() + 8, "WAVE", 4)) {
        response.wav.clear();
        return Result::kInvalidResponse;
    }
    return Result::kOk;
}
}  // namespace voice_tts
