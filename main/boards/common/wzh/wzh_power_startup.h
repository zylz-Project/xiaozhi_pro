#ifndef WZH_POWER_STARTUP_H_
#define WZH_POWER_STARTUP_H_
#include <cstdint>

namespace wzh {
// Cold-start qualification only. Runtime double-click/shutdown keep their own
// release-armed state machine. Unknown samples never count as held time.
class PowerStartupGate {
public:
    enum class Result { kWaiting, kReady, kRejected };
    Result Update(uint32_t now, bool valid, bool pressed) {
        if (result_ != Result::kWaiting)
            return result_;
        if (!started_) {
            started_ = true;
            start_ = now;
        }
        if (!valid || (sampled_ && now - last_sample_ > 100)) {
            holding_ = releasing_ = false;
        } else if (pressed) {
            releasing_ = false;
            if (!holding_) {
                holding_ = true;
                held_since_ = now;
            }
            if (now - held_since_ >= 1000)
                result_ = Result::kReady;
        } else {
            holding_ = false;
            if (!releasing_) {
                releasing_ = true;
                release_since_ = now;
            }
            if (now - release_since_ >= 60)
                result_ = Result::kRejected;
        }
        sampled_ = true;
        last_sample_ = now;
        if (result_ == Result::kWaiting && now - start_ >= 3000)
            result_ = Result::kRejected;
        return result_;
    }

private:
    bool started_ = false, sampled_ = false, holding_ = false, releasing_ = false;
    uint32_t start_ = 0, last_sample_ = 0, held_since_ = 0, release_since_ = 0;
    Result result_ = Result::kWaiting;
};
}  // namespace wzh
#endif
