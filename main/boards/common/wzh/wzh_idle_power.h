#ifndef WZH_IDLE_POWER_H_
#define WZH_IDLE_POWER_H_

#include <cstdint>

namespace wzh {

// Owned by the existing system prompt worker. A wrap-safe difference keeps
// the idle window independent of the uptime counter rolling over.
class IdlePower {
public:
    static constexpr uint32_t kReminderMs = 5 * 60 * 1000;
    static constexpr uint32_t kReminderRetryMs = 60 * 1000;
    static constexpr uint32_t kShutdownMs = 7 * 60 * 1000 + 30 * 1000;

    void Observe(uint32_t now, uint32_t activity, bool available) {
        if (!available_ || activity != activity_ || !available) {
            activity_ = activity;
            since_ms_ = now;
            reminded_ = false;
        }
        available_ = available;
    }
    bool ReminderDue(uint32_t now) const {
        const uint32_t elapsed = now - since_ms_;
        return available_ && !reminded_ && elapsed >= kReminderMs &&
               elapsed < kReminderMs + kReminderRetryMs;
    }
    void ReminderQueued() { reminded_ = true; }
    bool ShutdownDue(uint32_t now) const { return available_ && now - since_ms_ >= kShutdownMs; }

private:
    uint32_t activity_ = 0;
    uint32_t since_ms_ = 0;
    bool available_ = false;
    bool reminded_ = false;
};

}  // namespace wzh
#endif
