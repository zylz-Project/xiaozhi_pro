#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Optional board capability. All calls must be nonblocking; completion may run
// on a worker thread and must be scheduled onto the application task.
class ExternalTts {
public:
    virtual ~ExternalTts() = default;
    virtual bool Ready() = 0;
    virtual bool Begin(uint32_t playback_generation, std::function<void(bool)> completion) = 0;
    virtual bool Enqueue(const std::string& text) = 0;
    // A board may recognize a server control caption that requires returning
    // to the original audio stream. Default keeps existing boards unchanged.
    virtual bool ShouldUseServerAudio(const std::string&) const { return false; }
    virtual bool ShouldResumeExternalAudio(const std::string&) const { return false; }
    virtual void Finish() = 0;
    virtual void Cancel() = 0;
};
