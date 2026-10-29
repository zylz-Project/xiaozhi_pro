#ifndef WZH_TAIL_PHRASE_H_
#define WZH_TAIL_PHRASE_H_

#include <array>
#include "wzh_motion_profile.h"

namespace wzh {
// Symmetric acceleration at the ends retains equal travel/time on both sides.
inline float TailCycleProgress(float progress) {
    constexpr float edge = .12f, pi = 3.14159265359f;
    const float p = std::clamp(progress, 0.f, 1.f);
    auto ramp = [](float q) { return .5f * (q - edge / pi * std::sin(pi * q / edge)); };
    if (p < edge)
        return ramp(p) / (1 - edge);
    if (p > 1 - edge)
        return 1 - ramp(1 - p) / (1 - edge);
    return (p - edge / 2) / (1 - edge);
}

// Reference-inspired high sweep, circle, playful eight and alternating looks.
// Entry/return belong to the continuous player: no second amplitude window.
inline std::array<float, 3> TailPhrase(float progress, unsigned variant, float direction,
                                       uint32_t duration_ms = 6000) {
    constexpr float tau = 6.28318530718f;
    const float p = TailCycleProgress(progress);
    const float cycles = std::clamp(float(duration_ms / 3000), 1.f, 8.f);
    const float phase = tau * cycles * p;
    const float s = std::sin(phase), c = std::cos(phase);
    float lateral = s, lift;
    switch ((variant / 4) % 8) {
        case 0:
            lift = .85f + .15f * std::cos(2 * phase);
            break;
        case 1:
            lift = .52f + .48f * c;
            break;
        case 2:
            lift = .52f + .48f * std::cos(2 * phase);
            break;
        case 3:
            lift = .52f + .48f * std::sin(2 * phase);
            break;
        case 4:
            lift = .80f + .20f * c;
            break;
        case 5:
            lateral = 1.127f * s * (.88f + .12f * c);
            lift = .52f + .48f * c;
            break;
        case 6:
            lift = .86f + .14f * std::sin(2 * phase);
            break;
        default:
            lift = .52f - .48f * std::sin(2 * phase);
            break;
    }
    const float accent = variant % 4;
    const float head_role = variant % 4 < 2 ? 1.f : -1.f;
    // Complete looks to both sides, without the previous 0.72 attenuation.
    const float head = (56 + accent) * std::sin(tau * p);
    return {std::clamp(90 + direction * head_role * head, kPandaHeadMin, kPandaHeadMax),
            std::clamp(90 + direction * (82 + accent * (2.f / 3)) * lateral, 6.f, 174.f),
            std::clamp(150 - 140 * lift, 10.f, 178.f)};
}
}  // namespace wzh
#endif
