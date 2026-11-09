#ifndef WZH_STARTUP_GREETING_H_
#define WZH_STARTUP_GREETING_H_

#include <cstdint>

namespace wzh {
// Startup is a one-shot, bounded, low-priority request. Battery/servo safety,
// audio readiness and user interactions remain owned by the caller.
class StartupGreetingGate {
public:
    explicit StartupGreetingGate(uint32_t start_ms) : start_ms_(start_ms) {}

    bool Due(uint32_t now_ms, bool ready, bool interrupted) {
        if (!pending_)
            return false;
        const uint32_t elapsed = now_ms - start_ms_;
        if (interrupted || elapsed >= 30000) {
            pending_ = false;
            return false;
        }
        return elapsed >= 2000 && ready;
    }
    void Accepted() { pending_ = false; }

private:
    uint32_t start_ms_;
    bool pending_ = true;
};
}  // namespace wzh
#endif
