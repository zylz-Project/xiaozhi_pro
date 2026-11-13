#pragma once

#include <cstddef>
#include <string>

namespace voice_tts {
// Validates server events for one DashScope duplex task. The caller owns
// transport framing, limits and playback, and never logs event contents.
class AliStreamProtocol {
public:
    enum class Event { kStarted, kAudioFollows, kSentenceEnd, kOther, kFinished, kError };
    explicit AliStreamProtocol(std::string task_id);
    Event OnText(const char* data, size_t size);
    bool OnAudioMessage();
    bool MarkFinishSent();

private:
    enum class State { kStarting, kRunning, kFinishing, kFinished, kFailed };
    Event Fail();
    std::string task_id_;
    State state_ = State::kStarting;
    bool audio_expected_ = false;
};
}  // namespace voice_tts
