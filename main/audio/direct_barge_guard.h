#pragma once
#include <cstdint>

// Main-task-owned gate. Do not trigger on the previous user's VAD tail when
// entering Speaking: require silence first, then sustained detected speech.
class DirectBargeGuard {
public:
    void Reset() {
        armed_ = false;
        speech_since_us_ = 0;
    }
    bool Update(bool speaking, bool voice, int64_t now_us) {
        if (!speaking) {
            Reset();
            return false;
        }
        if (!voice) {
            armed_ = true;
            speech_since_us_ = 0;
            return false;
        }
        if (!armed_)
            return false;
        if (!speech_since_us_) {
            speech_since_us_ = now_us;
            return false;
        }
        if (now_us - speech_since_us_ < 700000)
            return false;
        Reset();
        return true;
    }

private:
    bool armed_ = false;
    int64_t speech_since_us_ = 0;
};
