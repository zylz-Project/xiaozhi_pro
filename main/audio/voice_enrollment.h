#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Optional board-owned enrollment. Capture runs on the audio task and must be bounded.
class VoiceEnrollment {
public:
    virtual ~VoiceEnrollment() = default;
    virtual bool Start(std::function<void(bool)> capture_switch,
                       std::function<void(bool)> complete) = 0;
    virtual bool StartNamed(const std::string&, std::function<void(bool)>,
                            std::function<void(bool)>) {
        return false;
    }
    virtual bool Capture(const int16_t* samples, size_t count, bool speech) = 0;
    virtual void Cancel() = 0;
    virtual const char* Status() const { return "unavailable"; }
    virtual const char* Failure() const { return ""; }
    virtual bool SelectionPending() const { return false; }
    virtual bool StartSelection(const std::string&, std::function<void(bool)>) { return false; }
};
