#ifndef AUDIO_PLAYOUT_BUFFER_H_
#define AUDIO_PLAYOUT_BUFFER_H_

#include <algorithm>
#include <cstdint>

// Output-task-owned, allocation-free jitter admission. A bounded prefix absorbs
// bursty downlink delivery. Once running, the estimated I2S horizon prevents an
// empty software queue from inserting a new delay between every normal packet.
class AudioPlayoutBuffer {
public:
    void Enable(bool enabled) { enabled_ = enabled; }
    uint32_t WaitMs(uint32_t generation, uint32_t buffered_ms, int64_t now) {
        if (generation != generation_) {
            generation_ = generation;
            primed_ = waiting_ = false;
            horizon_ms_ = 0;
        }
        if (!enabled_ || !buffered_ms)
            return 0;
        if (!waiting_ && (!primed_ || now > horizon_ms_ + 20)) {
            if (primed_)
                ++rebuffer_count_;
            waiting_ = true;
            since_ms_ = now;
        }
        if (!waiting_)
            return 0;
        const auto elapsed = std::max<int64_t>(0, now - since_ms_);
        if (buffered_ms >= 120 || elapsed >= 80) {
            waiting_ = false;
            primed_ = true;
            return 0;
        }
        return 80 - elapsed;
    }
    void Written(uint32_t duration_ms, int64_t started_ms) {
        horizon_ms_ = std::max(horizon_ms_, started_ms) + duration_ms;
    }
    uint32_t rebuffer_count() const { return rebuffer_count_; }

private:
    bool enabled_ = false, primed_ = false, waiting_ = false;
    uint32_t generation_ = 0, rebuffer_count_ = 0;
    int64_t horizon_ms_ = 0, since_ms_ = 0;
};
#endif
