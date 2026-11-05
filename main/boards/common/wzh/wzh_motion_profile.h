#ifndef WZH_MOTION_PROFILE_H_
#define WZH_MOTION_PROFILE_H_

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wzh {
// Command trajectory limit, not a claim about the loaded servo's real speed.
// test_crawl's 800 ms / 82 degree sinusoid peaks at 322 deg/s; its 400 ms
// quintic entry peaks at 384 deg/s. Neither may be flattened by this limiter.
constexpr float kPetServoSpeed = 400.0f;

constexpr float kPandaHeadMin = 30.f;
constexpr float kPandaHeadMax = 150.f;

// Quintic Hermite with zero endpoint acceleration. Tangents are normalized by
// segment duration. Ordered Bezier controls keep the complete path inside its
// landmarks; de Casteljau avoids cancellation near either end of the segment.
inline float MotionQuintic(float from, float to, float outgoing, float incoming, float t) {
    if (t <= 0)
        return from;
    if (t >= 1)
        return to;
    float points[] = {from,
                      from + outgoing / 5,
                      from + 2 * outgoing / 5,
                      to - 2 * incoming / 5,
                      to - incoming / 5,
                      to};
    for (unsigned count = 5; count > 0; --count)
        for (unsigned i = 0; i < count; ++i)
            points[i] += (points[i + 1] - points[i]) * t;
    return points[0];
}

// Tail-panda coordinate remap. The authored library still uses the old
// 150-degree sitting center; boards whose horn was re-indexed to 90 degrees
// enable this once at startup. The map preserves the full physical range:
// 10 -> 3, 150 -> 90, 178 -> 177.
inline bool g_tail_ud_remap = false;
inline float g_tail_servo_ud = 90.f;
inline float TailServoUd(float library_ud) {
    if (!g_tail_ud_remap)
        return library_ud;
    // The former piecewise linear map changed its slope by 5x at sitting.
    // Match velocity and zero acceleration through that landmark as well.
    constexpr float shared_slope = 1.2f * 87.f / 140.f;
    const float servo =
        library_ud <= 150.f
            ? MotionQuintic(3, 90, 87, shared_slope * 140, (library_ud - 10) / 140)
            : MotionQuintic(90, 177, shared_slope * 28, 87, (library_ud - 150) / 28);
    return std::clamp(servo, 3.f, 177.f);
}
// Yaw only. Preserve small attentive looks, then ease toward +/-60 degrees
// instead of driving the panda head against its former full-travel stops.
inline float PandaHeadTarget(float angle) {
    const float delta = angle - 90.f;
    const float magnitude = std::abs(delta);
    const float travel =
        magnitude <= 42.f ? magnitude : 42.f + 18.f * (1 - std::exp(-(magnitude - 42.f) / 18.f));
    return std::clamp(90.f + std::copysign(travel, delta), kPandaHeadMin, kPandaHeadMax);
}

inline float SettlingProgress(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * t * (10 + t * (-15 + 6 * t));
}

// Quintic easing peaks at 1.875 times average speed. Return travel must
// never pass through the fast gesture-duration compressor.
inline uint32_t SettlingDuration(float travel, float peak_speed = 90.f) {
    return std::max<uint32_t>(900, std::ceil(travel * 1875.f / std::max(1.f, peak_speed)));
}

// The monotone quintic's normalized third derivative is bounded by 60*travel.
// Give short motions a time budget as well as a peak-speed budget. This is a
// command-segment bound, not a measured motor/load limit or a global planner.
inline uint32_t MotionJerkDuration(float travel) {
    return static_cast<uint32_t>(std::ceil(std::cbrt(std::max(0.f, travel) / 1000.f) * 1000));
}

// Broad authored poses need headroom between their waypoints. Expanding a
// normalized excursion preserves the rest pose and endpoints without flattening
// several different poses against the same mechanical stop.
inline float StoryAngle(float angle, float center, float low, float high, float gain = 1.38f) {
    angle = std::clamp(angle, low, high);
    const float delta = angle - center;
    const float span = delta < 0 ? center - low : high - center;
    if (span <= 0)
        return center;
    const float t = delta / span;
    // Below 1.5 this curve stays strictly monotone, retaining distinct large
    // poses instead of clipping several different gestures to the same stop.
    gain = std::clamp(gain, 1.0f, 1.45f);
    const float interior = 1 - t * t;
    // Broaden intermediate gestures without flattening different landmarks at
    // a hard stop. For gain<=1.45 this stays strictly monotone, with endpoints
    // and center unchanged; quiet/settling scaling is still applied separately.
    return std::clamp(center + span * (t + (gain - 1) * t * interior * (1 + .45f * interior)), low,
                      high);
}

// Resolve once per performance, or deterministically from the same seed when
// looking ahead. Adjacent Hermite segments must agree on shared beat durations.
inline uint32_t StoryFrameDuration(float travel, uint32_t requested, uint32_t seed, uint8_t beat) {
    uint32_t hash = seed ^ (0x9e3779b9U * (static_cast<uint32_t>(beat) + 1));
    hash = (hash ^ (hash >> 16)) * 0x7feb352dU;
    hash ^= hash >> 15;
    const float phrasing = .94f + (hash & 255U) * (.12f / 255);
    const uint32_t authored = static_cast<uint32_t>(std::lround(requested * phrasing));
    if (travel < .01f)
        return std::clamp<uint32_t>(authored, 120, 650);
    return std::max({uint32_t(220), static_cast<uint32_t>(authored * .78f),
                     MotionJerkDuration(travel),
                     static_cast<uint32_t>(std::ceil(travel * 2000 / kPetServoSpeed))});
}

template <unsigned Capacity>
class MotionShuffleBag {
public:
    int Draw(unsigned count, uint32_t random) {
        if (count == 0 || count > Capacity)
            return -1;
        if (count_ != count || position_ >= count_) {
            count_ = count;
            position_ = 0;
            for (unsigned i = 0; i < count; ++i)
                items_[i] = i;
            for (unsigned i = count - 1; i > 0; --i) {
                random = random * 1664525U + 1013904223U;
                std::swap(items_[i], items_[random % (i + 1)]);
            }
            if (count > 1 && items_[0] == previous_)
                std::swap(items_[0], items_[1]);
        }
        previous_ = items_[position_++];
        return previous_;
    }

private:
    uint8_t items_[Capacity] = {};
    unsigned count_ = 0, position_ = 0;
    int previous_ = -1;
};

inline float ActionAngle(float angle, float center, float low, float high, float gain) {
    const float delta = angle - center;
    // Keep rest and tiny settling offsets intact. Give deliberate gestures an
    // extra visible excursion without converting every intermediate pose to a
    // mechanical endpoint. This applies only to authored choreography.
    const float t = std::clamp((std::abs(delta) - 5.0f) / 15.0f, 0.0f, 1.0f);
    const float accent = 10.0f * t * t * (3.0f - 2.0f * t);
    const float action_gain = 1.0f + (gain - 1.0f) * t;
    return std::clamp(center + delta * action_gain + std::copysign(accent, delta), low, high);
}

inline uint32_t MotionFrameDuration(float travel, uint32_t requested) {
    if (travel < 0.01f) {
        return std::clamp<uint32_t>(static_cast<uint32_t>(requested * .18f), 40, 120);
    }
    // Budget two times average travel for the monotone quintic's peak. The final
    // output limiter also covers voice offsets and interrupted/new requests.
    return std::max({uint32_t(120), static_cast<uint32_t>(requested * .34f),
                     MotionJerkDuration(travel),
                     static_cast<uint32_t>(std::ceil(travel * 2000.0f / kPetServoSpeed))});
}

inline float MotionTangent(float before, float after, float before_ms, float after_ms) {
    if (before * after <= 0 || before_ms <= 0 || after_ms <= 0)
        return 0;
    const float left = before / before_ms, right = after / after_ms;
    const float w1 = 2 * after_ms + before_ms, w2 = after_ms + 2 * before_ms;
    const float harmonic = (w1 + w2) / (w1 / left + w2 / right);
    // Each segment's two normalized tangents sum to at most 2.4*travel.
    // This keeps the quintic Bezier controls ordered, including unequal times.
    return std::copysign(
        std::min(std::abs(harmonic), 1.2f * std::min(std::abs(left), std::abs(right))), left);
}

// Durations are part of the tangents: unequal adjacent frame times must still
// have the same velocity at their shared waypoint. Zero knot acceleration makes
// the whole authored path C2; reversals/holds stop without an acceleration step.
inline float MotionHermite(float previous, float from, float to, float next, uint32_t before_ms,
                           uint32_t duration_ms, uint32_t after_ms, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float delta = to - from;
    const float m1 = MotionTangent(from - previous, delta, before_ms, duration_ms) * duration_ms;
    const float m2 = MotionTangent(delta, next - to, duration_ms, after_ms) * duration_ms;
    return MotionQuintic(from, to, m1, m2, t);
}
}  // namespace wzh
#endif
