#pragma once

#include <functional>
#include <string>

#include "voice_tts_provider.h"

namespace voice_tts {
// Runs on the voice worker. next_text must return promptly and set finished
// only after the last queued text has been returned.
struct StreamRequest {
    std::string api_key;
    std::string voice_id;
    std::function<bool()> cancelled;
    std::function<void(std::string&, bool&)> next_text;
    std::function<bool(const uint8_t*, size_t)> pcm_sink;
    std::function<void()> sentence_end;
};
struct StreamResponse {
    bool text_committed = false;
};
Result SynthesizeStream(const StreamRequest& request, StreamResponse& response);
}  // namespace voice_tts
