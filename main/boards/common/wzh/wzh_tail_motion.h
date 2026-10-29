#ifndef WZH_TAIL_MOTION_H_
#define WZH_TAIL_MOTION_H_

#include "wzh_sound_score.h"
#include "wzh_tail_phrase.h"

namespace wzh {
// One clock preserves all extrema; no sparse/random keyframe resampling.
class TailMotion {
public:
    using Pose = std::array<float, 5>;
    void Cancel() { active_ = false; }
    bool active() const { return active_; }
    uint32_t duration() const { return active_ ? body_ms_ + exit_ms_ : 0; }
    uint32_t body_duration() const { return active_ ? body_ms_ : 0; }
    void Begin(uint32_t duration_ms, const Pose& origin, SoundKind kind, unsigned variant,
               uint32_t seed) {
        active_ = true;
        origin_ = origin;
        kind_ = kind;
        variant_ = (variant + ((seed >> 8) % 8) * 4) % 32;
        direction_ = ((seed & 1U) ^ (variant & 1U)) ? 1.f : -1.f;
        centers_ = {90, 90, 150, 0, 0};
        radii_.fill(0);
        const auto first = Target(0, 3000);
        float travel = 0;
        for (unsigned a = 0; a < 3; ++a)
            travel = std::max(travel, std::abs(first[a] - origin_[a]));
        entry_ms_ = std::max<uint32_t>(750, std::ceil(travel * 1875 / 350));
        // Complete both sides at full travel, even for very short recordings.
        body_ms_ = std::max(duration_ms, entry_ms_ + 2550);
        const auto last = Target(1, body_ms_ - entry_ms_);
        exit_ms_ = SettlingDuration(std::abs(150 - last[2]), kind == SoundKind::kSleep ? 65 : 130);
    }
    void SetRecordedRange(const Pose& low, const Pose& high) {
        for (unsigned a = 0; a < 3; ++a) {
            centers_[a] = (low[a] + high[a]) / 2;
            radii_[a] = (high[a] - low[a]) / 2;
        }
    }
    Pose Sample(uint32_t time, const Pose* recorded = nullptr) const {
        const Pose rest{90, 90, 150, 0, 0};
        if (!active_ || time >= duration())
            return rest;
        const uint32_t middle = body_ms_ - entry_ms_;
        if (time < entry_ms_) {
            auto out = Target(0, middle);
            const float p = SettlingProgress(float(time) / entry_ms_);
            for (unsigned a = 0; a < 3; ++a)
                out[a] = origin_[a] + (out[a] - origin_[a]) * p;
            return out;
        }
        if (time >= body_ms_) {
            auto out = Target(1, middle);
            const float p = SettlingProgress(float(time - body_ms_) / exit_ms_);
            for (unsigned a = 0; a < 3; ++a)
                out[a] += (rest[a] - out[a]) * p;
            return out;
        }
        auto out = Target(float(time - entry_ms_) / middle, middle);
        if (recorded) {
            // Keep recorder details about their own center, preventing a
            // one-sided recording from dragging the whole performance left.
            const float window = SettlingProgress(float(time - entry_ms_) / 650) *
                                 SettlingProgress(float(body_ms_ - time) / 650);
            for (unsigned a = 0; a < 3; ++a)
                if (radii_[a] > 1)
                    out[a] += 3 * window *
                              std::clamp(((*recorded)[a] - centers_[a]) / radii_[a], -1.f, 1.f);
        }
        out[0] = std::clamp(out[0], kPandaHeadMin, kPandaHeadMax);
        out[1] = std::clamp(out[1], 6.f, 174.f);
        out[2] = std::clamp(out[2], 10.f, 178.f);
        return out;
    }

private:
    Pose Target(float progress, uint32_t middle) const {
        auto p = TailPhrase(progress, variant_, direction_, middle);
        if (kind_ == SoundKind::kSleep) {
            p[0] = 90 + (p[0] - 90) * .33f;
            p[1] = 90 + (p[1] - 90) * .33f;
            p[2] = 150 + (p[2] - 150) * .40f;
        } else if (kind_ == SoundKind::kChew) {
            p[0] = 90 + (p[0] - 90) * .90f;
            p[2] = 150 + (p[2] - 150) * .97f;
        }
        return {p[0], p[1], p[2], 0, 0};
    }
    bool active_ = false;
    Pose origin_{}, centers_{}, radii_{};
    SoundKind kind_ = SoundKind::kCall;
    unsigned variant_ = 0;
    float direction_ = 1;
    uint32_t entry_ms_ = 750, body_ms_ = 3300, exit_ms_ = 900;
};
}  // namespace wzh
#endif
