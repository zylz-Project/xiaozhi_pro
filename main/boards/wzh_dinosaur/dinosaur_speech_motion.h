#ifndef WZH_DINOSAUR_SPEECH_MOTION_H_
#define WZH_DINOSAUR_SPEECH_MOTION_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "wzh_speech_intent.h"

namespace wzh_dino {
class SpeechMotion {
public:
    std::array<float, 5> Update(uint32_t now_ms, float level, float attack,
                                wzh::SpeechIntent intent) {
        const float signal = std::isfinite(level) ? std::max(0.f, level - .018f) : 0.f;
        // Give quiet speech usable travel without flattening medium and strong speech.
        const float energy = std::sqrt(signal / (signal + .075f));
        if (!active_ && energy == 0.f)
            return {};

        const uint32_t elapsed_ms = active_ ? now_ms - last_ms_ : 20;
        const float dt = std::min<uint32_t>(elapsed_ms, 40) / 1000.f;
        last_ms_ = now_ms;
        active_ = true;
        if (elapsed_ms > 200) {
            // Avoid catching up missed servo updates in one command.
            velocity_.fill(0.f);
            accent_ = 0.f;
        }
        accent_ *= std::exp(-14.f * dt);
        if (!std::isfinite(attack) || attack < .08f || energy < .15f) {
            onset_latched_ = false;
        } else if (!onset_latched_ && attack >= .14f) {
            onset_latched_ = true;
            if (!accented_ || now_ms - last_accent_ms_ >= 140) {
                accent_ = std::clamp(attack * 1.75f + .12f, 0.f, 1.f);
                last_accent_ms_ = now_ms;
                accented_ = true;
            }
        }

        const auto index = static_cast<unsigned>(intent);
        const auto& shape = kShapes[index < kShapes.size() ? index : 0];
        const float omega = energy > 0.f ? 36.f : 18.f;
        const float friction = std::exp(-omega * dt);
        const float travel_limit = 125.f * dt;
        bool resting = energy == 0.f;
        for (unsigned axis = 0; axis < offsets_.size(); ++axis) {
            const float target =
                std::clamp(energy * (shape.rest[axis] + shape.accent[axis] * accent_),
                           -kMaximum[axis], kMaximum[axis]);
            const float displacement = offsets_[axis] - target;
            const float response = velocity_[axis] + omega * displacement;
            const float next = target + (displacement + response * dt) * friction;
            offsets_[axis] += std::clamp(next - offsets_[axis], -travel_limit, travel_limit);
            offsets_[axis] = std::clamp(offsets_[axis], -kMaximum[axis], kMaximum[axis]);
            velocity_[axis] =
                std::clamp((velocity_[axis] - omega * response * dt) * friction, -125.f, 125.f);
            resting &= std::abs(offsets_[axis]) < .08f && std::abs(velocity_[axis]) < .7f;
        }
        if (resting)
            Reset();
        return offsets_;
    }

    bool active() const { return active_; }
    void Reset() { *this = SpeechMotion{}; }

private:
    using Pose = std::array<float, 5>;
    struct Shape {
        Pose rest;
        Pose accent;
    };

    // Neck tilt, neck lean, head yaw, tail height and tail yaw in logical degrees.
    // Every accent follows the PCM onset and eases into the sentence posture.
    inline static constexpr std::array<Shape, 7> kShapes{{
        {{-4.5f, 0, 0, 4.5f, 0}, {-3, 1, 1, 3, -1}},  // Neutral
        {{-6, 4, 3, 6, -4}, {-4, 2, 3, 5, -4}},       // Greeting
        {{-7, 5, 6, 9, -8}, {-4, 2, 3, 6, -5}},       // Joy
        {{5, 2, 1, -3, -1}, {2, 1, 1, -2, 1}},        // Comfort
        {{-10, 5, 7, 2, -2}, {-4, 3, 3, 2, -1}},      // Question
        {{-4, -3, -6, 4, 5}, {-3, -2, -2, 4, 3}},     // Story
        {{5, 0, 1, -4, 1}, {1, .5f, 0, -1, 0}},       // Sleep
    }};
    inline static constexpr Pose kMaximum{15, 9, 12, 14, 13};

    Pose offsets_{}, velocity_{};
    uint32_t last_ms_ = 0, last_accent_ms_ = 0;
    float accent_ = 0;
    bool active_ = false, accented_ = false, onset_latched_ = false;
};
}  // namespace wzh_dino
#endif
