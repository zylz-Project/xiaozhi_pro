#ifndef WZH_PET_BEHAVIOR_H_
#define WZH_PET_BEHAVIOR_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string_view>

#include "wzh_interaction.h"
#include "wzh_motion_profile.h"

namespace wzh {

enum class PetControl { kNone, kStop, kQuiet, kRest, kWake };
enum class PetMode { kNormal, kQuiet, kRest, kStopped };
enum class PetPriority : uint8_t { kSensor, kEmotion, kExplicit };
enum class PetReaction {
    kNone,
    kHappy,
    kAffection,
    kComfort,
    kCurious,
    kProud,
    kEat,
    kStartled,
    kSleepy
};
enum class TouchResponse { kNone, kAffection, kPlayful, kHeld, kReleased };

struct TouchEvent {
    TouchResponse response = TouchResponse::kNone;
    uint8_t mask = 0;
    PetInteraction interaction() const {
        if (response == TouchResponse::kNone)
            return PetInteraction::kNone;
        if (response == TouchResponse::kHeld)
            return PetInteraction::kHeld;
        if (response == TouchResponse::kReleased)
            return PetInteraction::kReleased;
        if (response == TouchResponse::kPlayful)
            return PetInteraction::kPlayful;
        if (mask == 1)
            return PetInteraction::kTouch1;
        if (mask == 2)
            return PetInteraction::kTouch2;
        if (mask == 4)
            return PetInteraction::kTouch3;
        return PetInteraction::kTouch4;
    }
};

inline bool ContainsAny(std::string_view text, std::initializer_list<std::string_view> words) {
    for (auto word : words) {
        if (text.find(word) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

// Conservative local fallback, not a replacement for server-side semantics.
// Negation is scoped to the current clause and a short prefix of the matched word.
inline bool PositiveMention(std::string_view text, std::string_view word) {
    size_t position = 0;
    while ((position = text.find(word, position)) != std::string_view::npos) {
        const auto prefix = text.substr(0, position);
        size_t clause = 0;
        for (auto separator :
             {std::string_view("，"), std::string_view("。"), std::string_view("！"),
              std::string_view("？"), std::string_view(","), std::string_view(";"),
              std::string_view("；")}) {
            auto boundary = prefix.rfind(separator);
            if (boundary != std::string_view::npos) {
                clause = std::max(clause, boundary + separator.size());
            }
        }
        const size_t start = std::max(clause, position > 24 ? position - 24 : size_t(0));
        if (!ContainsAny(text.substr(start, position - start),
                         {"不", "别", "无需", "禁止", "停止", "取消"})) {
            return true;
        }
        position += word.size();
    }
    return false;
}

inline PetControl ParsePetControl(std::string_view text) {
    if (text == "stop" ||
        ContainsAny(text, {"停下", "停止动作", "别动", "不要动", "别摇", "不要摇", "别转", "不要转",
                           "别抬", "别挥", "不要挥", "别爬", "不要爬"})) {
        return PetControl::kStop;
    }
    if (text == "wake" || PositiveMention(text, "醒醒") || PositiveMention(text, "起来玩") ||
        PositiveMention(text, "恢复活动")) {
        return PetControl::kWake;
    }
    if (text == "quiet" || PositiveMention(text, "安静")) {
        return PetControl::kQuiet;
    }
    if (text == "rest" || text == "sleepy" || PositiveMention(text, "晚安") ||
        PositiveMention(text, "睡觉") || PositiveMention(text, "休息吧")) {
        return PetControl::kRest;
    }
    return PetControl::kNone;
}

inline PetReaction ReactionFromText(std::string_view text) {
    // Specific negative affect precedes its positive substring: 不开心 contains 开心.
    if (ContainsAny(text, {"不开心", "不高兴", "难过", "伤心", "想哭", "委屈", "难受", "孤单"})) {
        return PetReaction::kComfort;
    }
    for (auto word : {"开心", "高兴", "太棒", "真棒", "恭喜", "满分", "太好了", "哈哈"}) {
        if (PositiveMention(text, word)) {
            return PetReaction::kHappy;
        }
    }
    for (auto word : {"喜欢你", "爱你", "想你", "抱抱", "谢谢", "亲亲"}) {
        if (PositiveMention(text, word)) {
            return PetReaction::kAffection;
        }
    }
    if (ContainsAny(text, {"生气", "气死", "烦恼", "害怕", "担心"})) {
        return PetReaction::kComfort;
    }
    for (auto word : {"吃饭", "吃东西", "好吃", "饿了", "好饿"}) {
        if (PositiveMention(text, word)) {
            return PetReaction::kEat;
        }
    }
    if (ContainsAny(text, {"想一想", "想想", "好奇", "为什么"})) {
        return PetReaction::kCurious;
    }
    if (ContainsAny(text, {"吓到", "震惊", "没想到"})) {
        return PetReaction::kStartled;
    }
    if (ContainsAny(text, {"骄傲", "自豪", "冠军", "第一名"})) {
        return PetReaction::kProud;
    }
    if (ContainsAny(text, {"困了", "好困", "好累"})) {
        return PetReaction::kSleepy;
    }
    return PetReaction::kNone;
}

inline bool RecentAudioOutput(int64_t now_us, int64_t updated_us) {
    return updated_us > 0 && now_us >= updated_us && now_us - updated_us < 200000;
}

inline float ExpressiveAngle(float angle, float center, float low, float high, float gain = 1.25f) {
    return std::clamp(center + (angle - center) * gain, low, high);
}

inline uint32_t PetFrameDuration(float max_travel, uint32_t requested) {
    return MotionFrameDuration(max_travel, requested);
}

// Only a fresh arrival after a stable absence may greet. Startup levels, busy
// periods and cooldown must not turn into a delayed unsolicited performance.
class PresenceGreeting {
public:
    bool Update(bool high, uint32_t now, bool ready) {
        if (high != candidate_) {
            candidate_ = high;
            samples_ = 1;
        } else {
            samples_ = std::min(samples_ + 1, 10);
        }
        const bool rising = samples_ >= 10 && high && !present_;
        if (samples_ >= 10 && high != present_) {
            present_ = high;
        }
        if (now < 10000) {
            armed_ = absent_ = pending_ = false;
            return false;
        }
        if (!present_ && !candidate_) {
            if (!absent_) {
                absent_ = true;
                absent_since_ = now;
            }
            if (now - absent_since_ >= 3000)
                armed_ = true;
        } else {
            absent_ = false;
        }
        if (rising) {
            pending_ = armed_ && ready && (!greeted_ || now - last_greeting_ >= 60000);
            pending_since_ = now;
            armed_ = false;
        }
        if (!present_ || !ready || (pending_ && now - pending_since_ >= 1000))
            pending_ = false;
        return pending_;
    }
    void Accepted(uint32_t now) {
        greeted_ = true;
        pending_ = false;
        last_greeting_ = now;
    }
    bool present() const { return present_; }

private:
    bool candidate_ = false, present_ = false, pending_ = false, greeted_ = false;
    bool armed_ = false, absent_ = false;
    int samples_ = 0;
    uint32_t last_greeting_ = 0;
    uint32_t absent_since_ = 0, pending_since_ = 0;
};

// Pure, host-testable policy. Controllers own all PWM, sound and application effects.
class PetBehavior {
public:
    struct State {
        PetMode mode;
        bool motion;
        bool local_sound;
        bool gentle;
    };

    State Get(uint32_t now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool gentle = gentle_ && now - gentle_since_ < gentle_ms_;
        const bool motion = !maintenance_ && mode_ != PetMode::kStopped && mode_ != PetMode::kRest;
        const bool normal = mode_ == PetMode::kNormal && !maintenance_;
        return {mode_, motion, normal && !gentle, gentle};
    }

    void Control(PetControl control, uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (control == PetControl::kNone) {
            return;
        }
        mode_ = control == PetControl::kStop    ? PetMode::kStopped
                : control == PetControl::kRest  ? PetMode::kRest
                : control == PetControl::kQuiet ? PetMode::kQuiet
                                                : PetMode::kNormal;
        priority_active_ = false;
        priority_since_ = now;
        gentle_ = false;
        ResetTouchLocked();
    }

    void SetMaintenance(bool maintenance) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (maintenance_ != maintenance) {
            ResetTouchLocked();
            gentle_ = false;
        }
        maintenance_ = maintenance;
    }

    void Gentle(uint32_t now, uint32_t duration = 5000) {
        std::lock_guard<std::mutex> lock(mutex_);
        gentle_ = true;
        gentle_since_ = now;
        gentle_ms_ = duration;
    }

    // Only actual interaction can start a response. Cooldowns never schedule one.
    bool SensorReady(uint32_t now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !maintenance_ && mode_ != PetMode::kRest && mode_ != PetMode::kStopped &&
               (!sensor_responded_ || now - sensor_since_ >= 3000);
    }

    void SensorAccepted(uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        sensor_responded_ = true;
        sensor_since_ = now;
    }

    bool CanPlayLocalSound(uint32_t now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool gentle = gentle_ && now - gentle_since_ < gentle_ms_;
        return !maintenance_ && mode_ == PetMode::kNormal && !gentle &&
               (!sound_started_ || now - sound_since_ >= 12000);
    }

    void LocalSoundStarted(uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        sound_started_ = true;
        sound_since_ = now;
    }

    // A short acknowledgement of a deliberate interaction has its own budget.
    // Held/picked-up pets retain audible feedback while motion stays gentle.
    bool CanPlayInteractionSound(uint32_t now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !maintenance_ && mode_ == PetMode::kNormal &&
               (!interaction_sound_started_ || now - interaction_sound_since_ >= 650);
    }

    void InteractionSoundStarted(uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        interaction_sound_started_ = true;
        interaction_sound_since_ = now;
    }

    void ClearGentle() {
        std::lock_guard<std::mutex> lock(mutex_);
        gentle_ = false;
    }

    bool Offer(PetPriority priority, uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (maintenance_ || mode_ == PetMode::kRest || mode_ == PetMode::kStopped) {
            return false;
        }
        if (priority_active_ && now - priority_since_ < priority_ms_ && priority < priority_) {
            return false;
        }
        if (priority == PetPriority::kEmotion && emotion_responded_ &&
            now - emotion_since_ < 1800) {
            return false;
        }
        if (priority == PetPriority::kEmotion) {
            emotion_responded_ = true;
            emotion_since_ = now;
        }
        priority_ = priority;
        priority_since_ = now;
        priority_ms_ = priority == PetPriority::kExplicit ? 2500 : 1200;
        priority_active_ = true;
        return true;
    }

    void Touch(uint8_t mask, uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if ((mask & 0xF0) != 0) {
            // Invalid/disconnected touch input cancels contact; it is neither
            // a petting hold nor a deliberate release gesture.
            ResetTouchLocked();
            return;
        }
        if (mask == touch_mask_) {
            return;
        }
        const uint8_t previous = touch_mask_;
        touch_mask_ = mask;
        if (maintenance_ || mode_ == PetMode::kRest || mode_ == PetMode::kStopped) {
            pending_touch_ = TouchResponse::kNone;
            return;
        }
        if (mask == 0) {
            if (touch_held_) {
                pending_touch_ = TouchResponse::kReleased;
            }
            touch_held_ = false;
        } else if (previous == 0) {
            taps_ = mask == last_tap_mask_ && now - last_tap_ <= 1400 ? std::min(taps_ + 1, 3) : 1;
            last_tap_mask_ = mask;
            last_tap_ = touch_since_ = now;
            touch_held_ = false;
            pending_touch_ = taps_ >= 3 || (mask & (mask - 1)) != 0 ? TouchResponse::kPlayful
                                                                    : TouchResponse::kAffection;
            if (pending_touch_ == TouchResponse::kPlayful) {
                // Consume the gesture. A fourth tap starts a new interaction instead
                // of repeatedly restarting the long playful response.
                taps_ = 0;
            }
        }
        // Contact stays active but the decoded pad code drifted: only update
        // touch_mask_. Interactions start on the 0 -> nonzero rising edge, so a
        // resting finger cannot keep retriggering responses through R-2R noise.
    }

    TouchEvent PollTouchEvent(uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (maintenance_ || mode_ == PetMode::kRest || mode_ == PetMode::kStopped) {
            return {};
        }
        if (touch_mask_ != 0 && now - touch_since_ >= 1200 && !touch_held_) {
            touch_held_ = true;
            gentle_ = true;
            gentle_since_ = now;
            gentle_ms_ = 4000;
            held_beat_ms_ = now;
            pending_touch_ = TouchResponse::kHeld;
        }
        // Continued contact extends gentleness without repeatedly restarting a
        // clip, but a long hold still gets a soft acknowledgement every few
        // seconds so the pet does not look frozen under a resting hand.
        if (touch_held_ && touch_mask_ != 0) {
            gentle_since_ = now;
            if (pending_touch_ == TouchResponse::kNone && now - held_beat_ms_ >= 9000) {
                held_beat_ms_ = now;
                pending_touch_ = TouchResponse::kHeld;
            }
        }
        const auto response = pending_touch_;
        pending_touch_ = TouchResponse::kNone;
        return {response, touch_mask_};
    }

    TouchResponse PollTouch(uint32_t now) { return PollTouchEvent(now).response; }

private:
    void ResetTouchLocked() {
        touch_mask_ = 0;
        touch_held_ = false;
        held_beat_ms_ = 0;
        taps_ = 0;
        last_tap_mask_ = 0;
        pending_touch_ = TouchResponse::kNone;
    }
    mutable std::mutex mutex_;
    PetMode mode_ = PetMode::kNormal;
    bool maintenance_ = false;
    bool gentle_ = false;
    bool sensor_responded_ = false, sound_started_ = false;
    uint32_t sensor_since_ = 0, sound_since_ = 0;
    bool interaction_sound_started_ = false;
    uint32_t interaction_sound_since_ = 0;
    uint32_t gentle_since_ = 0, gentle_ms_ = 0;
    uint32_t held_beat_ms_ = 0;
    bool priority_active_ = false;
    bool emotion_responded_ = false;
    uint32_t emotion_since_ = 0;
    PetPriority priority_ = PetPriority::kSensor;
    uint32_t priority_since_ = 0, priority_ms_ = 0;
    uint8_t touch_mask_ = 0;
    uint8_t last_tap_mask_ = 0;
    bool touch_held_ = false;
    int taps_ = 0;
    uint32_t touch_since_ = 0, last_tap_ = 0;
    TouchResponse pending_touch_ = TouchResponse::kNone;
};

}  // namespace wzh
#endif  // WZH_PET_BEHAVIOR_H_
