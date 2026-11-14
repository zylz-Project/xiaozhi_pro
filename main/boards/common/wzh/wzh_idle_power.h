#ifndef WZH_IDLE_POWER_H_
#define WZH_IDLE_POWER_H_

#include <cstdint>
#include <functional>

namespace wzh {

// Pure timing policy. The owner serializes calls and performs audio and power
// operations elsewhere; a token prevents a late prompt completion from
// re-entering quiet mode after a new interaction.
class IdlePower {
public:
    // 2026-11-12 product decision: 5 min says one goodbye-ish sentence, 10 min
    // powers off. 10/15 was too long; the exhibition cadence keeps the pet alive
    // in between, and conversation always counts as interaction.
    static constexpr uint32_t kMissMs = 5U * 60U * 1000U;
    static constexpr uint32_t kOffMs = 10U * 60U * 1000U;

    void Interaction(uint32_t now) {
        last_interaction_ = now;
        ++generation_;
        initialized_ = true;
        prompt_pending_ = false;
        quiet_ = false;
    }

    bool MissDue(uint32_t now) const {
        return initialized_ && !quiet_ && !prompt_pending_ && now - last_interaction_ >= kMissMs;
    }

    uint32_t BeginMiss() {
        prompt_pending_ = true;
        return generation_;
    }

    void FinishMiss(uint32_t token, bool completed) {
        if (!prompt_pending_ || token != generation_)
            return;
        prompt_pending_ = false;
        quiet_ = completed;
    }

    bool Quiet() const { return quiet_; }
    bool OffDue(uint32_t now) const { return initialized_ && now - last_interaction_ >= kOffMs; }

private:
    uint32_t last_interaction_ = 0;
    uint32_t generation_ = 0;
    bool initialized_ = false;
    bool prompt_pending_ = false;
    bool quiet_ = false;
};

// Any real interaction (touch, pickup, power key, wake word, presence) resets
// the timeline, and an active conversation keeps it pinned at "just now": a call
// that runs past the deadline must never be cut off by the countdown. Callers
// only bump an atomic; the timeline worker performs the audio and power work.
void NotifyIdleInteraction();
// True from the miss mark (5 min) until the next interaction: idle sound effects
// go silent while motion keeps its full amplitude.
bool IdleSoundSuppressed();
// `shutdown_request` runs on the idle worker after the shutdown prompt has been
// played; it must only signal the power task, never block.
void StartIdlePower(std::function<void()> shutdown_request);
void StopIdlePower();

}  // namespace wzh
#endif  // WZH_IDLE_POWER_H_