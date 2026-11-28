#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A provider is independent of the transport used for the Xiaozhi conversation.
// Calls are synchronous and must run in a dedicated worker, never the audio task.
namespace voice_tts {
enum class Result {
    kOk,
    kInvalidInput,
    kCancelled,
    kNetworkError,
    kHttpError,
    kInvalidResponse,
    kTooLarge
};

struct Request {
    std::string api_key;
    std::string text;
    std::string voice_id;
    std::function<bool()> cancelled;
    // Incremental Ogg/Opus bytes; called only by the synthesis worker.
    std::function<bool(const uint8_t*, size_t)> opus_sink;
};

struct Response {
    int http_status = 0;
    unsigned header_ms = 0;
    // Bounded, content-free diagnostics; timings include scheduling delays.
    const char* stream_detail = "none";
    size_t stream_bytes = 0;
    size_t stream_events = 0;
    unsigned stream_max_read_ms = 0;
    unsigned stream_max_process_ms = 0;
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual Result Synthesize(const Request& request, Response& response) = 0;
};
}  // namespace voice_tts
