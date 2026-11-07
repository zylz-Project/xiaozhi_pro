#ifndef WZH_USER_PAIR_MOTION_H_
#define WZH_USER_PAIR_MOTION_H_

#include <array>
#include <cmath>
#include "wzh_sound_expression.h"
#include "wzh_tail_motion.h"
#include "wzh_user_pairs.h"

namespace wzh {
// Keep the user's physical path as the main performance. Expand shorter motion
// over the audio, add one coordinated expression, then settle separately.
class UserPairMotion {
public:
    using Pose = std::array<float, 5>;
    template <class Clip>
    void SetReferenceClip(const Clip& clip) {
        expression_.SetReferenceClip(clip);
    }
    void Begin(const UserPair* pair, const Pose& origin, const Pose& rest, unsigned variant = 0,
               uint32_t seed = 0, SoundKind kind = SoundKind::kCall) {
        pair_ = pair;
        tail_motion_.Cancel();
        origin_ = origin;
        rest_ = rest;
        if (!pair_)
            return;
        kind_ = pair_->score ? pair_->score->kind : kind;
        // Sparse cumulative index: avoid replaying all historical recorder
        // frames on every 20 ms tick. 128 bytes, no allocation on the audio path.
        block_stride_ = (pair_->frame_count + block_ends_.size() - 1) / block_ends_.size();
        block_count_ = 0;
        uint32_t end = 0;
        for (unsigned i = 0; i < pair_->frame_count; ++i) {
            end += pair_->frames[i].duration_ms;
            if ((i + 1) % block_stride_ == 0 || i + 1 == pair_->frame_count)
                block_ends_[block_count_++] = end;
        }
        const auto first = FramePose(0), last = FramePose(pair_->frame_count - 1);
        float entry_travel = 0, exit_travel = 0;
        for (unsigned a = 0; a < pair_->axes; ++a) {
            entry_travel = std::max(entry_travel, std::abs(origin[a] - first[a]));
            exit_travel = std::max(exit_travel, std::abs(rest[a] - last[a]));
        }
        const bool tail = pair_->axes == 3 && rest[2] == 150;
        body_ms_ = std::max<uint32_t>({pair_->audio_ms, pair_->motion_ms,
                                       pair_->axes == 5 ? 1600U
                                       : tail           ? 2200U
                                                        : 1600U});
        entry_ms_ = std::min<uint32_t>(
            body_ms_ / 2, std::clamp<uint32_t>(std::ceil(entry_travel * (tail ? 8 : 10)),
                                               tail ? 750 : 600, 1200));
        if (pair_->axes == 5 && kind_ == SoundKind::kRoar) {
            const Pose sky{0, 90, 90, 150, 90};
            float travel = 0;
            for (unsigned a = 0; a < 5; ++a)
                travel = std::max(travel, std::abs(origin[a] - sky[a]));
            // A call interrupting low feeding has much farther to travel than
            // one starting at neutral. Keep <=133 deg/s average on entry, and
            // leave time for the recorded detail before the separate return.
            entry_ms_ = std::max<uint32_t>(650, std::ceil(travel * 7.5f));
            body_ms_ = std::max(body_ms_, entry_ms_ + 1150);
        }
        const float speed = kind_ == SoundKind::kSleep ? 65.f
                            : pair_->axes == 5         ? 100.f
                            : rest[2] == 150           ? 90.f
                                                       : 115.f;
        exit_ms_ = SettlingDuration(exit_travel, speed);
        window_ms_ = std::min<uint32_t>(
            pair_->axes == 5
                ? ((kind_ == SoundKind::kSteps || kind_ == SoundKind::kRoar) ? 600 : 360)
            : rest[2] == 150 ? 600
                             : (kind_ == SoundKind::kSneeze ? 420 : 320),
            body_ms_ / 3);
        expression_.Begin(pair_->axes == 5 ? PetMechanism::kDinosaur
                          : rest[2] == 150 ? PetMechanism::kTail
                                           : PetMechanism::kCrawl,
                          pair_->score, kind_, body_ms_, variant, seed, true);
        if (tail) {
            Pose low = FramePose(0), high = low;
            for (unsigned i = 1; i < pair_->frame_count; ++i) {
                const auto q = FramePose(i);
                for (unsigned a = 0; a < 3; ++a) {
                    low[a] = std::min(low[a], q[a]);
                    high[a] = std::max(high[a], q[a]);
                }
            }
            tail_motion_.Begin(std::max(pair_->audio_ms, pair_->motion_ms), origin, kind_, variant,
                               seed);
            tail_motion_.SetRecordedRange(low, high);
        }
    }
    bool active() const { return pair_ != nullptr; }
    uint32_t duration() const {
        return tail_motion_.active() ? tail_motion_.duration() : pair_ ? body_ms_ + exit_ms_ : 0;
    }
    uint32_t body_duration() const {
        return tail_motion_.active() ? tail_motion_.body_duration() : body_ms_;
    }
    Pose Sample(uint32_t time) const {
        if (!pair_)
            return rest_;
        if (tail_motion_.active()) {
            const auto detail = Filtered(std::min(time, body_ms_));
            return tail_motion_.Sample(time, &detail);
        }
        Pose last = FramePose(pair_->frame_count - 1);
        if (time >= body_ms_) {
            const float t = SettlingProgress(float(time - body_ms_) / exit_ms_);
            for (unsigned a = 0; a < pair_->axes; ++a)
                last[a] += (rest_[a] - last[a]) * t;
            return last;
        }
        Pose out = Filtered(time);
        out = expression_.Apply(out, time);
        // End at the user's final landmark with zero velocity/acceleration,
        // so the separate return never starts with a position discontinuity.
        const uint32_t end_window =
            pair_->axes == 3
                ? std::min<uint32_t>(window_ms_ * 2, std::max<uint32_t>(750, body_ms_ / 3))
                : window_ms_ * 2;
        if (time + end_window > body_ms_) {
            const float blend = SettlingProgress(float(time + end_window - body_ms_) / end_window);
            for (unsigned a = 0; a < pair_->axes; ++a)
                out[a] += (last[a] - out[a]) * blend;
        }
        if (pair_->axes == 5 && kind_ == SoundKind::kRoar) {
            // Match the reference's front-facing skyward onset before the
            // recorded neck/head/tail lateral details unfold. One shared
            // progress owns all changing axes; no staggered servo clocks.
            const float reveal = SettlingProgress((float(time) - entry_ms_) / 650);
            const Pose sky{0, 90, 90, 150, 90};
            for (unsigned a = 0; a < 5; ++a)
                out[a] = sky[a] + (out[a] - sky[a]) * reveal;
        }
        if (time < entry_ms_) {
            const float blend = SettlingProgress(float(time) / entry_ms_);
            for (unsigned a = 0; a < pair_->axes; ++a)
                out[a] = origin_[a] + (out[a] - origin_[a]) * blend;
        }
        return out;
    }

private:
    TailMotion tail_motion_;
    Pose FramePose(unsigned index) const {
        Pose pose = rest_;
        for (unsigned a = 0; a < pair_->axes; ++a)
            pose[a] = pair_->frames[index].angles[a];
        if (pair_->axes == 3)
            pose[0] = PandaHeadTarget(pose[0]);
        if (pair_->axes == 5)
            pose[0] = DinosaurContentTilt(pose[0], kind_);
        if (kind_ != SoundKind::kSleep && kind_ != SoundKind::kAmbient) {
            const bool dino = pair_->axes == 5, tail = !dino && rest_[2] == 150;
            for (unsigned a = 1; a < pair_->axes; ++a)
                pose[a] = StoryAngle(pose[a], rest_[a],
                                     dino   ? (a == 3 ? 55 : 0)
                                     : tail ? (a == 1 ? 6 : 10)
                                            : 8,
                                     dino   ? 180
                                     : tail ? (a == 1 ? 174 : 178)
                                            : 172,
                                     dino ? 1.10f : 1.10f);
        }
        if (pair_->axes == 5 && kind_ == SoundKind::kSteps) {
            // Walking/listening needs an alert neck, not the ground-feeding
            // posture present in some joystick recordings. Keep its direction
            // and broad lateral travel; do not swap or reverse physical axes.
            pose[0] = 70 + (pose[0] - 70) * .42f;
        }
        return pose;
    }
    Pose Filtered(uint32_t time) const {
        const float scale = float(pair_->motion_ms) / body_ms_;
        const float at = time * scale, width = window_ms_ * scale;
        // Integrate a triangular kernel over the *continuous* recorded curve.
        // Four-point Gaussian quadrature is exact for our cubic Hermite or
        // quintic easing multiplied by a linear kernel. Sparse point taps can
        // alias 20 ms stick reversals into sharp speed pulses; integration does
        // not. Positive weights keep every joint in its authored physical box.
        auto cdf = [](float z) {
            z = std::clamp(z, -1.f, 1.f);
            return z < 0 ? .5f * (z + 1) * (z + 1) : 1 - .5f * (1 - z) * (1 - z);
        };
        const float before_weight = cdf(-at / width);
        const float after_weight = 1 - cdf((pair_->motion_ms - at) / width);
        const Pose last = FramePose(pair_->frame_count - 1);
        Pose result{};
        float weight_sum = before_weight + after_weight;
        for (unsigned a = 0; a < pair_->axes; ++a)
            result[a] = origin_[a] * before_weight + last[a] * after_weight;
        const float left_edge = at - width;
        const auto block =
            std::upper_bound(block_ends_.begin(), block_ends_.begin() + block_count_, left_edge) -
            block_ends_.begin();
        const unsigned first = std::min<unsigned>(block * block_stride_, pair_->frame_count - 1);
        uint32_t start = block ? block_ends_[block - 1] : 0;
        Pose from = first ? FramePose(first - 1) : origin_;
        Pose previous = first > 1 ? FramePose(first - 2) : origin_;
        for (unsigned i = first; i < pair_->frame_count; ++i) {
            const auto& frame = pair_->frames[i];
            const uint32_t end = start + frame.duration_ms;
            const Pose target = FramePose(i);
            const Pose next = FramePose(std::min<unsigned>(i + 1, pair_->frame_count - 1));
            const float lo = std::max<float>(start, at - width);
            const float hi = std::min<float>(end, at + width);
            if (hi > lo) {
                const uint32_t before = i ? pair_->frames[i - 1].duration_ms : frame.duration_ms;
                const uint32_t after =
                    pair_->frames[std::min<unsigned>(i + 1, pair_->frame_count - 1)].duration_ms;
                constexpr float nodes[] = {-.8611363116f, -.3399810436f, .3399810436f,
                                           .8611363116f};
                constexpr float weights[] = {.3478548451f, .6521451549f, .6521451549f,
                                             .3478548451f};
                // Split at the triangular kernel's apex before integrating.
                const float split = std::clamp(at, lo, hi);
                for (unsigned part = 0; part < 2; ++part) {
                    const float left = part ? split : lo, right = part ? hi : split;
                    if (right <= left)
                        continue;
                    const float center = (left + right) * .5f, half = (right - left) * .5f;
                    for (unsigned q = 0; q < 4; ++q) {
                        const float sample = center + half * nodes[q];
                        const float weight =
                            half * weights[q] * (1 - std::abs(sample - at) / width) / width;
                        const float t = (sample - start) / frame.duration_ms;
                        weight_sum += weight;
                        for (unsigned a = 0; a < pair_->axes; ++a) {
                            const float value =
                                frame.ease == 0
                                    ? from[a] + (target[a] - from[a]) * SettlingProgress(t)
                                    : MotionHermite(previous[a], from[a], target[a], next[a],
                                                    before, frame.duration_ms, after, t);
                            result[a] += value * weight;
                        }
                    }
                }
            }
            previous = from;
            from = target;
            start = end;
            if (start >= at + width)
                break;
        }
        for (unsigned a = 0; a < pair_->axes; ++a)
            result[a] /= weight_sum;
        return result;
    }
    const UserPair* pair_ = nullptr;
    SoundKind kind_ = SoundKind::kCall;
    SoundExpression expression_;
    std::array<uint32_t, 32> block_ends_{};
    unsigned block_stride_ = 1, block_count_ = 0;
    Pose origin_{}, rest_{};
    uint32_t entry_ms_ = 220, exit_ms_ = 900, body_ms_ = 0, window_ms_ = 70;
};
}  // namespace wzh
#endif
