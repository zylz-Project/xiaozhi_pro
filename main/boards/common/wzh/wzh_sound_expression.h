#ifndef WZH_SOUND_EXPRESSION_H_
#define WZH_SOUND_EXPRESSION_H_

#include <array>
#include <cmath>
#include "wzh_motion_profile.h"
#include "wzh_sound_score.h"
#include "wzh_tail_phrase.h"

namespace wzh {
enum class PetMechanism : uint8_t { kCrawl, kTail, kDinosaur };
using SoundPose = std::array<float, 5>;

// Preserve the recorded rhythm, but put it in the register of the sound:
// IO17=0 faces the sky; IO17=180 reaches the food. Do this before filtering,
// including the final landmark, so entry and return remain continuous.
inline float DinosaurContentTilt(float angle, SoundKind kind) {
    const float p = std::clamp(angle / 180.f, 0.f, 1.f);
    if (kind == SoundKind::kRoar)
        return 42 * p;
    if (kind == SoundKind::kChew)
        return 148 + 32 * p;
    return angle;
}

// Whole-performance expression, inspired by the reference dinosaur's spatial
// styles. All joints share one clock. Choices are made once, not on servo ticks;
// finite broad arcs and deliberate looks replace per-syllable random wiggles.
// Only the motion task owns this object. No allocations, I/O or audio decoding.
class SoundExpression {
public:
    template <class Clip>
    void SetReferenceClip(const Clip& clip) {
        reference_count_ = std::min<size_t>(clip.count, reference_.size());
        SoundPose previous{70, 90, 90, 90, 90};
        reference_ms_ = 0;
        for (size_t i = 0; i < reference_count_; ++i) {
            const auto& frame = clip.frames[i];
            const int axes[] = {frame.neck_tilt, frame.neck_lean, frame.head_turn, frame.tail_ud,
                                frame.tail_lr};
            auto pose = previous;
            float travel = 0;
            for (unsigned a = 0; a < 5; ++a) {
                if (axes[a] != 0x7fffffff)
                    pose[a] = std::clamp<float>(axes[a], a == 3 ? 55 : 0, 180);
                travel = std::max(travel, std::abs(pose[a] - previous[a]));
            }
            const uint32_t duration = std::max<uint32_t>(
                frame.duration_ms, travel > .1f ? std::max(420.f, std::ceil(travel * 12.5f)) : 1);
            reference_ms_ += duration;
            reference_[i] = {pose, reference_ms_};
            previous = pose;
        }
    }
    void Begin(PetMechanism mechanism, const SoundScore* score, SoundKind kind, uint32_t duration,
               unsigned variant, uint32_t seed, bool recorded) {
        mechanism_ = mechanism;
        score_ = score;
        kind_ = kind;
        duration_ = std::max<uint32_t>(duration, 1);
        variant_ = variant;
        direction_ = ((seed & 1U) ^ (variant & 1U)) ? 1.f : -1.f;
        recorded_ = recorded;
    }
    SoundPose Apply(SoundPose pose, uint32_t time) const {
        if (time >= duration_ || kind_ == SoundKind::kAmbient)
            return pose;
        const bool dino = mechanism_ == PetMechanism::kDinosaur;
        const bool tail = mechanism_ == PetMechanism::kTail;
        // Endpoints have zero position, velocity and acceleration contribution.
        // Short sounds still get a readable gesture, without a fast extra loop.
        const float ramp = std::min<float>(dino ? 1000 : 700, duration_ * .4f);
        const float window =
            SettlingProgress(time / ramp) * SettlingProgress((duration_ - time) / ramp);
        const float p = float(time) / duration_;
        const float energy = Energy(time, dino ? 450 : 300);
        const float strength = kind_ == SoundKind::kSleep  ? .14f + .08f * energy
                               : kind_ == SoundKind::kChew ? .30f + .25f * energy
                               : tail                      ? .90f + .10f * energy
                                                           : .72f + .28f * energy;
        const float amount = window * strength;
        constexpr float pi = 3.14159265358979323846f;
        const float cycles = std::clamp(duration_ / (dino ? 3400.f : 4100.f), .75f, 4.f);
        const bool answering = variant_ >= 16;
        const float phase =
            2 * pi * cycles * p + float(variant_ % 4) * .42f + (answering ? .85f : 0.f);
        const float side = direction_ * std::sin(phase);
        const float arch = std::cos(phase);
        const float look = Look(p, (variant_ / 4 + (answering ? 1 : 0)) % 4) * direction_;
        const unsigned neck_style = variant_ % 4, tail_style = (variant_ / 4) % 4;
        SoundPose offset{};
        if (dino) {
            if (reference_count_ && (kind_ != SoundKind::kRoar || recorded_)) {
                auto reference = Reference(time);
                reference[0] = DinosaurContentTilt(reference[0], kind_);
                const float mix = (recorded_ ? .34f : .52f) * amount;
                for (unsigned a = 0; a < 5; ++a)
                    pose[a] += (reference[a] - pose[a]) * mix;
            }
            if (kind_ != SoundKind::kRoar) {
                // Wide arc, two-axis orbit, S reach, or an inviting approach.
                offset[0] = neck_style == 0   ? -18 * arch
                            : neck_style == 1 ? 24 * std::sin(phase)
                            : neck_style == 2 ? 16 * arch
                                              : 28 * std::sin(pi * p);
                offset[1] = (neck_style == 0   ? 36 * look
                             : neck_style == 1 ? 54 * direction_ * arch
                             : neck_style == 2 ? 50 * side
                                               : 36 * look);
                // Preserve physical axis polarity. Do not reverse an axis based
                // on a model's world-space rotation or another product's pins.
                const float focus = variant_ % 3 == 0   ? 90.f
                                    : variant_ % 3 == 1 ? 90.f - .40f * (pose[1] - 90)
                                                        : 90.f + 52 * look;
                pose[2] += (focus - pose[2]) * .10f * amount;
                offset[2] = 38 * look;
                if (kind_ == SoundKind::kChew) {
                    // A low peck, sideways graze or sniff-and-take, rather than
                    // the upward nod used by curious/listening performances.
                    offset[0] = 5 * arch;
                    offset[1] *= .50f;
                    const float food_side = answering ? -look : side;
                    pose[2] += (90 + 28 * food_side - pose[2]) * .55f * amount;
                } else if (answering) {
                    // Second bank reaches across then checks back; it changes
                    // the spatial phrase, not just the gain of the same orbit.
                    offset[0] = -12 + 26 * arch;
                    offset[1] = 40 * look;
                    pose[2] += (90 - 40 * side - pose[2]) * .35f * amount;
                }
            } else {
                // The paired player keeps the front-facing lift as one shared
                // segment; these deliberate looks unfold only after that lift.
                offset[0] = -12 * energy;
                if (recorded_) {
                    offset[1] = 20 * look;
                    offset[2] = 38 * look;
                }
            }
            if (tail_style == 0) {
                offset[3] = 20 * arch;
                offset[4] = 68 * side;
            } else if (tail_style == 1) {
                offset[3] = 34 * std::sin(phase + .65f);
                offset[4] = 66 * direction_ * std::cos(phase + .65f);
            } else if (tail_style == 2) {
                offset[3] = 36 + 12 * arch;
                offset[4] = 28 * side;
            } else {
                offset[3] = -16 + 18 * arch;
                offset[4] = -.45f * (pose[1] - 90) + 26 * side;
            }
            if (kind_ == SoundKind::kRoar) {
                // Paired calls also stand tall. Keep the recorded left/right
                // motion as the main path, with a broad, readable tail crown.
                const float crown = 140 + 8 * tail_style + (answering ? 8 * arch : 0);
                pose[3] += (crown - pose[3]) * .78f * amount;
                offset[4] = recorded_ ? (answering ? 52 * look : -44 * side) : 0;
            }
        } else {
            // Panda head is yaw only: look, hold, check, return. The head never
            // bobs to the loudness envelope. Different styles keep the original
            // recording's character while reducing rapid recorder reversals.
            if (!tail)
                pose[0] += (90 + 60 * look - pose[0]) * .84f * amount;
            if (tail) {
                auto goal = TailPhrase(p, variant_, direction_, duration_);
                if (kind_ == SoundKind::kSleep) {
                    goal = {90 + 18 * look, 90 + 18 * side, 125 + 8 * arch};
                } else if (kind_ == SoundKind::kChew) {
                    goal[0] = 90 + (goal[0] - 90) * .65f;
                    goal[2] = 150 + (goal[2] - 150) * .70f;
                }
                // Deliberate head lead, high flag, radar, peek, ribbon, hug,
                // question/answer, stretch and recoil/greet. No fixed sine wag.
                const float mixes[] = {.94f, .76f, .82f};
                for (unsigned a = 0; a < 3; ++a)
                    pose[a] += (goal[a] - pose[a]) * mixes[a] * amount;
            } else if (kind_ != SoundKind::kSteps) {
                // Leave crawling traction exactly to the authored gait. Other
                // sounds offer one paw, change paws, tuck, or open both to greet.
                const float offer = .5f + .5f * std::cos(phase);
                const float answer = .5f + .5f * std::sin(phase);
                offset[1] = neck_style == 0   ? -38 * offer
                            : neck_style == 1 ? 14 * answer
                            : neck_style == 2 ? -38 * offer
                                              : -30 * offer;
                offset[2] = neck_style == 0   ? 10 * answer
                            : neck_style == 1 ? 38 * offer
                            : neck_style == 2 ? 38 * answer
                                              : 30 * offer;
                if (tail_style == 1) {
                    const float left = offset[1];
                    offset[1] = -offset[2] * .78f;
                    offset[2] = -left * 1.12f;
                } else if (tail_style == 2) {
                    offset[1] = (18 + 4 * neck_style) * offer;
                    offset[2] = (30 + 3 * neck_style) * answer;
                } else if (tail_style == 3) {
                    offset[1] = -(34.f + 2.f * neck_style) * offer;
                    offset[2] = (24 + 4 * neck_style) * (.65f * offer + .35f * answer);
                }
                if (answering) {
                    // Salute, exchange paws, open for a hug, then a shy cheek
                    // tuck. The left and right shoulders have opposite signs.
                    const float greet = .5f + .5f * look * direction_;
                    const SoundPose goals[] = {
                        {0, 18 + 28 * answer, 90 + 16 * offer, 0, 0},
                        {0, 90 - 64 * greet, 90 + 64 * (1 - greet), 0, 0},
                        {0, 24 + 20 * offer, 156 - 20 * answer, 0, 0},
                        {0, 134 - 34 * answer, 132 + 30 * offer, 0, 0},
                    };
                    for (unsigned a = 1; a < 3; ++a)
                        pose[a] += (goals[neck_style][a] - pose[a]) * .68f * amount;
                    offset[1] *= .30f;
                    offset[2] *= .30f;
                }
            }
        }
        const SoundPose low = dino   ? SoundPose{0, 0, 0, 55, 0}
                              : tail ? SoundPose{kPandaHeadMin, 6, 10, 0, 0}
                                     : SoundPose{kPandaHeadMin, 8, 8, 0, 0};
        const SoundPose high = dino   ? SoundPose{180, 180, 180, 180, 180}
                               : tail ? SoundPose{kPandaHeadMax, 174, 178, 0, 0}
                                      : SoundPose{kPandaHeadMax, 172, 172, 0, 0};
        for (unsigned a = 0; a < (dino ? 5U : 3U); ++a) {
            // Soft headroom avoids a long hard-clamped plateau at an endpoint.
            const float delta = offset[a] * amount;
            const float room = std::max(0.f, delta < 0 ? pose[a] - low[a] : high[a] - pose[a]);
            pose[a] +=
                std::copysign(room * (1 - std::exp(-std::abs(delta) / std::max(1.f, room))), delta);
            pose[a] = std::clamp(pose[a], low[a], high[a]);
        }
        return pose;
    }

private:
    struct Landmark {
        SoundPose pose{};
        uint32_t end = 0;
    };
    std::array<Landmark, 16> reference_{};
    size_t reference_count_ = 0;
    uint32_t reference_ms_ = 0, duration_ = 1;
    PetMechanism mechanism_ = PetMechanism::kCrawl;
    const SoundScore* score_ = nullptr;
    SoundKind kind_ = SoundKind::kCall;
    unsigned variant_ = 0;
    float direction_ = 1;
    bool recorded_ = false;
    static float Look(float p, unsigned style) {
        constexpr float points[][5] = {{0, .9f, .9f, -.55f, 0},
                                       {0, -.75f, .85f, .85f, 0},
                                       {0, .95f, .25f, -.9f, 0},
                                       {0, .45f, -.85f, .65f, 0}};
        const float at = std::clamp(p * 4, 0.f, 3.99999f);
        const auto i = static_cast<unsigned>(at);
        return points[style][i] +
               (points[style][i + 1] - points[style][i]) * SettlingProgress(at - i);
    }
    float Energy(uint32_t time, int width) const {
        if (!score_ || !score_->count)
            return .6f;
        // Smooth, continuous interpolation of measured PCM; compressed bytes
        // and packet lengths have no part in this envelope.
        float sum = 0, weight_sum = 0;
        const int bin = time / 100;
        for (int i = std::max(0, bin - width / 100); i <= bin + width / 100 + 1; ++i) {
            const float distance = std::abs(i * 100.f + 50 - time) / width;
            if (distance >= 1)
                continue;
            const float weight = .5f + .5f * std::cos(distance * 3.14159265359f);
            sum += (i < static_cast<int>(score_->count) ? score_->envelope[i] / 255.f : 0) * weight;
            weight_sum += weight;
        }
        return weight_sum > 0 ? sum / weight_sum : 0;
    }
    SoundPose Reference(uint32_t time) const {
        const uint32_t at = uint64_t(time) * reference_ms_ / duration_;
        SoundPose from{70, 90, 90, 90, 90}, previous = from;
        uint32_t start = 0;
        for (size_t i = 0; i < reference_count_; ++i) {
            const auto& f = reference_[i];
            if (at < f.end) {
                const auto& next = reference_[std::min(i + 1, reference_count_ - 1)].pose;
                SoundPose out{};
                const uint32_t dt = f.end - start;
                const uint32_t before = i ? start - (i > 1 ? reference_[i - 2].end : 0) : dt;
                const uint32_t after =
                    i + 1 < reference_count_ ? reference_[i + 1].end - f.end : dt;
                for (unsigned a = 0; a < 5; ++a)
                    out[a] = MotionHermite(previous[a], from[a], f.pose[a], next[a], before, dt,
                                           after, float(at - start) / dt);
                return out;
            }
            previous = from;
            from = f.pose;
            start = f.end;
        }
        return from;
    }
};
}  // namespace wzh
#endif
