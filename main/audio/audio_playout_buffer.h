#ifndef AUDIO_PLAYOUT_BUFFER_H_
#define AUDIO_PLAYOUT_BUFFER_H_

#include <algorithm>
#include <cstdint>

// Output-task-owned, allocation-free jitter admission. A bounded prefix absorbs
// bursty downlink delivery. Once running, the estimated I2S horizon prevents an
// empty software queue from inserting a new delay between every normal packet.
//
// A bursty cellular link can use a deeper prefix after an underrun. The same
// wait cap applies to the first prefix and to each recovery.
class AudioPlayoutBuffer {
public:
    void Enable(bool enabled) { enabled_ = enabled; }
    void Configure(uint32_t target_ms, uint32_t max_wait_ms, uint32_t recovery_target_ms = 0) {
        target_ms_ = target_ms ? target_ms : 1;
        max_wait_ms_ = max_wait_ms ? max_wait_ms : 1;
        recovery_target_ms_ = recovery_target_ms ? recovery_target_ms : target_ms_;
    }
    template <typename BufferedPrefix>
    uint32_t WaitMs(uint32_t generation, int64_t now, BufferedPrefix&& buffered_prefix) {
        if (generation != generation_) {
            generation_ = generation;
            primed_ = waiting_ = false;
            horizon_ms_ = 0;
        }
        if (!enabled_ || (!waiting_ && primed_ && now <= horizon_ms_ + 20))
            return 0;

        const auto target_ms = primed_ ? recovery_target_ms_ : target_ms_;
        const auto buffered_ms = buffered_prefix(target_ms);
        if (!buffered_ms)
            return 0;
        if (!waiting_) {
            if (primed_)
                ++rebuffer_count_;
            waiting_ = true;
            since_ms_ = now;
        }
        const auto elapsed = std::max<int64_t>(0, now - since_ms_);
        if (buffered_ms >= target_ms || elapsed >= max_wait_ms_) {
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

private:
    bool enabled_ = false, primed_ = false, waiting_ = false;
    uint32_t generation_ = 0, rebuffer_count_ = 0;
    // Defaults reproduce the original 120 ms prefix with an 80 ms cap.
    uint32_t target_ms_ = 120, max_wait_ms_ = 80, recovery_target_ms_ = 120;
    int64_t horizon_ms_ = 0, since_ms_ = 0;
};
#endif
