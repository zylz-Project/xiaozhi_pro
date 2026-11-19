#ifndef AUDIO_PLAYOUT_BUFFER_H_
#define AUDIO_PLAYOUT_BUFFER_H_

#include <algorithm>
#include <cstdint>

// Output-task-owned, allocation-free jitter admission. A bounded prefix absorbs
// bursty downlink delivery. Once running, the estimated I2S horizon prevents an
// empty software queue from inserting a new delay between every normal packet.
//
// The prefix depth and the give-up window are configurable because a cellular
// downlink delivers in bursts hundreds of milliseconds apart: a deeper prefix
// costs latency but stops every burst from being heard as a gap, and a longer
// give-up window keeps an already-running stream from restarting on a hiccup.
class AudioPlayoutBuffer {
public:
    void Enable(bool enabled) { enabled_ = enabled; }
    // target_ms is the prefix depth that releases playback; max_wait_ms bounds
    // how long the output task stalls before it plays whatever it has.
    void Configure(uint32_t target_ms, uint32_t max_wait_ms) {
        target_ms_ = target_ms ? target_ms : 1;
        max_wait_ms_ = max_wait_ms ? max_wait_ms : 1;
    }
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
        if (buffered_ms >= target_ms_ || elapsed >= max_wait_ms_) {
            waiting_ = false;
            primed_ = true;
            return 0;
        }
        return max_wait_ms_ - static_cast<uint32_t>(elapsed);
    }
    void Written(uint32_t duration_ms, int64_t started_ms) {
        horizon_ms_ = std::max(horizon_ms_, started_ms) + duration_ms;
    }
    uint32_t rebuffer_count() const { return rebuffer_count_; }
    uint32_t target_ms() const { return target_ms_; }

private:
    bool enabled_ = false, primed_ = false, waiting_ = false;
    uint32_t generation_ = 0, rebuffer_count_ = 0;
    // Defaults reproduce the original 120 ms prefix with an 80 ms cap.
    uint32_t target_ms_ = 120, max_wait_ms_ = 80;
    int64_t horizon_ms_ = 0, since_ms_ = 0;
};
#endif
