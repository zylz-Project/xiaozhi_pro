#ifndef WZH_SOUND_CHOREOGRAPHY_H_
#define WZH_SOUND_CHOREOGRAPHY_H_

#include <array>
#include <cmath>
#include "wzh_sound_variants.h"
#include "wzh_user_pair_motion.h"

namespace wzh {

// Completion-based cadence. Busy/disabled intervals restart the quiet interval,
// including conversation, touch, maintenance, protection and decoder contention.
class AutoplayCadence {
public:
    void Busy(uint32_t now, uint32_t random = 0) {
        since_ = now;
        // Short exhibition cadence: about two seconds after the previous
        // audio+motion finishes. Touch responses stay immediate.
        delay_ = 1800 + random % 600;
        armed_ = true;
    }
    bool Due(uint32_t now, bool eligible, uint32_t random) {
        if (!eligible || !armed_) {
            Busy(now, random);
            return false;
        }
        return now - since_ >= delay_;
    }

private:
    bool armed_ = false;
    uint32_t since_ = 0, delay_ = 2100;
};

// Between sounds and interactions the pet should still look alive. This
// cadence only fires while fully idle, then leaves the next few seconds to the
// clip/pulse that was just started.
class IdleCadence {
public:
    void Busy(uint32_t now, uint32_t random = 0) {
        since_ = now;
        delay_ = 3000 + random % 4000;
        armed_ = true;
    }
    bool Due(uint32_t now, bool eligible, uint32_t random) {
        if (!eligible || !armed_) {
            Busy(now, random);
            return false;
        }
        return now - since_ >= delay_;
    }

private:
    bool armed_ = false;
    uint32_t since_ = 0, delay_ = 6000;
};

// A bounded, allocation-free score player. Each file's measured envelope and
// content seed author its own path, with 32 phrase variants per file. Targets
// are physical angles: no cross-product axis inference or random endpoint gain.
class SoundChoreography {
public:
    static constexpr unsigned kVariantCount = 32;
    unsigned ReferenceVariant(const SoundPlayback& sound, uint32_t random, unsigned count) {
        if (!count || count > 8)
            return 0;
        // Source indexes already distinguish all 32 Flash + 32 paired slots.
        // Keep these tiny reference bags without another score-identity cache.
        auto& bag = sound.sound_index >= 0 && sound.sound_index < 64
                        ? reference_variants_[sound.sound_index]
                        : unknown_reference_;
        return bag.Draw(count, random);
    }
    template <class Clip>
    void SetReferenceClip(const Clip& clip) {
        expression_.SetReferenceClip(clip);
        user_motion_.SetReferenceClip(clip);
    }
    bool NeedsBegin(const SoundPlayback& sound) const {
        return sound.playing && sound.audible && sound.sequence != suppressed_ &&
               (!active_ || sound.sequence != sequence_);
    }
    // The dinosaur owns the seven sky-roar stories. Borrow their authored path,
    // then fit spatial landmarks to the actual recording instead of running an
    // unrelated fixed-length performance. Other mechanisms never call this.
    template <class Clip>
    void SetRoarClip(const Clip& clip, float compact_tail = 0) {
        compact_roar_tail_ = compact_tail > 0 ? std::clamp(compact_tail, 55.f, 180.f) : 0;
        roar_count_ = std::min<size_t>(clip.count, roar_poses_.size());
        SoundPose pose{70, 90, 90, 90, 90};
        for (size_t i = 0; i < roar_count_; ++i) {
            const auto& f = clip.frames[i];
            const int axes[] = {f.neck_tilt, f.neck_lean, f.head_turn, f.tail_ud, f.tail_lr};
            for (size_t a = 0; a < pose.size(); ++a) {
                if (axes[a] != 0x7fffffff)
                    pose[a] = std::clamp<float>(axes[a], a == 3 ? 55 : 0, 180);
            }
            roar_poses_[i] = pose;
        }
    }
    void Cancel(uint32_t sequence) {
        suppressed_ = sequence;
        active_ = false;
    }
    bool Update(const SoundPlayback& sound, PetMechanism mechanism, const SoundPose& origin,
                uint32_t now, SoundPose& out, bool& powered, uint32_t random = 0) {
        if (sound.playing && sound.sequence != suppressed_ && sound.audible) {
            if (!active_ || sound.sequence != sequence_) {
                sequence_ = sound.sequence;
                Begin(sound, mechanism, origin, random);
                active_ = true;
            }
            elapsed_ = sound.elapsed_ms;
            updated_ = now;
        } else if (!active_) {
            // Reserve the motion lane while the accepted sound awaits its first PCM.
            powered = false;
            return sound.playing && sound.sequence != suppressed_;
        }
        if (!active_)
            return false;
        const uint32_t time = elapsed_ + (sound.playing ? 0 : now - updated_);
        if (time >= duration()) {
            active_ = false;
            suppressed_ = sequence_;
            // The motion task settles from the actual PWM pose, even after a
            // delayed tick; never issue a one-frame neutral jump here.
            powered = false;
            return false;
        }
        out = Sample(time, powered);
        return true;
    }

    uint32_t duration() const {
        return tail_motion_.active()   ? tail_motion_.duration()
               : user_motion_.active() ? user_motion_.duration()
               : count_                ? frames_[count_ - 1].end
                                       : 0;
    }
    uint32_t body_duration() const {
        return tail_motion_.active()   ? tail_motion_.body_duration()
               : user_motion_.active() ? user_motion_.body_duration()
                                       : duration();
    }
    size_t count() const { return count_; }
    unsigned variant() const { return variant_; }

    void Begin(const SoundPlayback& sound, PetMechanism mechanism, const SoundPose& origin,
               uint32_t random = 0) {
        mechanism_ = mechanism;
        origin_ = origin;
        rest_ = mechanism == PetMechanism::kDinosaur ? SoundPose{70, 90, 90, 90, 90}
                : mechanism == PetMechanism::kTail   ? SoundPose{90, 90, 150, 0, 0}
                                                     : SoundPose{90, 90, 90, 0, 0};
        count_ = 0;
        tail_motion_.Cancel();
        const unsigned variant = variant_ = variants_.Draw(sound, random);
        auto seed = sound.score ? sound.score->seed : 0xabc123U;
        if (mechanism == PetMechanism::kTail && sound.kind != SoundKind::kAmbient) {
            tail_leading_side_ = !tail_leading_side_;
            seed = (seed & ~1U) | (unsigned(tail_leading_side_) ^ (variant & 1U));
        }
        user_motion_.Begin(UserPairAtIndex(sound.sound_index), origin_, rest_, variant, seed,
                           sound.kind);
        if (user_motion_.active())
            return;
        if (mechanism == PetMechanism::kTail && sound.kind != SoundKind::kAmbient) {
            tail_motion_.Begin(sound.duration_ms, origin, sound.kind, variant, seed);
            return;
        }
        const bool ambient = sound.kind == SoundKind::kAmbient;
        // Short recordings may finish before the gesture reads. Let the
        // choreography overrun the audio instead of compressing the motion.
        const uint32_t min_length = mechanism == PetMechanism::kDinosaur ? 1200 : 900;
        const uint32_t length = ambient ? std::max<uint32_t>(sound.duration_ms, 300)
                                        : std::max<uint32_t>(sound.duration_ms, min_length);
        const float direction = ((seed & 1U) ^ (variant & 1U)) ? 1.f : -1.f;
        const bool dino = mechanism == PetMechanism::kDinosaur;
        expression_.Begin(mechanism_, sound.score, sound.kind, length, variant, seed, false);
        if (ambient) {
            // Observe, settle, then relax. Four frames per 12-second scene fit
            // the full long ambience in the fixed buffer and bound torque time.
            for (uint32_t base = 0; base < length && count_ + 4 <= frames_.size(); base += 12000) {
                for (unsigned part = 0; part < 4; ++part) {
                    const uint32_t at = base + (part == 3 ? 8000 : part * 2000);
                    if (at >= length)
                        break;
                    const auto target =
                        part < 2 ? Target(sound, at + 1000, part, direction, variant) : rest_;
                    Append(target, std::min<uint32_t>(part >= 2 ? 4000 : 2000, length - at),
                           part == 3);
                }
            }
            if (count_ && frames_[count_ - 1].pose != rest_)
                Append(rest_, dino ? 1000 : 700, false);
            return;
        }
        // Active long-neck gestures use >=300 ms and <=133 deg/s average. Other products
        // use <=266 deg/s average so smoothstep peaks stay within 400 deg/s.
        const uint32_t beat = dino ? (sound.kind == SoundKind::kRoar ? 700 : 1100)
                              : sound.kind == SoundKind::kSteps ? 600
                              : sound.kind == SoundKind::kSleep ? 1800
                                                                : 520;
        const unsigned n = std::clamp<unsigned>(length / beat, 2, 48);
        // Per-playback rhythm: non-uniform beat lengths plus occasional holds.
        // The recording still defines the overall length; this only removes the
        // metronome so the same file does not read as the same loop every time.
        uint32_t rhythm = seed ^ (variant * 0x9e3779b9U);
        auto next_rhythm = [&rhythm]() {
            rhythm = rhythm * 1664525U + 1013904223U;
            return rhythm;
        };
        std::array<uint32_t, 48> weights{};
        uint32_t weight_sum = 0;
        for (unsigned i = 0; i < n; ++i) {
            weights[i] = 700 + next_rhythm() % 601;  // 0.70x .. 1.30x beat
            weight_sum += weights[i];
        }
        uint32_t previous_at = 0, cumulative = 0;
        SoundPose previous_pose = origin_;
        for (unsigned i = 0; i < n; ++i) {
            cumulative += weights[i];
            const uint32_t at = i + 1 == n ? length
                                           : static_cast<uint32_t>(static_cast<uint64_t>(length) *
                                                                   cumulative / weight_sum);
            SoundPose target = Target(sound, at, i, direction, variant);
            if (!(dino && sound.kind == SoundKind::kRoar))
                target = expression_.Apply(target, at);
            if (dino && sound.kind == SoundKind::kRoar && i == 0) {
                target = Broaden(roar_count_ ? roar_poses_[0] : SoundPose{0, 90, 90, 140, 90},
                                 sound.kind);
                // A short recording has only lift/return time. Preserve its
                // skyward onset while keeping the board's compact tail story
                // visible; otherwise different borrowed clips collapse to one.
                if (n == 2 && compact_roar_tail_ > 0)
                    target[3] = compact_roar_tail_;
            } else if (i > 0 && next_rhythm() % 100 < 18) {
                // Let the pose wait for one beat instead of moving on every grid
                // line. The next beat still starts from this held pose.
                target = previous_pose;
            }
            Append(target, at - previous_at, false);
            previous_at = at;
            previous_pose = target;
        }
        // Do not snap back to neutral at the exact audio end. Hold the last
        // expressive pose for a beat, then return slowly; the timeline keeps
        // running after playback stops.
        if (count_ && frames_[count_ - 1].pose != rest_) {
            Append(frames_[count_ - 1].pose, dino ? 180 : 220, false);
            Append(rest_, dino ? 1100 : 900, true);
        }
    }

    SoundPose Sample(uint32_t time, bool& powered) const {
        if (tail_motion_.active()) {
            powered = time < tail_motion_.duration();
            return tail_motion_.Sample(time);
        }
        if (user_motion_.active()) {
            powered = time < user_motion_.duration();
            return user_motion_.Sample(time);
        }
        SoundPose from = origin_;
        uint32_t start = 0;
        for (size_t i = 0; i < count_; ++i) {
            const auto& f = frames_[i];
            if (time < f.end) {
                float t = float(time - start) / std::max<uint32_t>(1, f.end - start);
                SoundPose out{};
                const auto& previous = i > 1 ? frames_[i - 2].pose : origin_;
                const auto& next = i + 1 < count_ ? frames_[i + 1].pose : f.pose;
                const uint32_t before =
                    i ? frames_[i - 1].end - (i > 1 ? frames_[i - 2].end : 0) : f.end - start;
                const uint32_t after = i + 1 < count_ ? frames_[i + 1].end - f.end : f.end - start;
                for (size_t axis = 0; axis < out.size(); ++axis)
                    out[axis] = f.settling
                                    ? from[axis] + (f.pose[axis] - from[axis]) * SettlingProgress(t)
                                    : MotionHermite(previous[axis], from[axis], f.pose[axis],
                                                    next[axis], before, f.end - start, after, t);
                // A release flag on a return segment must not cut torque before
                // arrival. Only a stationary resting segment can relax early.
                powered = !f.release || from != f.pose;
                return out;
            }
            start = f.end;
            from = f.pose;
        }
        powered = false;
        return rest_;
    }

private:
    struct Frame {
        SoundPose pose{};
        uint32_t end = 0;
        bool release = false;
        bool settling = false;
    };
    std::array<Frame, 128> frames_{};
    std::array<SoundPose, 16> roar_poses_{};
    size_t roar_count_ = 0;
    float compact_roar_tail_ = 0;
    size_t count_ = 0;
    PetMechanism mechanism_ = PetMechanism::kCrawl;
    SoundPose origin_{}, rest_{};
    bool active_ = false;
    uint32_t sequence_ = 0, suppressed_ = 0, elapsed_ = 0, updated_ = 0;
    SoundVariantSelector<kVariantCount> variants_;
    std::array<MotionShuffleBag<8>, 64> reference_variants_;
    MotionShuffleBag<8> unknown_reference_;
    uint32_t variant_ = 0;
    SoundExpression expression_;
    UserPairMotion user_motion_;
    TailMotion tail_motion_;
    bool tail_leading_side_ = false;

    SoundPose Broaden(SoundPose pose, SoundKind kind) const {
        const bool dino = mechanism_ == PetMechanism::kDinosaur;
        const bool tail = mechanism_ == PetMechanism::kTail;
        const SoundPose low = dino   ? SoundPose{0, 0, 0, 55, 0}
                              : tail ? SoundPose{8, 6, 10, 0, 0}
                                     : SoundPose{8, 8, 8, 0, 0};
        const SoundPose high = dino   ? SoundPose{180, 180, 180, 180, 180}
                               : tail ? SoundPose{172, 174, 178, 0, 0}
                                      : SoundPose{172, 172, 172, 0, 0};
        const float gain = kind == SoundKind::kSleep     ? 1.26f
                           : kind == SoundKind::kAmbient ? 1.40f
                                                         : 1.45f;
        for (unsigned axis = 0; axis < (dino ? 5U : 3U); ++axis)
            pose[axis] = StoryAngle(pose[axis], rest_[axis], low[axis], high[axis], gain);
        if (!dino)
            pose[0] = PandaHeadTarget(pose[0]);
        return pose;
    }

    void Append(const SoundPose& target, uint32_t requested, bool release) {
        if (count_ == frames_.size())
            return;
        const auto& from = count_ ? frames_[count_ - 1].pose : origin_;
        float travel = 0;
        for (size_t i = 0; i < target.size(); ++i)
            travel = std::max(travel, std::abs(target[i] - from[i]));
        const bool dino = mechanism_ == PetMechanism::kDinosaur;
        const uint32_t minimum =
            travel > .1f ? std::max<uint32_t>(
                               dino ? 300 : 160,
                               static_cast<uint32_t>(std::ceil(travel * (dino ? 7.5f : 3.75f))))
                         : 1;
        const bool settling = target == rest_ && travel > .1f;
        const uint32_t duration =
            settling ? std::max(requested, SettlingDuration(
                                               travel, dino                                ? 100.f
                                                       : mechanism_ == PetMechanism::kTail ? 90.f
                                                                                           : 115.f))
                     : std::max(requested, minimum);
        frames_[count_] = {target, (count_ ? frames_[count_ - 1].end : 0) + duration, release,
                           settling};
        ++count_;
    }

    SoundPose Target(const SoundPlayback& sound, uint32_t at, unsigned frame, float direction,
                     unsigned variant) const {
        const uint32_t seed = sound.score ? sound.score->seed : 19;
        float energy = .6f;
        if (sound.score && sound.score->count) {
            const size_t bin = std::min<size_t>(sound.score->count - 1, at / 100);
            // Integrate 300 ms rather than twitch at individual sample peaks.
            float sum = 0;
            unsigned n = 0;
            for (size_t i = bin > 1 ? bin - 2 : 0; i <= bin; ++i) {
                sum += sound.score->envelope[i];
                ++n;
            }
            energy = sum / (255.f * n);
        }
        const float e = .35f + .65f * std::sqrt(energy);
        // Glance/reach, double-check, hold/answer, horizon sweep, proud display,
        // playful exchange, recoil/recover and curl: not a fixed left/right loop.
        static constexpr float kPhrasing[16][4] = {
            {1, .3f, -.6f, .2f},     {.6f, 1, -1, .4f},        {.8f, 1, .2f, -.5f},
            {.2f, -.8f, -1, .6f},    {.9f, .4f, -.4f, -.9f},   {.4f, -.6f, .8f, -.3f},
            {-.7f, -1, .5f, .3f},    {.5f, .1f, -.5f, -.2f},   {.95f, -.4f, .7f, -.8f},
            {.25f, .6f, -.9f, .5f},  {.35f, 1.2f, -.7f, .9f},  {.65f, -1.2f, .4f, -1},
            {.55f, .9f, -.2f, -.6f}, {-.45f, .7f, 1.1f, -.9f}, {.75f, -1.1f, -.8f, .6f},
            {.85f, .2f, .9f, .4f},
        };
        const unsigned motif = variant % 16;
        const bool answering = variant >= 16;
        const unsigned phrase = (frame + (answering ? 1 : 0)) % 4;
        const float side = direction * kPhrasing[motif][phrase];
        const float gaze =
            mechanism_ == PetMechanism::kDinosaur
                ? side
                : direction * kPhrasing[motif][(frame / 2 + (answering ? 2 : 0)) % 4];
        const float detail = .85f + float((seed >> 12) % 16) / 100.f;
        float head = gaze * (54 + 30 * e) * detail;
        float lift = e;
        switch (sound.kind) {
            case SoundKind::kSleep:
                head *= .35f;
                lift *= .30f;
                break;
            case SoundKind::kSneeze:
                head = gaze * 48;
                break;
            case SoundKind::kAffection:
                head *= .8f;
                break;
            case SoundKind::kSearch:
                head = gaze * 72;
                break;
            case SoundKind::kStretch:
                lift = 1;
                break;
            case SoundKind::kAmbient:
                head = side * (mechanism_ == PetMechanism::kDinosaur ? 62 + 26 * e : 40 + 20 * e);
                lift *= mechanism_ == PetMechanism::kDinosaur ? .88f : .55f;
                break;
            default:
                break;
        }
        if (mechanism_ == PetMechanism::kTail) {
            if (sound.kind != SoundKind::kAmbient && sound.kind != SoundKind::kSleep) {
                auto story = TailPhrase(float(at) / std::max<uint32_t>(sound.duration_ms, 900),
                                        variant, direction, sound.duration_ms);
                if (sound.kind == SoundKind::kChew) {
                    story[0] = 90 + (story[0] - 90) * .65f;
                    story[2] = 150 + (story[2] - 150) * .70f;
                }
                return Broaden({story[0], story[1], story[2], 0, 0}, sound.kind);
            }
            const float arch = .84f + .16f * ((frame + variant) % 3) / 2.f;
            float ud = 150 - (124 + 44 * lift) * arch;
            float lr = 90 - side * (68 + 30 * e);
            if (sound.kind == SoundKind::kSleep)
                ud = 135 - 20 * e;
            if (sound.kind == SoundKind::kChew) {
                head *= .45f;
                ud = 100 - 45 * e;
            }
            if (sound.kind == SoundKind::kSteps)
                lr = 90 + side * 78;
            // An inviting curl, a high sweep, a side answer: independent per-file path.
            if ((variant % 3) == 1 && (frame % 3) == 1)
                ud = std::min(ud, 10.f);
            if (sound.duration_ms < 1000)
                ud = std::max(ud, 10.f);
            return Broaden({90 + head, lr, std::clamp(ud, 10.f, 178.f), 0, 0}, sound.kind);
        }
        if (mechanism_ == PetMechanism::kCrawl) {
            // Positive means an offered paw, negative a tucked/face-side paw.
            // Most motifs use one paw and leave the other sitting, rather than
            // moving both shoulders up and down on the same clock.
            static constexpr std::array<float, 2> kPaws[16][4] = {
                {{.9f, 0}, {.45f, 0}, {1, 0}, {0, 0}},
                {{0, .9f}, {0, .4f}, {0, 1}, {0, 0}},
                {{1, 0}, {0, 1}, {.85f, 0}, {0, .85f}},
                {{.75f, 0}, {.75f, 0}, {.15f, .8f}, {0, .2f}},
                {{-.75f, 0}, {-.2f, .8f}, {0, .8f}, {.7f, 0}},
                {{.9f, .15f}, {.9f, .15f}, {.25f, 0}, {0, .8f}},
                {{.8f, .8f}, {.45f, .45f}, {.9f, 0}, {0, .9f}},
                {{.8f, 0}, {.2f, .9f}, {-.3f, .4f}, {0, .8f}},
                {{1, .1f}, {.1f, .9f}, {.85f, 0}, {0, .6f}},
                {{0, .8f}, {.7f, .2f}, {.35f, .9f}, {0, .5f}},
                {{.9f, .2f}, {.2f, .7f}, {.8f, .3f}, {0, .9f}},
                {{-.8f, .3f}, {.4f, .9f}, {.9f, .1f}, {0, .4f}},
                {{.6f, .6f}, {.9f, .2f}, {.2f, .8f}, {0, .7f}},
                {{.9f, .4f}, {.3f, .9f}, {.7f, .1f}, {0, .5f}},
                {{.2f, .8f}, {.8f, .4f}, {.5f, .9f}, {0, .6f}},
                {{.7f, .1f}, {.1f, .9f}, {.9f, .3f}, {0, .8f}},
            };
            auto paws = kPaws[motif][phrase];
            if (answering)
                std::swap(paws[0], paws[1]);
            float left = 90 - paws[0] * (56 + 34 * lift);
            float right = 90 + paws[1] * (56 + 34 * lift);
            if (sound.kind == SoundKind::kSteps) {
                const float stride = 64 + (variant % 3) * 7;
                left = 90 + ((frame & 1) ? stride : -stride);
                right = motif == 6 ? 180 - left : left;
                head = gaze * 60;
            } else if (sound.kind == SoundKind::kChew) {
                head *= .40f;
                left = variant & 1 ? 38 : (frame & 1) ? 38 : 76;
                right = variant & 1 ? (frame & 1) ? 142 : 104 : 142;
            } else if (sound.kind == SoundKind::kSleep) {
                left = 90 - 14 * e;
                right = 90 + 14 * e;
            } else if (sound.kind == SoundKind::kStretch && frame % 4 == 0) {
                left = 18;
                right = 162;
            }
            return Broaden({90 + head, left, right, 0, 0}, sound.kind);
        }
        // Some variants nod down through the lower neck range, others lift
        // skyward; the tail uses its full 55..180 range instead of hovering
        // around the middle. A per-beat neck/tail orbit keeps both ends of the
        // body moving even when the recording's envelope is flat.
        const float dip = variant % 3 == 0 ? 55.f : variant % 3 == 1 ? 22.f : 0.f;
        const float orbit = std::sin((frame + motif) * 1.5707963f);
        const float orbit_cross = std::cos((frame + motif) * 1.5707963f);
        const float burst = orbit * (.4f + .6f * e);
        float tilt = 40 - 30 * burst + dip;
        const float answer = direction * kPhrasing[(motif + 3) % 16][phrase];
        float lean = 90 + answer * 62 * e + orbit_cross * 18 * (.4f + .6f * e);
        float turn = 90 + side * (62 + 22 * e) * detail - orbit_cross * 14 * (.4f + .6f * e);
        float ud = 118 + 62 * burst;
        float lr = 90 - answer * (56 + 28 * e) + orbit_cross * 22 * (.4f + .6f * e);
        if (sound.kind == SoundKind::kRoar) {
            if (roar_count_) {
                const size_t landmark =
                    std::min<size_t>(roar_count_ - 1, uint64_t(at) * roar_count_ /
                                                          std::max<uint32_t>(1, sound.duration_ms));
                auto pose = roar_poses_[landmark];
                pose[3] = std::min(180.f, pose[3] + 8.f * energy);
                // Keep the skyward onset unchanged. Later phrases fan out into
                // a proud call, an echo, or a head/tail answer. Even a short
                // call has 32 distinct paths instead of 32 IDs for 3 poses.
                const float spread = (float(motif) - 7.5f) / 7.5f;
                pose[0] = std::min(pose[0], 28.f);
                pose[1] += (answering ? -1 : 1) * spread * 14;
                pose[2] += spread * (answering ? -28 : 28);
                pose[3] += answering ? -14 : 0;
                pose[4] += (answering ? 24 : -24) * spread;
                return Broaden(pose, sound.kind);
            }
            tilt = frame % 3 == 0 ? 0 : 4;
            lean = 90 + side * 18;
            turn = 90 + side * 28;
            lr = 90 - side * 24;
        } else if (sound.kind == SoundKind::kChew) {
            tilt = frame & 1 ? 154 : 180;
            lean = 90 + side * 22;
            turn = 90 + side * 24;
            ud = 105;
        } else if (sound.kind == SoundKind::kSteps) {
            tilt = frame & 1 ? 46 : 90;
            lean = 90 + side * 42;
            turn = 90 - side * 38;
        } else if (sound.kind == SoundKind::kSleep) {
            tilt = 105 + 12 * e;
            lean = 90 + side * 12;
            turn = 90 + side * 18;
            ud = 76;
        } else if (sound.kind == SoundKind::kSneeze) {
            // The long neck drops toward the ground, then recoils upward.
            tilt = frame % 3 == 0 ? 156 : 126 + 18 * e;
            lean = 90 + side * 10;
            turn = 90;
            ud = 140;
            lr = 90;
        }
        return Broaden({tilt, lean, turn, ud, lr}, sound.kind);
    }
};
}  // namespace wzh
#endif
