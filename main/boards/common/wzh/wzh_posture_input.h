#ifndef WZH_POSTURE_INPUT_H_
#define WZH_POSTURE_INPUT_H_
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "wzh_interaction.h"

namespace wzh {
// Handling estimates from acceleration, not height/contact/gyroscope measurements.
// Safety free-fall detection remains independent in Sensors.
class PostureInput {
public:
    PetInteraction Update(int x, int y, int z, bool powered, bool falling, uint32_t now) {
        if (valid_ && now - last_sample_ > kMaxSampleGapMs)
            Invalidate();
        last_sample_ = now;
        const float norm = std::sqrt(float(x * x + y * y + z * z));
        const bool gravity = norm >= 700 && norm <= 1300;
        delta_ = valid_ ? std::abs(x - px_) + std::abs(y - py_) + std::abs(z - pz_) : 0;
        // A single accelerometer cannot distinguish servo vibration from handling.
        // Keep sampling for independent fall protection, but never queue pet
        // gestures from powered motion or its mechanical settling tail.
        if (powered) {
            own_motion_ = true;
            own_motion_since_ = now;
            gentle_shake_ready_ = false;
            Recalibrate();
        }
        if (powered || (own_motion_ && now - own_motion_since_ < 600)) {
            px_ = x;
            py_ = y;
            pz_ = z;
            valid_ = true;
            pending_ = PetInteraction::kNone;
            return pending_;
        }
        own_motion_ = false;
        const bool gentle_shake = !falling && ConfirmGentleShake(x, y, z, now);
        if (!valid_) {
            px_ = x;
            py_ = y;
            pz_ = z;
            valid_ = true;
            return PetInteraction::kNone;
        }
        if (falling) {
            gentle_shake_ready_ = false;
            pending_ = PetInteraction::kNone;
            Recalibrate();
        }
        if (baseline_samples_ < kBaselineSamples) {
            // Establish a new resting orientation after boot, I2C recovery or
            // our own movement. A different settled pose is not a human flip.
            if (gravity && delta_ < 90 && !falling) {
                bx_ += x;
                by_ += y;
                bz_ += z;
                if (++baseline_samples_ == kBaselineSamples) {
                    bx_ /= kBaselineSamples;
                    by_ /= kBaselineSamples;
                    bz_ /= kBaselineSamples;
                    landing_armed_ = handled_;
                }
            } else {
                Recalibrate();
            }
            px_ = x;
            py_ = y;
            pz_ = z;
            return QueueCandidate(gentle_shake ? PetInteraction::kShaken : PetInteraction::kNone,
                                  now);
        }
        const float bn = std::sqrt(float(bx_ * bx_ + by_ * by_ + bz_ * bz_));
        const float alignment = gravity && bn > 0 ? (x * bx_ + y * by_ + z * bz_) / (norm * bn) : 1;
        const bool quiet = gravity && delta_ < 90;
        upright_samples_ = quiet && alignment > .94f ? std::min(upright_samples_ + 1, 6) : 0;
        if (upright_samples_ >= 6)
            tilted_ = false;
        tilt_samples_ = quiet && alignment < .85f ? std::min(tilt_samples_ + 1, 6) : 0;
        const bool flipped = tilt_samples_ >= 6 && !tilted_;
        if (flipped)
            tilted_ = true;
        const bool shaken = ConfirmShake(x - bx_, y - by_, z - bz_, now) || gentle_shake;
        // The fourth count records that this streak was already observed.
        move_samples_ =
            delta_ >= 300 || std::abs(norm - bn) > 180 ? std::min(move_samples_ + 1, 4) : 0;
        // A sustained acceleration offset can describe the first pickup, but
        // cannot prove a NEW movement toward placement after that response.
        handling_quiet_samples_ =
            handled_ && quiet ? std::min(handling_quiet_samples_ + 1, kBaselineSamples) : 0;
        if (handling_quiet_samples_ >= kBaselineSamples)
            landing_armed_ = true;
        landing_samples_ = landing_armed_ && delta_ >= 300 ? std::min(landing_samples_ + 1, 3) : 0;
        if (landing_samples_ >= 3)
            landing_motion_seen_ = true;
        still_samples_ =
            handled_ && landing_motion_seen_ && quiet ? std::min(still_samples_ + 1, 35) : 0;
        PetInteraction candidate = PetInteraction::kNone;
        if (shaken)
            candidate = PetInteraction::kShaken;
        else if (flipped)
            candidate = PetInteraction::kFlipped;
        else if (move_samples_ == 3 && !handled_)
            candidate = PetInteraction::kPickedUp;
        else if (still_samples_ >= 35 && now - last_accepted_ >= 1200) {
            candidate = PetInteraction::kPutDown;
            landing_motion_seen_ = false;
        }
        px_ = x;
        py_ = y;
        pz_ = z;
        return QueueCandidate(candidate, now);
    }
    void Accepted(uint32_t now) {
        if (pending_ == PetInteraction::kNone)
            return;
        handled_ = pending_ != PetInteraction::kPutDown;
        landing_motion_seen_ = landing_armed_ = false;
        accepted_ = true;
        last_accepted_ = now;
        shake_peak_ = move_samples_ = landing_samples_ = handling_quiet_samples_ = still_samples_ =
            0;
        for (auto& axis : gentle_shake_axes_) {
            axis.sample_sign = axis.swing_sign = 0;
            axis.reversals = 0;
        }
        pending_ = PetInteraction::kNone;
    }
    void Invalidate() {
        valid_ = false;
        handled_ = false;
        gentle_shake_ready_ = false;
        pending_ = PetInteraction::kNone;
        Recalibrate();
    }
    int delta() const { return delta_; }

private:
    struct ShakeAxis {
        float center = 0;
        uint32_t last_swing_ms = 0;
        int8_t sample_sign = 0;
        int8_t swing_sign = 0;
        uint8_t reversals = 0;

        bool Update(int sample, uint32_t now) {
            constexpr float kMinimumSwingMg = 135;
            constexpr uint32_t kMinSwingMs = 80, kMaxSwingMs = 460;
            const uint32_t elapsed = now - last_swing_ms;
            if (swing_sign && elapsed > kMaxSwingMs) {
                *this = {};
                center = sample;
                return false;
            }
            const float offset = sample - center;
            const int8_t sign = offset >= kMinimumSwingMg ? 1 : offset <= -kMinimumSwingMg ? -1 : 0;
            // Learn the resting position between gestures. Hold it during a
            // candidate so the return from an asymmetric bump stays neutral.
            if (!swing_sign && !sign)
                center += .02f * offset;
            const bool confirmed = sign != 0 && sign == sample_sign;
            sample_sign = sign;
            if (!confirmed)
                return false;

            if (!swing_sign) {
                swing_sign = sign;
                last_swing_ms = now;
                return false;
            }
            if (sign == swing_sign)
                return false;
            swing_sign = sign;
            last_swing_ms = now;
            if (elapsed < kMinSwingMs) {
                reversals = 0;
                return false;
            }
            if (++reversals < 2)
                return false;
            reversals = 0;
            return true;
        }
    };

    bool ConfirmGentleShake(int x, int y, int z, uint32_t now) {
        const std::array<int, 3> samples = {x, y, z};
        if (!gentle_shake_ready_) {
            gentle_shake_axes_ = {};
            for (size_t i = 0; i < samples.size(); ++i)
                gentle_shake_axes_[i].center = samples[i];
            gentle_shake_ready_ = true;
            return false;
        }
        // Two reversals on one axis distinguish a smooth hand shake from a
        // bump followed by its rebound. No per-sample velocity gate here.
        bool shaken = false;
        for (size_t i = 0; i < samples.size(); ++i)
            shaken |= gentle_shake_axes_[i].Update(samples[i], now);
        return shaken;
    }

    PetInteraction QueueCandidate(PetInteraction candidate, uint32_t now) {
        if (pending_ != PetInteraction::kNone && now - pending_since_ >= 800)
            pending_ = PetInteraction::kNone;
        if (candidate != PetInteraction::kNone && (!accepted_ || now - last_accepted_ >= 900)) {
            if (pending_ == PetInteraction::kNone || candidate == PetInteraction::kShaken) {
                pending_ = candidate;
                pending_since_ = now;
            }
        }
        return pending_;
    }

    bool ConfirmShake(int x, int y, int z, uint32_t now) {
        const int strength = std::abs(x) + std::abs(y) + std::abs(z);
        if (strength < 350 || delta_ < 90)
            return false;
        const bool expired = shake_peak_ == 0 || now - shake_since_ > 300;
        const bool reversed =
            !expired && x * shake_x_ + y * shake_y_ + z * shake_z_ < 0 &&
            std::abs(x - shake_x_) + std::abs(y - shake_y_) + std::abs(z - shake_z_) >= 1100;
        // Retain a recent peak across the low-acceleration part of a shake.
        // A single bump followed by a return to baseline has no opposing peak.
        if (expired || reversed || strength > shake_peak_) {
            shake_x_ = x;
            shake_y_ = y;
            shake_z_ = z;
            shake_peak_ = strength;
            shake_since_ = now;
        }
        return reversed;
    }
    void Recalibrate() {
        landing_motion_seen_ = landing_armed_ = tilted_ = false;
        baseline_samples_ = bx_ = by_ = bz_ = 0;
        shake_peak_ = move_samples_ = landing_samples_ = handling_quiet_samples_ = still_samples_ =
            tilt_samples_ = upright_samples_ = 0;
    }
    static constexpr int kBaselineSamples = 15;
    static constexpr uint32_t kMaxSampleGapMs = 120;
    bool valid_ = false, handled_ = false, accepted_ = false, tilted_ = false;
    bool own_motion_ = false, landing_motion_seen_ = false, landing_armed_ = false;
    bool gentle_shake_ready_ = false;
    std::array<ShakeAxis, 3> gentle_shake_axes_{};
    int px_ = 0, py_ = 0, pz_ = 0, bx_ = 0, by_ = 0, bz_ = 0, delta_ = 0;
    int baseline_samples_ = 0, move_samples_ = 0, landing_samples_ = 0, handling_quiet_samples_ = 0,
        tilt_samples_ = 0, upright_samples_ = 0, still_samples_ = 0;
    int shake_x_ = 0, shake_y_ = 0, shake_z_ = 0, shake_peak_ = 0;
    uint32_t last_accepted_ = 0, pending_since_ = 0, last_sample_ = 0, shake_since_ = 0;
    uint32_t own_motion_since_ = 0;
    PetInteraction pending_ = PetInteraction::kNone;
};
}  // namespace wzh
#endif
