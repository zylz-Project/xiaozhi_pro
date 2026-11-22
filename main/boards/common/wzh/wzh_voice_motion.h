#ifndef WZH_VOICE_MOTION_H_
#define WZH_VOICE_MOTION_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "wzh_motion_profile.h"
#include "wzh_speech_intent.h"

namespace wzh {
enum class VoiceMechanism : uint8_t { kCrawl, kTail, kDinosaur };

// Finite conversational phrases: look, gesture, answer, release. A shuffled
// phrase changes the spatial relationship, rather than restarting one sine wave.
// All state belongs to the motion task; PCM callbacks only publish the envelope.
class VoiceMotion {
public:
    static constexpr unsigned kPhraseCount = 16;
    unsigned phrase() const { return style_; }
    std::array<float, 5> Update(VoiceMechanism mechanism, bool playing, float level, float attack,
                                uint32_t now, SpeechIntent intent = SpeechIntent::kNeutral) {
        if (!playing) {
            active_ = false;
            phrase_started_ = false;
            level_ = 0;
            accent_ = 0;
            gaze_energy_ = 0;
            for (auto& joint : joints_)
                joint = {};
            return {};
        }
        const bool delayed = active_ && now - last_ms_ > 200;
        if (delayed) {
            // Resume at the previous offset with one normal tick of travel;
            // pause the phrase clock rather than skipping ahead in the gesture.
            if (phrase_started_)
                phrase_ms_ += now - last_ms_ - 20;
            for (auto& joint : joints_)
                joint.velocity = 0;
        }
        const float dt =
            active_ && !delayed ? std::min<uint32_t>(now - last_ms_, 50) / 1000.f : .02f;
        active_ = true;
        last_ms_ = now;
        level = std::clamp(level, 0.f, 1.f);
        // A noise floor and slower release let short syllable gaps flow into
        // the same gesture. In longer pauses the envelope settles to rest.
        const float input = std::max(0.f, level - .018f);
        if (input > 0) {
            if (!phrase_started_ || now - last_voiced_ms_ >= 420 || now - phrase_ms_ >= period_)
                NextPhrase(now, intent);
            last_voiced_ms_ = now;
        }
        if (!phrase_started_)
            return {};
        level_ += (input - level_) * (1 - std::exp(-dt * (input > level_ ? 12.f : 8.f)));
        if (attack > .16f && level > .035f && now - accent_ms_ > 350) {
            accent_ms_ = now;
            accent_ = std::min(1.f, attack * 1.7f);
        }
        accent_ *= std::exp(-dt * 5.f);
        const float energy = std::sqrt(std::clamp((level_ - .001f) * 6.f, 0.f, 1.f));
        // Gaze follows the sentence, not its syllables. A separate slow lane
        // keeps the two panda yaw joints from looking like a faulty nod axis.
        gaze_energy_ +=
            (energy - gaze_energy_) * (1 - std::exp(-dt * (energy > gaze_energy_ ? 5.f : 8.f)));
        const float t =
            wzh::MotionPhraseBeat(std::min(.999f, float(now - phrase_ms_) / period_), style_);
        const unsigned beat = static_cast<unsigned>(t);
        const unsigned family = style_ % kPatternsPerSide;
        const bool dinosaur = mechanism == VoiceMechanism::kDinosaur;
        const auto& poses = dinosaur                             ? kDinosaur[family]
                            : mechanism == VoiceMechanism::kTail ? kTail[family]
                                                                 : kCrawl[family];
        Pose from = poses[beat ? beat - 1 : 3];
        Pose to = poses[beat];
        if (style_ >= kPatternsPerSide) {
            Mirror(mechanism, from);
            Mirror(mechanism, to);
        }
        const float head_blend = SettlingProgress((t - beat) / .62f);
        const float body_blend = SettlingProgress((t - beat) / .78f);
        const bool calm = intent_ == SpeechIntent::kComfort || intent_ == SpeechIntent::kSleep;
        const float expression = intent_ == SpeechIntent::kSleep   ? .38f
                                 : calm                            ? .72f
                                 : intent_ == SpeechIntent::kJoy   ? 1.10f
                                 : intent_ == SpeechIntent::kStory ? .92f
                                                                   : 1.f;
        std::array<float, 5> result{};
        const std::array<float, 5> low =
            mechanism == VoiceMechanism::kDinosaur ? std::array<float, 5>{-70, -90, -90, -35, -90}
            : mechanism == VoiceMechanism::kTail   ? std::array<float, 5>{-60, -84, -140, 0, 0}
                                                   : std::array<float, 5>{-60, -82, -82, 0, 0};
        const std::array<float, 5> high =
            mechanism == VoiceMechanism::kDinosaur ? std::array<float, 5>{110, 90, 90, 90, 90}
            : mechanism == VoiceMechanism::kTail   ? std::array<float, 5>{60, 84, 28, 0, 0}
                                                   : std::array<float, 5>{60, 82, 82, 0, 0};
        for (size_t i = 0; i < result.size(); ++i) {
            const bool head_axis = dinosaur ? i == 2 : i == 0;
            const float blend = head_axis || dinosaur ? head_blend : body_blend;
            const float gain = head_axis ? gaze_energy_ : energy * (1 + accent_ * .13f);
            float target = (from[i] + (to[i] - from[i]) * blend) * gain * expression;
            target = (head_axis || mechanism == VoiceMechanism::kTail)
                         ? SoftLimit(target, low[i], high[i])
                         : std::clamp(target, low[i], high[i]);
            const float omega = dinosaur                             ? 8.2f
                                : head_axis                          ? 6.f
                                : mechanism == VoiceMechanism::kTail ? 6.8f
                                                                     : 8.8f;
            const float speed = dinosaur                             ? 115.f
                                : head_axis                          ? 130.f
                                : mechanism == VoiceMechanism::kTail ? 155.f
                                                                     : 185.f;
            result[i] = joints_[i].Follow(target, dt, omega, speed);
        }
        return result;
    }

private:
    using Pose = std::array<float, 5>;
    static constexpr unsigned kPatternsPerSide = kPhraseCount / 2;

    // Offsets from each product's neutral pose: orient, respond, then ease off.
    // The fourth beat returns to neutral. All left/right partners mirror the
    // same path, leaving pitch and tail height alone.
    inline static constexpr Pose kDinosaur[kPatternsPerSide][4] = {
        // Neck pitch, neck lean, head yaw, tail height and tail yaw.
        // Set the direction on the first beat so one-word replies also move.
        {{-15, 32, 38, 4, -16}, {-32, 50, 53, 42, -43}, {30, 18, -18, -10, 4}, {}},
        {{53, 25, 38, -16, -26}, {78, 33, 57, -27, -41}, {46, -31, -45, 6, 14}, {}},
        {{19, 38, 55, 0, -26}, {38, 51, 68, -8, -30}, {8, -48, -59, 28, 46}, {}},
        {{45, 23, 26, -14, -12}, {68, -21, 12, -27, 14}, {24, -16, -9, -9, 8}, {}},
        {{-25, 39, 35, 24, -28}, {-51, 48, 57, 74, -49}, {24, -23, -37, 20, 33}, {}},
        {{29, -43, 51, 0, 0}, {51, -48, 57, 1, 42}, {2, 26, -32, 17, -30}, {}},
        {{52, 22, 50, -5, -14}, {15, 53, 58, 24, -38}, {57, -26, -28, -20, 23}, {}},
        {{25, 33, 32, 0, 0}, {42, 44, 48, -14, -9}, {5, 16, -21, 7, 10}, {}},
    };
    inline static constexpr Pose kTail[kPatternsPerSide][4] = {
        // Head yaw, tail yaw and tail pitch. Negative pitch lifts the tail.
        {{45, -20, -26}, {47, -48, -101}, {-20, 22, -45}, {}},
        {{37, 18, 12}, {39, 38, -25}, {16, -20, 13}, {}},
        {{74, 26, -56}, {72, -29, -90}, {-73, -56, -42}, {}},
        {{25, 7, 11}, {24, -28, 16}, {8, 13, 5}, {}},
        {{38, -29, -87}, {58, 21, -133}, {25, 57, -63}, {}},
        {{48, 0, 0}, {49, 39, -71}, {-21, -42, -53}, {}},
        {{57, 34, -9}, {56, 55, -44}, {20, -48, -25}, {}},
        {{30, 0, 0}, {31, -26, -15}, {9, -8, 10}, {}},
    };
    inline static constexpr Pose kCrawl[kPatternsPerSide][4] = {
        // Head yaw, left paw, right paw. The two lift directions are opposite.
        {{44, -24, 0}, {46, -82, 0}, {46, -48, 0}, {}},
        {{38, 0, 0}, {40, 0, 52}, {15, 0, 22}, {}},
        {{74, -36, 0}, {71, 0, 72}, {-72, -64, 0}, {}},
        {{24, 18, 0}, {25, -27, 0}, {8, 0, 0}, {}},
        {{40, -27, 26}, {62, -82, 82}, {20, -30, 0}, {}},
        {{49, 0, 0}, {50, -68, 0}, {-21, 0, 57}, {}},
        {{53, -40, 0}, {53, -69, 0}, {19, 16, 0}, {}},
        {{26, 0, 0}, {28, -36, 0}, {6, 0, 0}, {}},
    };

    static void Mirror(VoiceMechanism mechanism, Pose& pose) {
        if (mechanism == VoiceMechanism::kDinosaur) {
            pose[1] = -pose[1];
            pose[2] = -pose[2];
            pose[4] = -pose[4];
        } else {
            pose[0] = -pose[0];
            pose[1] = -pose[1];
            if (mechanism == VoiceMechanism::kCrawl) {
                pose[2] = -pose[2];
                std::swap(pose[1], pose[2]);
            }
        }
    }
    static float SoftLimit(float value, float low, float high) {
        const float limit = value < 0 ? -low : high;
        if (limit <= 0)
            return 0;
        const float magnitude = std::abs(value);
        const float knee = limit * .75f, room = limit - knee;
        return std::copysign(magnitude <= knee
                                 ? magnitude
                                 : knee + room * (1 - std::exp(-(magnitude - knee) / room)),
                             value);
    }
    struct Joint {
        float position = 0, velocity = 0;
        float Follow(float target, float dt, float omega, float speed) {
            const float delta = position - target;
            const float impulse = velocity + omega * delta;
            const float decay = std::exp(-omega * dt);
            const float next = target + (delta + impulse * dt) * decay;
            position += std::clamp(next - position, -speed * dt, speed * dt);
            velocity = std::clamp((velocity - omega * impulse * dt) * decay, -speed, speed);
            return position;
        }
    };
    void NextPhrase(uint32_t now, SpeechIntent intent) {
        intent_ = intent;
        random_ = random_ * 1664525U + 1013904223U + now;
        // Each speech intent draws without replacement from its own pool.
        static constexpr unsigned counts[] = {16, 8, 8, 6, 8, 10, 6};
        static constexpr unsigned pools[7][kPhraseCount] = {
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
            {0, 4, 5, 6, 8, 12, 13, 14},
            {0, 2, 4, 6, 8, 10, 12, 14},
            {1, 3, 7, 9, 11, 15},
            {1, 2, 5, 6, 9, 10, 13, 14},
            {0, 1, 2, 3, 5, 8, 9, 10, 11, 13},
            {3, 5, 7, 11, 13, 15},
        };
        const unsigned bucket = std::min<unsigned>(static_cast<unsigned>(intent), 6);
        const auto* pool = pools[bucket];
        const unsigned count = counts[bucket];
        const auto* previous = std::find(pool, pool + count, style_);
        const int avoid = previous == pool + count ? -1 : static_cast<int>(previous - pool);
        style_ = pool[styles_[bucket].Draw(count, random_, avoid)];
        period_ = 2200 + (random_ >> 8) % 801;
        if (intent == SpeechIntent::kComfort || intent == SpeechIntent::kSleep)
            period_ += 450;
        if (intent == SpeechIntent::kJoy)
            period_ -= 180;
        else if (intent == SpeechIntent::kQuestion || intent == SpeechIntent::kGreeting)
            period_ -= 100;
        phrase_ms_ = now;
        phrase_started_ = true;
    }
    SpeechIntent intent_ = SpeechIntent::kNeutral;
    std::array<MotionShuffleBag<kPhraseCount>, 7> styles_;
    std::array<Joint, 5> joints_{};
    bool active_ = false;
    bool phrase_started_ = false;
    uint32_t last_ms_ = 0, last_voiced_ms_ = 0, phrase_ms_ = 0, period_ = 2600;
    uint32_t random_ = 0x5a7e391U;
    uint32_t accent_ms_ = 0;
    float accent_ = 0;
    unsigned style_ = 0;
    float level_ = 0, gaze_energy_ = 0;
};
}  // namespace wzh
#endif
