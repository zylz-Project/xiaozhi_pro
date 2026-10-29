#ifndef WZH_CRAWLPANDA_MOTION_STYLE_H_
#define WZH_CRAWLPANDA_MOTION_STYLE_H_

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "wzh_voice_motion.h"

namespace wzh_crawl {

// Shift the interior of a beat, keeping progress AND its derivative unchanged
// at both ends. Adjacent Hermite segments therefore retain their shared speed.
inline float GestureProgress(float progress, unsigned axis, unsigned beat, bool mirror) {
    const float t = std::clamp(progress, 0.0f, 1.0f);
    if (mirror && axis != 0)
        axis = 3 - axis;
    const float skew = axis == 0 ? .10f : (axis == (beat % 2 + 1) ? -.04f : -.10f);
    const float tail = 1 - t;
    const float bump = 64 * t * t * t * tail * tail * tail;
    return t + skew * bump;
}

inline float CrawlGaze(uint32_t elapsed, uint32_t running_ms, uint32_t arm_period_ms) {
    if (elapsed >= running_ms)
        return 0;
    const float edge = std::min({1.0f, elapsed / 400.0f, (running_ms - elapsed) / 400.0f});
    const float envelope = edge * edge * (3 - 2 * edge);
    // One look left/right for every two arm cycles; no interruption of traction.
    return 60 * envelope * std::sin(3.14159265359f * elapsed / arm_period_ms);
}

// Instance-owned conversational gestures. Goals follow actual audio, while a
// critically damped response removes abrupt accent steps and paw catch-up.
class VoiceGesture {
public:
    struct Offset {
        float head = 0, left = 0, right = 0;
    };

    Offset Update(bool playing, float level, float attack, uint32_t now,
                  wzh::SpeechIntent intent = wzh::SpeechIntent::kNeutral) {
        const auto offset =
            motion_.Update(wzh::VoiceMechanism::kCrawl, playing, level, attack, now, intent);
        return {offset[0], offset[1], offset[2]};
    }

private:
    wzh::VoiceMotion motion_;
};

}  // namespace wzh_crawl
#endif
