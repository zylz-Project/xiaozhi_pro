#ifndef WZH_POSTURE_INPUT_H_
#define WZH_POSTURE_INPUT_H_
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "wzh_interaction.h"

namespace wzh {
// Handling estimates from acceleration, not height/contact/gyroscope measurements.
// Safety free-fall detection remains independent in Sensors.
class PostureInput {
public:
    PetInteraction Update(int x, int y, int z, bool powered, bool falling, uint32_t now) {
        const float norm = std::sqrt(float(x * x + y * y + z * z));
        const bool gravity = norm >= 700 && norm <= 1300;
        delta_ = valid_ ? std::abs(x - px_) + std::abs(y - py_) + std::abs(z - pz_) : 0;
        // A single accelerometer cannot distinguish servo vibration from handling.
        // Keep sampling for independent fall protection, but never queue pet
        // gestures from powered motion or its mechanical settling tail.
        if (powered) {
            own_motion_ = true;
            own_motion_since_ = now;
            Recalibrate();
        }
        if (powered || (own_motion_ && now - own_motion_since_ < 600)) {
            px_ = x;
            py_ = y;
            pz_ = z;
            valid_ = true;
            pending_ = PetInteraction::kNone;
            shake_samples_ = move_samples_ = landing_samples_ = still_samples_ = tilt_samples_ = 0;
            return pending_;
        }
        own_motion_ = false;
        if (!valid_) {
            px_ = x;
            py_ = y;
            pz_ = z;
            valid_ = true;
            return PetInteraction::kNone;
        }
        if (falling) {
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
            return PetInteraction::kNone;
        }
        const float bn = std::sqrt(float(bx_ * bx_ + by_ * by_ + bz_ * bz_));
        const float alignment = gravity && bn > 0 ? (x * bx_ + y * by_ + z * bz_) / (norm * bn) : 1;
        if (alignment > .94f)
            tilted_ = false;
        tilt_samples_ = alignment < .85f ? std::min(tilt_samples_ + 1, 6) : 0;
        shake_samples_ = delta_ >= 1100 ? std::min(shake_samples_ + 1, 2) : 0;
        move_samples_ =
            delta_ >= 300 || std::abs(norm - bn) > 180 ? std::min(move_samples_ + 1, 3) : 0;
        // A sustained acceleration offset can describe the first pickup, but
        // cannot prove a NEW movement toward placement after that response.
        handling_quiet_samples_ = handled_ && gravity && delta_ < 90
                                      ? std::min(handling_quiet_samples_ + 1, kBaselineSamples)
                                      : 0;
        if (handling_quiet_samples_ >= kBaselineSamples)
            landing_armed_ = true;
        landing_samples_ = landing_armed_ && delta_ >= 300 ? std::min(landing_samples_ + 1, 3) : 0;
        if (landing_samples_ >= 3)
            landing_motion_seen_ = true;
        still_samples_ = handled_ && landing_motion_seen_ && gravity && delta_ < 90
                             ? std::min(still_samples_ + 1, 35)
                             : 0;
        PetInteraction candidate = PetInteraction::kNone;
        if (!falling) {
            if (shake_samples_ >= 2)
                candidate = PetInteraction::kShaken;
            else if (tilt_samples_ >= 6 && !tilted_)
                candidate = PetInteraction::kFlipped;
            else if (move_samples_ >= 3 && !handled_)
                candidate = PetInteraction::kPickedUp;
            else if (still_samples_ >= 35 && now - last_accepted_ >= 1200)
                candidate = PetInteraction::kPutDown;
        }
        px_ = x;
        py_ = y;
        pz_ = z;
        if (falling) {
            pending_ = PetInteraction::kNone;
            return pending_;
        }
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
    void Accepted(uint32_t now) {
        if (pending_ == PetInteraction::kNone)
            return;
        handled_ = pending_ != PetInteraction::kPutDown;
        landing_motion_seen_ = landing_armed_ = false;
        if (pending_ == PetInteraction::kFlipped)
            tilted_ = true;
        accepted_ = true;
        last_accepted_ = now;
        shake_samples_ = move_samples_ = landing_samples_ = handling_quiet_samples_ =
            still_samples_ = 0;
        pending_ = PetInteraction::kNone;
    }
    void Invalidate() {
        valid_ = false;
        handled_ = false;
        Recalibrate();
    }
    int delta() const { return delta_; }

private:
    void Recalibrate() {
        pending_ = PetInteraction::kNone;
        landing_motion_seen_ = landing_armed_ = tilted_ = false;
        baseline_samples_ = bx_ = by_ = bz_ = 0;
        shake_samples_ = move_samples_ = landing_samples_ = handling_quiet_samples_ =
            still_samples_ = tilt_samples_ = 0;
    }
    static constexpr int kBaselineSamples = 15;
    bool valid_ = false, handled_ = false, accepted_ = false, tilted_ = false;
    bool own_motion_ = false, landing_motion_seen_ = false, landing_armed_ = false;
    int px_ = 0, py_ = 0, pz_ = 0, bx_ = 0, by_ = 0, bz_ = 0, delta_ = 0;
    int baseline_samples_ = 0, shake_samples_ = 0, move_samples_ = 0, landing_samples_ = 0,
        handling_quiet_samples_ = 0, tilt_samples_ = 0, still_samples_ = 0;
    uint32_t last_accepted_ = 0, pending_since_ = 0;
    uint32_t own_motion_since_ = 0;
    PetInteraction pending_ = PetInteraction::kNone;
};
}  // namespace wzh
#endif
