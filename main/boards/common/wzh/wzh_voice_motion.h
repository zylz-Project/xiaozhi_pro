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
        if (!playing || (active_ && now - last_ms_ > 200)) {
            active_ = false;
            level_ = 0;
            accent_ = 0;
            gaze_energy_ = 0;
            for (auto& joint : joints_)
                joint = {};
            if (!playing)
                return {};
        }
        const float dt = active_ ? std::min<uint32_t>(now - last_ms_, 50) / 1000.f : .02f;
        if (!active_) {
            active_ = true;
            NextPhrase(now, intent);
        }
        last_ms_ = now;
        if (intent != intent_ || now - phrase_ms_ >= period_)
            NextPhrase(now, intent);
        level = std::clamp(level, 0.f, 1.f);
        level_ += (level - level_) * (1 - std::exp(-dt * (level > level_ ? 15 : 9)));
        if (level > .018f)
            last_voice_ms_ = now;
        // Silence is a punctuation mark: yield during a pause rather than
        // continuing a wall-clock performance through the whole sentence.
        const float quiet = now - last_voice_ms_ > 220 ? 0.f : 1.f;
        if (attack > .16f && level > .02f && now - accent_ms_ > 280) {
            accent_ms_ = now;
            accent_ = std::min(1.f, attack * 1.7f);
        }
        accent_ *= std::exp(-dt * 6.5f);
        // Keep quiet speech visible. The previous sqrt(level*2.6) made normal
        // TTS output settle around half excursion; this maps conversational
        // levels much closer to the authored gesture range.
        const float energy = quiet * std::sqrt(std::clamp(level_ * 3.8f, 0.f, 1.f));
        // Gaze follows the sentence, not its syllables. A separate slow lane
        // keeps the two panda yaw joints from looking like a faulty nod axis.
        gaze_energy_ +=
            (energy - gaze_energy_) * (1 - std::exp(-dt * (energy > gaze_energy_ ? 3.6f : 6.0f)));
        const float t = std::min(.999f, float(now - phrase_ms_) / period_) * 4;
        const unsigned beat = static_cast<unsigned>(t);
        const auto& beat_intent = kPhrases[style_][beat];
        const auto& before = kPhrases[style_][beat ? beat - 1 : 3];
        const float phase = std::clamp((t - beat) / .65f, 0.f, 1.f);
        const float blend = phase * phase * (3 - 2 * phase);
        const float phrase = std::min(1.f, float(now - phrase_ms_) / period_);
        const float first_look = kPhrases[style_][1][0];
        const float check = kPhrases[style_][2][0];
        const float gaze =
            side_ * (phrase < .24f   ? first_look * SettlingProgress(phrase / .24f)
                     : phrase < .54f ? first_look
                     : phrase < .80f ? first_look + (check - first_look) *
                                                        SettlingProgress((phrase - .54f) / .26f)
                                     : check * (1 - SettlingProgress((phrase - .80f) / .20f)));
        const float gesture = (before[1] + (beat_intent[1] - before[1]) * blend) * side_;
        const float lift = before[2] + (beat_intent[2] - before[2]) * blend;
        const float emphasis = accent_ * .12f;
        const bool calm = intent == SpeechIntent::kComfort || intent == SpeechIntent::kSleep;
        const float expression = calm                                 ? .78f
                                 : intent == SpeechIntent::kJoy       ? 1.20f
                                 : intent == SpeechIntent::kQuestion  ? 1.10f
                                 : intent == SpeechIntent::kGreeting  ? 1.06f
                                 : intent == SpeechIntent::kStory     ? .98f
                                                                      : 1.04f;
        std::array<float, 5> goal{};
        if (mechanism == VoiceMechanism::kCrawl) {
            // A resting paw stays at rest. Opposite servo directions must not
            // turn every "expressive" offset into two paws lifting together.
            const auto& paws = kPaws[style_][beat];
            goal = {82 * gaze, -86 * paws[0], 86 * paws[1], 0, 0};
        } else if (mechanism == VoiceMechanism::kTail) {
            goal = {82 * gaze, 76 * gesture - 10 * gaze, -(100 + 44 * lift), 0, 0};
            if (style_ >= 8) {
                goal[1] = 60 * gesture - 28 * gaze;
                goal[2] = -(96 + 30 * lift);
            }
        } else {
            goal = {-62 * lift, 68 * gesture, 96 * gaze, 42 + 36 * lift, -82 * gesture + 10 * gaze};
            if (style_ >= 8)
                goal = {-42 * lift + 16 * gesture, 54 * gesture - 18 * gaze, 96 * gaze,
                        48 + 30 * lift, -56 * gesture - 26 * gaze};
        }
        if (intent == SpeechIntent::kSleep) {
            for (auto& axis : goal)
                axis *= .45f;
        }
        // The last beat yields before the next idea. No constant raised paw or
        // permanently pinned-up tail during a long monologue. The tail panda
        // keeps more momentum so its tail never looks like it stops and restarts.
        const float rest = beat == 3 ? (mechanism == VoiceMechanism::kTail ? .62f : .32f) : 1.f;
        if (mechanism == VoiceMechanism::kCrawl && !calm) {
            // A voiced stress accents the active paw; the resting paw stays still.
            goal[1] *= 1 + accent_ * .16f;
            goal[2] *= 1 + accent_ * .16f;
        }
        std::array<float, 5> result{};
        // Panda heads have yaw only: no per-syllable "nod" or high-frequency
        // sway on that axis. A gaze travels, arrives and waits for the next idea.
        tail_phase_ += dt * (2.0f + .5f * energy);
        if (tail_phase_ > 6.2831853f)
            tail_phase_ -= 6.2831853f;
        const unsigned tail_style = style_ % 4;
        const float tail_wave_lr = (tail_style == 0   ? 55.f
                                    : tail_style == 1 ? 45.f
                                                      : 35.f) *
                                   std::sin(tail_phase_) * quiet * energy;
        const float tail_wave_ud =
            (tail_style == 1 ? 38.f : 24.f) * std::cos(tail_phase_) * quiet * energy;
        // A slow sentence-level head arc keeps the yaw joint visibly engaged
        // without following every syllable attack.
        const float head_slow = std::sin(now * .001f * 1.1f + style_ * .7f) * quiet *
                                (energy > .02f ? std::max(energy, .30f) : 0.f) *
                                (16.f + 8.f * energy) * (calm ? .45f : 1.f);
        const std::array<float, 5> low =
            mechanism == VoiceMechanism::kDinosaur ? std::array<float, 5>{-70, -90, -90, -35, -90}
            : mechanism == VoiceMechanism::kTail   ? std::array<float, 5>{-60, -84, -140, 0, 0}
                                                   : std::array<float, 5>{-60, -82, -82, 0, 0};
        const std::array<float, 5> high =
            mechanism == VoiceMechanism::kDinosaur ? std::array<float, 5>{110, 90, 90, 90, 90}
            : mechanism == VoiceMechanism::kTail   ? std::array<float, 5>{60, 84, 28, 0, 0}
                                                   : std::array<float, 5>{60, 82, 82, 0, 0};
        for (size_t i = 0; i < goal.size(); ++i) {
            float target = goal[i] * energy * rest * expression * (1 + emphasis);
            if (mechanism == VoiceMechanism::kTail && i == 1)
                target += tail_wave_lr;
            else if (mechanism == VoiceMechanism::kTail && i == 2)
                target += tail_wave_ud;
            const bool head_axis = mechanism == VoiceMechanism::kDinosaur ? i == 2 : i == 0;
            if (head_axis)
                target = goal[i] * gaze_energy_ * (calm ? .78f : 1.f) + head_slow;
            target = (head_axis || mechanism == VoiceMechanism::kTail)
                         ? SoftLimit(target, low[i], high[i])
                         : std::clamp(target, low[i], high[i]);
            const float omega = head_axis ? 6.f : mechanism == VoiceMechanism::kTail ? 6.8f : 10.f;
            const float speed = mechanism == VoiceMechanism::kDinosaur ? 120.f
                                : head_axis                            ? 160.f
                                : mechanism == VoiceMechanism::kTail   ? 190.f
                                                                       : 280.f;
            result[i] = joints_[i].Follow(target, dt, omega, speed);
        }
        return result;
    }

private:
    using Intent = std::array<float, 3>;
    inline static constexpr Intent kPhrases[kPhraseCount][4] = {
        {{.7f, -.3f, .1f}, {1, -.8f, .8f}, {1, .7f, .3f}, {0, 0, 0}},
        {{-.7f, .2f, -.3f}, {-1, .8f, .6f}, {-1, -.7f, 1}, {0, 0, 0}},
        {{.3f, -.6f, .5f}, {-.8f, -.6f, .5f}, {.8f, .9f, -.2f}, {0, 0, 0}},
        {{-.6f, -.8f, .8f}, {-.6f, .1f, 1}, {.9f, .8f, .2f}, {0, 0, 0}},
        {{.2f, .7f, -.4f}, {-.8f, .9f, .2f}, {.8f, -.5f, .9f}, {0, 0, 0}},
        {{.9f, .4f, .1f}, {.9f, -.8f, .8f}, {-.7f, .6f, -.3f}, {0, 0, 0}},
        {{.9f, -.4f, .2f}, {.2f, .8f, 1}, {-.8f, .2f, .5f}, {0, 0, 0}},
        {{-.3f, .4f, .7f}, {-1, -.6f, .3f}, {-.2f, .8f, 1}, {0, 0, 0}},
        {{.2f, -.5f, .4f}, {.95f, -.85f, 1}, {.4f, .3f, .6f}, {0, 0, 0}},
        {{-.2f, .5f, .4f}, {-.95f, .85f, 1}, {-.4f, -.3f, .6f}, {0, 0, 0}},
        {{.45f, -.8f, .2f}, {.85f, -.4f, .9f}, {-.7f, .9f, .5f}, {0, 0, 0}},
        {{-.45f, .8f, .2f}, {-.85f, .4f, .9f}, {.7f, -.9f, .5f}, {0, 0, 0}},
        {{.35f, .2f, .6f}, {.9f, .75f, 1}, {.2f, -.6f, .8f}, {0, 0, 0}},
        {{-.35f, -.2f, .6f}, {-.9f, -.75f, 1}, {-.2f, .6f, .8f}, {0, 0, 0}},
        {{.1f, -.6f, .3f}, {.55f, .9f, .75f}, {-.85f, .5f, .9f}, {0, 0, 0}},
        {{-.1f, .6f, .3f}, {-.55f, -.9f, .75f}, {.85f, -.5f, .9f}, {0, 0, 0}},
    };
    inline static constexpr std::array<float, 2> kPaws[kPhraseCount][4] = {
        {{.35f, 0}, {1, 0}, {.5f, 0}, {0, 0}},         {{0, .35f}, {0, 1}, {0, .5f}, {0, 0}},
        {{.9f, 0}, {0, .95f}, {.75f, 0}, {0, 0}},      {{.65f, 0}, {.9f, 0}, {.9f, 0}, {0, 0}},
        {{.35f, .35f}, {.9f, .9f}, {.65f, 0}, {0, 0}}, {{-.65f, 0}, {-.3f, 0}, {0, .85f}, {0, 0}},
        {{0, .65f}, {0, 1}, {.65f, 0}, {0, 0}},        {{-.5f, 0}, {0, .85f}, {.65f, 0}, {0, 0}},
        {{.3f, 0}, {1, .12f}, {.7f, .12f}, {0, 0}},    {{0, .3f}, {.12f, 1}, {.12f, .7f}, {0, 0}},
        {{.8f, 0}, {.85f, .2f}, {0, .95f}, {0, 0}},    {{0, .8f}, {.2f, .85f}, {.95f, 0}, {0, 0}},
        {{.4f, .4f}, {1, 1}, {.6f, .6f}, {0, 0}},      {{-.8f, 0}, {-.5f, .3f}, {0, 1}, {0, 0}},
        {{0, -.8f}, {.3f, -.5f}, {1, 0}, {0, 0}},      {{.75f, 0}, {0, 1}, {.85f, .25f}, {0, 0}},
    };
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
        // Each sentence style owns a pool, rather than overriding a global
        // shuffle with the same two gestures every time that style appears.
        static constexpr unsigned counts[] = {16, 8, 8, 6, 8, 10, 6};
        static constexpr unsigned pools[7][kPhraseCount] = {
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
            {0, 1, 4, 6, 8, 9, 12, 15},
            {2, 4, 5, 6, 10, 11, 12, 15},
            {0, 3, 7, 8, 9, 13},
            {1, 2, 5, 7, 10, 11, 14, 15},
            {0, 2, 5, 6, 7, 8, 10, 13, 14, 15},
            {1, 3, 7, 9, 13, 14},
        };
        const unsigned bucket = std::min<unsigned>(static_cast<unsigned>(intent), 6);
        style_ = pools[bucket][styles_[bucket].Draw(counts[bucket], random_)];
        side_ = (++turn_ & 1U) ? 1.f : -1.f;
        period_ = 3300 + (random_ >> 8) % 1501;
        if (intent == SpeechIntent::kComfort || intent == SpeechIntent::kSleep)
            period_ += 900;
        if (intent == SpeechIntent::kJoy)
            period_ -= 450;
        else if (intent == SpeechIntent::kQuestion || intent == SpeechIntent::kGreeting)
            period_ -= 150;
        phrase_ms_ = now;
    }
    SpeechIntent intent_ = SpeechIntent::kNeutral;
    std::array<MotionShuffleBag<kPhraseCount>, 7> styles_;
    std::array<Joint, 5> joints_{};
    bool active_ = false;
    uint32_t last_ms_ = 0, phrase_ms_ = 0, period_ = 3600, turn_ = 0, random_ = 0x5a7e391U;
    uint32_t accent_ms_ = 0, last_voice_ms_ = 0;
    float accent_ = 0, tail_phase_ = 0;
    unsigned style_ = 0;
    float side_ = 1, level_ = 0, gaze_energy_ = 0;
};
}  // namespace wzh
#endif
