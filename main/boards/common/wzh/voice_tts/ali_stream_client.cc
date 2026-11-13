#include "ali_stream_client.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "ali_stream_protocol.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"

namespace voice_tts {
namespace {
constexpr char kHost[] = "dashscope.aliyuncs.com";
constexpr char kPath[] = "/api-ws/v1/inference";
[[maybe_unused]] constexpr char kTag[] = "AliStream";
constexpr size_t kMaxEvent = 8192;
// This is a cumulative per-turn limit, not an allocation: 64 MiB is over
// twenty minutes of PCM16 at 24 kHz, while the actual ring remains bounded.
constexpr size_t kMaxAudio = 64 * 1024 * 1024;
struct JsonDeleter {
    void operator()(cJSON* value) const { cJSON_Delete(value); }
};
using Json = std::unique_ptr<cJSON, JsonDeleter>;
struct Transport {
    esp_transport_handle_t tls = nullptr;
    esp_transport_handle_t ws = nullptr;
    ~Transport() {
        if (ws) {
            esp_transport_close(ws);
            esp_transport_destroy(ws);
        }
        if (tls)
            esp_transport_destroy(tls);
    }
};
bool Cancelled(const StreamRequest& request) { return request.cancelled && request.cancelled(); }
std::string TaskId() {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    char id[37];
    std::snprintf(id, sizeof(id),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
                  bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
                  bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return id;
}
bool Send(esp_transport_handle_t ws, const char* action, const std::string& id,
          const std::string& value) {
    Json root(cJSON_CreateObject());
    if (!root)
        return false;
    cJSON* header = cJSON_AddObjectToObject(root.get(), "header");
    cJSON* payload = cJSON_AddObjectToObject(root.get(), "payload");
    if (!header || !payload || !cJSON_AddStringToObject(header, "action", action) ||
        !cJSON_AddStringToObject(header, "task_id", id.c_str()) ||
        !cJSON_AddStringToObject(header, "streaming", "duplex"))
        return false;
    if (std::strcmp(action, "run-task") == 0) {
        cJSON* params = cJSON_AddObjectToObject(payload, "parameters");
        if (!params || !cJSON_AddStringToObject(payload, "task_group", "audio") ||
            !cJSON_AddStringToObject(payload, "task", "tts") ||
            !cJSON_AddStringToObject(payload, "function", "SpeechSynthesizer") ||
            !cJSON_AddStringToObject(payload, "model", "qwen-audio-3.1-tts-flash") ||
            !cJSON_AddStringToObject(params, "text_type", "PlainText") ||
            !cJSON_AddStringToObject(params, "voice", value.c_str()) ||
            !cJSON_AddStringToObject(params, "format", "pcm") ||
            !cJSON_AddNumberToObject(params, "sample_rate", 24000) ||
            !cJSON_AddStringToObject(
                params, "instruction",
                "按照文本原有语言，以自然亲切的交谈语气表达，随语意和标点自然变化"
                "语速、停顿和情绪，避免播报腔。"))
            return false;
    }
    cJSON* input = cJSON_AddObjectToObject(payload, "input");
    if (!input || (std::strcmp(action, "continue-task") == 0 &&
                   !cJSON_AddStringToObject(input, "text", value.c_str())))
        return false;
    std::unique_ptr<char, decltype(&cJSON_free)> encoded(cJSON_PrintUnformatted(root.get()),
                                                         cJSON_free);
    if (!encoded)
        return false;
    const size_t length = std::strlen(encoded.get());
    return length <= 4096 &&
           esp_transport_ws_send_raw(ws,
                                     static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_TEXT |
                                                                         WS_TRANSPORT_OPCODES_FIN),
                                     encoded.get(), static_cast<int>(length),
                                     5000) == static_cast<int>(length);
}
}  // namespace

Result SynthesizeStream(const StreamRequest& request, StreamResponse& response) {
    response = {};
    [[maybe_unused]] const int64_t started_at = esp_timer_get_time();
    if (request.api_key.empty() || request.api_key.size() > 256 || request.voice_id.empty() ||
        request.voice_id.size() > 128 ||
        request.api_key.find_first_of("\r\n") != std::string::npos || !request.next_text ||
        !request.pcm_sink || !request.sentence_end)
        return Result::kInvalidInput;
    if (Cancelled(request))
        return Result::kCancelled;
    Transport connection;
    connection.tls = esp_transport_ssl_init();
    if (!connection.tls)
        return Result::kNetworkError;
    esp_transport_ssl_crt_bundle_attach(connection.tls, esp_crt_bundle_attach);
    connection.ws = esp_transport_ws_init(connection.tls);
    if (!connection.ws)
        return Result::kNetworkError;
    esp_transport_ws_set_path(connection.ws, kPath);
    const std::string headers = "Authorization: Bearer " + request.api_key + "\r\n";
    if (esp_transport_ws_set_headers(connection.ws, headers.c_str()) != ESP_OK ||
        esp_transport_connect(connection.ws, kHost, 443, 10000) < 0)
        return Cancelled(request) ? Result::kCancelled : Result::kNetworkError;
    ESP_LOGI(kTag, "Connected: elapsed_ms=%u",
             static_cast<unsigned>((esp_timer_get_time() - started_at) / 1000));
    const std::string id = TaskId();
    AliStreamProtocol protocol(id);
    if (!Send(connection.ws, "run-task", id, request.voice_id))
        return Result::kNetworkError;

    bool started = false, finish_sent = false, got_audio = false;
    bool odd_byte = false;
    size_t total_audio = 0;
    int frame_remaining = 0;
    bool frame_fin = false;
    int message_opcode = -1;
    std::string text;
    int64_t last_activity = esp_timer_get_time();
    while (!Cancelled(request)) {
        if (started && !finish_sent) {
            std::string next;
            bool finished = false;
            request.next_text(next, finished);
            if (!next.empty()) {
                if (next.size() > 512)
                    return Result::kInvalidInput;
                // Once a send starts, retrying the text may duplicate speech or billing.
                response.text_committed = true;
                if (!Send(connection.ws, "continue-task", id, next))
                    return Result::kNetworkError;
                last_activity = esp_timer_get_time();
            }
            if (finished) {
                if (!Send(connection.ws, "finish-task", id, {}) || !protocol.MarkFinishSent())
                    return Result::kNetworkError;
                finish_sent = true;
                last_activity = esp_timer_get_time();
            }
        }
        if (esp_timer_get_time() - last_activity > 20000000)
            return Result::kNetworkError;
        int ready = esp_transport_poll_read(connection.ws, 100);
        if (ready < 0)
            return Result::kNetworkError;
        if (!ready)
            continue;
        char buffer[2048];
        int count = esp_transport_read(connection.ws, buffer, sizeof(buffer), 2000);
        if (count < 0)
            return Result::kNetworkError;
        if (!count)
            continue;  // Handled ping/pong or no data.
        last_activity = esp_timer_get_time();
        if (!frame_remaining) {
            const int opcode = esp_transport_ws_get_read_opcode(connection.ws);
            frame_remaining = esp_transport_ws_get_read_payload_len(connection.ws);
            frame_fin = esp_transport_ws_get_fin_flag(connection.ws);
            if (frame_remaining <= 0 || count > frame_remaining)
                return Result::kInvalidResponse;
            if (opcode == WS_TRANSPORT_OPCODES_TEXT || opcode == WS_TRANSPORT_OPCODES_BINARY) {
                if (message_opcode != -1)
                    return Result::kInvalidResponse;
                if (opcode == WS_TRANSPORT_OPCODES_BINARY && !protocol.OnAudioMessage())
                    return Result::kInvalidResponse;
                message_opcode = opcode;
                text.clear();
            } else if (opcode != WS_TRANSPORT_OPCODES_CONT || message_opcode == -1) {
                return Result::kInvalidResponse;
            }
        }
        frame_remaining -= count;
        if (message_opcode == WS_TRANSPORT_OPCODES_TEXT) {
            if (static_cast<size_t>(count) > kMaxEvent - text.size())
                return Result::kTooLarge;
            text.append(buffer, static_cast<size_t>(count));
        } else {
            if (static_cast<size_t>(count) > kMaxAudio - total_audio)
                return Result::kTooLarge;
            if (!got_audio)
                ESP_LOGI(kTag, "First PCM: elapsed_ms=%u",
                         static_cast<unsigned>((esp_timer_get_time() - started_at) / 1000));
            total_audio += static_cast<size_t>(count);
            odd_byte = odd_byte != ((count & 1) != 0);
            if (!request.pcm_sink(reinterpret_cast<const uint8_t*>(buffer), count))
                return Cancelled(request) ? Result::kCancelled : Result::kInvalidResponse;
            got_audio = true;
        }
        if (frame_remaining || !frame_fin)
            continue;
        if (message_opcode == WS_TRANSPORT_OPCODES_TEXT) {
            switch (protocol.OnText(text.data(), text.size())) {
                case AliStreamProtocol::Event::kStarted:
                    started = true;
                    ESP_LOGI(kTag, "Task ready: elapsed_ms=%u",
                             static_cast<unsigned>((esp_timer_get_time() - started_at) / 1000));
                    break;
                case AliStreamProtocol::Event::kSentenceEnd:
                    request.sentence_end();
                    break;
                case AliStreamProtocol::Event::kFinished:
                    return got_audio && !odd_byte ? Result::kOk : Result::kInvalidResponse;
                case AliStreamProtocol::Event::kError:
                    return Result::kInvalidResponse;
                default:
                    break;
            }
        }
        message_opcode = -1;
    }
    return Result::kCancelled;
}
}  // namespace voice_tts
