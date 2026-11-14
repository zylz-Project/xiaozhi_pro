#ifndef WZH_ONBOARDING_PROGRESS_H_
#define WZH_ONBOARDING_PROGRESS_H_

#include <cstdint>

namespace wzh {

// Playback ownership and persistence are handled by the caller. Completed
// steps are retained separately so a timeout or a skipped interrupted step can
// never accidentally mark an entire chapter complete.
struct OnboardingProgress {
    static constexpr uint8_t kOfflineComplete = 1;
    static constexpr uint8_t kOnlineComplete = 2;
    static constexpr uint8_t kOfflineEnd = 6;
    static constexpr uint8_t kStepCount = 13;
    static constexpr uint8_t kMaxAttempts = 3;
    static constexpr uint16_t kOfflineMask = (1U << kOfflineEnd) - 1;
    static constexpr uint16_t kOnlineMask = ((1U << kStepCount) - 1) & ~kOfflineMask;

    uint8_t flags = 0;
    uint8_t step = 0;
    uint8_t attempts = 0;
    uint16_t completed = 0;
    // Steps deferred after kMaxAttempts interruptions. They are retried on
    // the next boot and never counted as complete until playback succeeds.
    uint16_t skipped = 0;

    bool Done() const { return (flags & (kOfflineComplete | kOnlineComplete)) == 3; }

    // Called after a prompt has actually drained and any requested interaction
    // has either happened or timed out. A timeout advances but is not marked.
    void Finish(bool interaction_satisfied) {
        if (step >= kStepCount)
            return;
        if (interaction_satisfied) {
            completed |= static_cast<uint16_t>(1U << step);
            skipped &= static_cast<uint16_t>(~(1U << step));
        }
        attempts = 0;
        ++step;
        // On resume, do not replay steps that already completed in an earlier
        // session merely because an earlier interaction timed out.
        while (step < kStepCount && (completed & (1U << step)) != 0)
            ++step;
        if (step >= kOfflineEnd && (completed & kOfflineMask) == kOfflineMask)
            flags |= kOfflineComplete;
        if (step == kStepCount && (completed & kOnlineMask) == kOnlineMask)
            flags |= kOnlineComplete;
    }

    void Interrupted() {
        if (step >= kStepCount)
            return;
        if (++attempts < kMaxAttempts)
            return;
        // Avoid pinning this session on a repeatedly interrupted step. It is
        // recorded as skipped and retried on the next boot.
        skipped |= static_cast<uint16_t>(1U << step);
        attempts = 0;
        ++step;
    }

    // Resume incomplete earlier steps after a restart. Steps that finished, or
    // are not replayed. Interrupted or previously skipped steps are retried
    // so the tutorial can eventually become complete.
    void Resume() {
        if (Done()) {
            step = kStepCount;
            return;
        }
        if (step > kStepCount)
            step = kStepCount;
        for (uint8_t i = 0; i < step; ++i) {
            const bool chapter_done =
                i < kOfflineEnd ? (flags & kOfflineComplete) : (flags & kOnlineComplete);
            const auto bit = static_cast<uint16_t>(1U << i);
            if (!chapter_done && (completed & bit) == 0) {
                step = i;
                attempts = 0;
                break;
            }
        }
    }

    static OnboardingProgress MigratedComplete() {
        OnboardingProgress progress;
        progress.flags = kOfflineComplete | kOnlineComplete;
        progress.step = kStepCount;
        progress.completed = kOfflineMask | kOnlineMask;
        return progress;
    }
};

}  // namespace wzh
#endif  // WZH_ONBOARDING_PROGRESS_H_
