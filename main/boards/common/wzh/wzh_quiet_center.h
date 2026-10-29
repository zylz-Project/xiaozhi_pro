#ifndef WZH_QUIET_CENTER_H_
#define WZH_QUIET_CENTER_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "wzh_motion_profile.h"

namespace wzh {
// Motion-task owned. Return from the last commanded physical pose, including
// voice offsets, rather than an interrupted clip's stale logical waypoint.
template <size_t Axes>
class QuietCenter {
public:
    void Reset() { active_ = false; }
    bool active() const { return active_; }
    std::array<float, Axes> Sample(const std::array<float, Axes>& actual,
                                   const std::array<float, Axes>& rest, uint32_t now,
                                   float peak_speed = 100.f, uint32_t hold_ms = 0) {
        if (!active_) {
            active_ = true;
            origin_ = actual;
            since_ = now + hold_ms;
            float travel = 0;
            for (size_t i = 0; i < Axes; ++i)
                travel = std::max(travel, std::abs(rest[i] - actual[i]));
            duration_ = SettlingDuration(travel, peak_speed);
        }
        if (static_cast<int32_t>(now - since_) <= 0) {
            return origin_;
        }
        float t = std::min(1.f, float(now - since_) / duration_);
        t = SettlingProgress(t);
        std::array<float, Axes> out{};
        for (size_t i = 0; i < Axes; ++i)
            out[i] = origin_[i] + (rest[i] - origin_[i]) * t;
        return out;
    }
    bool powered(uint32_t now) const {
        return active_ &&
               static_cast<int32_t>(now - since_) < static_cast<int32_t>(duration_ + 450);
    }

private:
    std::array<float, Axes> origin_{};
    bool active_ = false;
    uint32_t since_ = 0, duration_ = 650;
};
}  // namespace wzh
#endif
