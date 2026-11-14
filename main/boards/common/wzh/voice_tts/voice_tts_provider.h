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
    // Optional native-rate signed PCM16LE stream; called only by the worker.
    std::function<bool(const uint8_t*, size_t)> pcm_sink;
};

struct Response {
    // WAV bytes, owned by the caller. Verify its format before playback.
    std::vector<uint8_t> wav;
    int http_status = 0;
    bool streamed = false;
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
