#include "panda_controller.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include "config.h"
#include "mcp_server.h"
#include "panda_audio.h"

#define TAG "PandaMotion"

namespace {
constexpr uint32_t kTickMs = 20;
constexpr int kServoMaxDuty = (1 << 13) - 1;
constexpr int kServoPeriodUs = 20000;
// 数组顺序必须与三路舵机顺序一致：头、左臂、右臂。
constexpr gpio_num_t kServoPins[] = {SERVO_HEAD_GPIO, SERVO_LEFT_ARM_GPIO, SERVO_RIGHT_ARM_GPIO};
constexpr ledc_channel_t kServoChannels[] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2};

int irnd(int count) { return count > 0 ? static_cast<int>(esp_random() % static_cast<uint32_t>(count)) : 0; }
int irand(int low, int high) { return low + irnd(high - low + 1); }
float frand(float low, float high) {
    return low + (static_cast<float>(esp_random()) / static_cast<float>(UINT32_MAX)) * (high - low);
}
float ease_motion(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

using wzh_panda::Behaviour;
using wzh_panda::Clip;
using wzh_panda::Keyframe;
using wzh_panda::Pose;
using wzh_panda::ShuffleBag;

constexpr size_t kBehaviourCount = static_cast<size_t>(Behaviour::kCount);

uint32_t AngleToDuty(int angle) {
    // 常见 180 度舵机使用 20 ms 周期和约 0.5～2.5 ms 脉宽。
    angle = std::clamp(angle, 0, 180);
    const int pulse_us = 500 + angle * 2000 / 180;
    return pulse_us * (kServoMaxDuty + 1) / kServoPeriodUs;
}

Pose TargetPose(const Clip& clip, uint8_t frame, bool mirror) {
    const Keyframe& key = clip.frames[frame];
    Pose pose{static_cast<float>(key.head), static_cast<float>(key.left_arm),
              static_cast<float>(key.right_arm)};
    if (mirror && clip.mirrorable) {
        pose.head = 180.0f - pose.head;
        const float old_left = pose.left_arm;
        pose.left_arm = 180.0f - pose.right_arm;
        pose.right_arm = 180.0f - old_left;
    }
    return pose;
}

// ============================================================================
// 关键帧动作库（移植自旧工程 crawlPanda 的 living-motion 引擎）。
// Keyframe 顺序为 duration_ms / head / left_arm / right_arm。
// head: 0=右 90=前 180=左；left_arm: 15，爬行方向 180°→0°（角度减小）；
// right_arm: 16，爬行方向 0°→180°（角度增大）。默认坐姿 90/90/90。
// ============================================================================
constexpr Keyframe kIdleListen[] = {
    {650, 136, 90, 90}, {500, 148, 90, 90}, {720, 126, 72, 90}, {900, 102, 88, 104},
    {900, 90, 90, 90}};
constexpr Keyframe kIdlePawAdjust[] = {
    {700, 54, 90, 90}, {700, 66, 58, 92}, {650, 118, 82, 128}, {900, 104, 100, 84},
    {850, 90, 90, 90}};
constexpr Keyframe kIdleBreath[] = {
    {1100, 102, 82, 98}, {1250, 78, 94, 86}, {1150, 110, 76, 105}, {1300, 90, 90, 90}};
constexpr Keyframe kIdleStretch[] = {
    {900, 138, 132, 48}, {1000, 150, 162, 22}, {950, 76, 42, 142}, {1100, 62, 22, 164},
    {1200, 90, 90, 90}};
constexpr Keyframe kCuriousPeek[] = {
    {380, 146, 90, 90}, {620, 156, 90, 90}, {720, 150, 42, 94}, {430, 54, 28, 102},
    {700, 128, 84, 152}, {1000, 90, 90, 90}};
constexpr Keyframe kCuriousReach[] = {
    {600, 38, 90, 90}, {750, 52, 22, 92}, {620, 142, 48, 138}, {800, 128, 86, 166},
    {1000, 90, 90, 90}};
constexpr Keyframe kCuriousDoubleCheck[] = {
    {440, 142, 78, 90}, {520, 154, 50, 92}, {420, 42, 82, 128}, {620, 54, 90, 166},
    {760, 124, 36, 116}, {1000, 90, 90, 90}};
constexpr Keyframe kEatHoldBamboo[] = {
    {750, 44, 62, 118}, {850, 132, 30, 150}, {520, 58, 24, 146}, {580, 128, 38, 158},
    {700, 72, 48, 136}, {1000, 90, 90, 90}};
constexpr Keyframe kEatChew[] = {
    {650, 132, 36, 144}, {360, 54, 30, 148}, {470, 124, 42, 156}, {340, 66, 28, 146},
    {520, 142, 54, 164}, {950, 90, 90, 90}};
constexpr Keyframe kEatPawSwap[] = {
    {700, 46, 22, 132}, {620, 132, 68, 168}, {520, 58, 18, 154}, {650, 142, 54, 140},
    {760, 112, 34, 158}, {1000, 90, 90, 90}};
constexpr Keyframe kBabyCall[] = {
    {420, 34, 86, 92}, {380, 148, 88, 94}, {600, 128, 36, 98}, {500, 52, 74, 158},
    {700, 136, 20, 168}, {950, 90, 90, 90}};
constexpr Keyframe kBabyPaddle[] = {
    {480, 138, 22, 92}, {480, 58, 74, 164}, {520, 128, 16, 118}, {520, 62, 64, 170},
    {700, 142, 30, 150}, {900, 90, 90, 90}};
constexpr Keyframe kBabyFindUser[] = {
    {650, 38, 90, 90}, {750, 52, 26, 92}, {620, 148, 72, 154}, {780, 138, 18, 170},
    {850, 112, 44, 140}, {1000, 90, 90, 90}};
constexpr Keyframe kProudPushUp[] = {
    {650, 42, 78, 102}, {750, 138, 36, 146}, {850, 154, 14, 168}, {650, 56, 22, 160},
    {850, 132, 52, 136}, {1050, 90, 90, 90}};
constexpr Keyframe kProudPatrol[] = {
    {780, 28, 20, 94}, {850, 46, 44, 154}, {780, 146, 86, 170}, {850, 158, 24, 128},
    {1050, 90, 90, 90}};
constexpr Keyframe kProudCall[] = {
    {550, 118, 82, 98}, {760, 154, 42, 142}, {900, 146, 16, 166}, {720, 44, 62, 158},
    {800, 132, 24, 124}, {1050, 90, 90, 90}};
constexpr Keyframe kAffectionHug[] = {
    {800, 142, 86, 94}, {900, 156, 48, 132}, {850, 146, 22, 160}, {900, 116, 34, 150},
    {1100, 90, 90, 90}};
constexpr Keyframe kAffectionShy[] = {
    {650, 36, 94, 88}, {850, 52, 134, 52}, {620, 142, 72, 142}, {850, 154, 30, 162},
    {1100, 90, 90, 90}};
constexpr Keyframe kAffectionAskUp[] = {
    {900, 132, 104, 76}, {950, 148, 142, 38}, {900, 126, 54, 142}, {850, 62, 24, 168},
    {1150, 90, 90, 90}};
constexpr Keyframe kPlayfulCrawl[] = {
    {520, 138, 18, 92}, {520, 54, 70, 166}, {540, 126, 12, 122}, {540, 62, 62, 172},
    {650, 144, 26, 150}, {900, 90, 90, 90}};
constexpr Keyframe kPlayfulPawDrum[] = {
    {480, 142, 28, 94}, {480, 52, 78, 158}, {500, 132, 18, 118}, {500, 58, 66, 170},
    {560, 122, 14, 166}, {900, 90, 90, 90}};
constexpr Keyframe kPlayfulPounce[] = {
    {700, 44, 138, 44}, {620, 132, 86, 96}, {620, 148, 18, 168}, {700, 54, 42, 148},
    {760, 124, 24, 166}, {950, 90, 90, 90}};
constexpr Keyframe kAmbientListen[] = {
    {1200, 42, 90, 90}, {1500, 56, 54, 94}, {1300, 146, 82, 142}, {1700, 132, 28, 160},
    {1600, 90, 90, 90}};
constexpr Keyframe kAmbientRain[] = {
    {1200, 132, 120, 62}, {1400, 48, 72, 128}, {1300, 118, 30, 152}, {1500, 62, 66, 168},
    {1500, 104, 22, 136}, {1600, 90, 90, 90}};
constexpr Keyframe kAmbientExplore[] = {
    {1300, 34, 40, 92}, {1450, 62, 78, 158}, {1300, 144, 86, 172}, {1450, 118, 22, 126},
    {1600, 90, 90, 90}};
constexpr Keyframe kAmbientStretch[] = {
    {1500, 122, 90, 90}, {1800, 146, 152, 30}, {1800, 132, 48, 132}, {1700, 54, 16, 168},
    {1800, 90, 90, 90}};
constexpr Keyframe kSettle[] = {{750, 102, 78, 102}, {1050, 94, 88, 92}, {900, 90, 90, 90}};

enum ClipId {
    C_IDLE_LISTEN,
    C_IDLE_PAW_ADJUST,
    C_IDLE_BREATH,
    C_IDLE_STRETCH,
    C_CURIOUS_PEEK,
    C_CURIOUS_REACH,
    C_CURIOUS_DOUBLE_CHECK,
    C_EAT_HOLD,
    C_EAT_CHEW,
    C_EAT_PAW_SWAP,
    C_BABY_CALL,
    C_BABY_PADDLE,
    C_BABY_FIND_USER,
    C_PROUD_PUSH_UP,
    C_PROUD_PATROL,
    C_PROUD_CALL,
    C_AFFECTION_HUG,
    C_AFFECTION_SHY,
    C_AFFECTION_ASK_UP,
    C_PLAYFUL_CRAWL,
    C_PLAYFUL_PAW_DRUM,
    C_PLAYFUL_POUNCE,
    C_AMBIENT_LISTEN,
    C_AMBIENT_RAIN,
    C_AMBIENT_EXPLORE,
    C_AMBIENT_STRETCH,
    C_SETTLE,
    C_COUNT,
};

constexpr Clip kClips[C_COUNT] = {
    {"idle_listen", kIdleListen, std::size(kIdleListen), true},
    {"idle_paw_adjust", kIdlePawAdjust, std::size(kIdlePawAdjust), true},
    {"idle_breath", kIdleBreath, std::size(kIdleBreath), true},
    {"idle_stretch", kIdleStretch, std::size(kIdleStretch), true},
    {"curious_peek", kCuriousPeek, std::size(kCuriousPeek), true},
    {"curious_reach", kCuriousReach, std::size(kCuriousReach), true},
    {"curious_double_check", kCuriousDoubleCheck, std::size(kCuriousDoubleCheck), true},
    {"eat_hold_bamboo", kEatHoldBamboo, std::size(kEatHoldBamboo), true},
    {"eat_chew", kEatChew, std::size(kEatChew), true},
    {"eat_paw_swap", kEatPawSwap, std::size(kEatPawSwap), true},
    {"baby_call", kBabyCall, std::size(kBabyCall), true},
    {"baby_paddle", kBabyPaddle, std::size(kBabyPaddle), true},
    {"baby_find_user", kBabyFindUser, std::size(kBabyFindUser), true},
    {"proud_push_up", kProudPushUp, std::size(kProudPushUp), true},
    {"proud_patrol", kProudPatrol, std::size(kProudPatrol), true},
    {"proud_call", kProudCall, std::size(kProudCall), true},
    {"affection_hug", kAffectionHug, std::size(kAffectionHug), true},
    {"affection_shy", kAffectionShy, std::size(kAffectionShy), true},
    {"affection_ask_up", kAffectionAskUp, std::size(kAffectionAskUp), true},
    {"playful_crawl", kPlayfulCrawl, std::size(kPlayfulCrawl), true},
    {"playful_paw_drum", kPlayfulPawDrum, std::size(kPlayfulPawDrum), true},
    {"playful_pounce", kPlayfulPounce, std::size(kPlayfulPounce), true},
    {"ambient_listen", kAmbientListen, std::size(kAmbientListen), true},
    {"ambient_rain", kAmbientRain, std::size(kAmbientRain), true},
    {"ambient_explore", kAmbientExplore, std::size(kAmbientExplore), true},
    {"ambient_stretch", kAmbientStretch, std::size(kAmbientStretch), true},
    {"settle", kSettle, std::size(kSettle), false},
};

constexpr int8_t kFamily[kBehaviourCount][4] = {
    {C_IDLE_LISTEN, C_IDLE_PAW_ADJUST, C_IDLE_BREATH, C_IDLE_STRETCH},
    {C_CURIOUS_PEEK, C_CURIOUS_REACH, C_CURIOUS_DOUBLE_CHECK, -1},
    {C_EAT_HOLD, C_EAT_CHEW, C_EAT_PAW_SWAP, -1},
    {C_BABY_CALL, C_BABY_PADDLE, C_BABY_FIND_USER, -1},
    {C_PROUD_PUSH_UP, C_PROUD_PATROL, C_PROUD_CALL, -1},
    {C_AFFECTION_HUG, C_AFFECTION_SHY, C_AFFECTION_ASK_UP, -1},
    {C_PLAYFUL_CRAWL, C_PLAYFUL_PAW_DRUM, C_PLAYFUL_POUNCE, -1},
    {C_AMBIENT_LISTEN, C_AMBIENT_RAIN, C_AMBIENT_EXPLORE, C_AMBIENT_STRETCH},
    {C_SETTLE, -1, -1, -1},
};

// ============================================================================
// 身体部位级动作剪辑：从当前姿态起手，结束时回到默认姿态 90/90/90。
// 方向性动作（看左/看右）不可镜像，避免反向。
// ============================================================================
constexpr Keyframe kBodyShakeHead[] = {
    {140, 60, 90, 90}, {140, 120, 90, 90}, {140, 60, 90, 90}, {140, 120, 90, 90},
    {320, 90, 90, 90}};
constexpr Keyframe kBodyLookLeft[] = {{300, 140, 90, 90}, {600, 140, 90, 90}, {400, 90, 90, 90}};
constexpr Keyframe kBodyLookRight[] = {{300, 40, 90, 90}, {600, 40, 90, 90}, {400, 90, 90, 90}};
constexpr Keyframe kBodyWaveLeft[] = {
    {200, 90, 40, 90}, {200, 90, 130, 90}, {200, 90, 40, 90}, {200, 90, 130, 90},
    {320, 90, 90, 90}};
constexpr Keyframe kBodyWaveRight[] = {
    {200, 90, 90, 40}, {200, 90, 90, 130}, {200, 90, 90, 40}, {200, 90, 90, 130},
    {320, 90, 90, 90}};
constexpr Keyframe kBodyRaiseArms[] = {
    {350, 92, 45, 135}, {600, 92, 45, 135}, {350, 92, 90, 90}};

enum BodyClipId {
    BODY_SHAKE_HEAD,
    BODY_LOOK_LEFT,
    BODY_LOOK_RIGHT,
    BODY_WAVE_LEFT,
    BODY_WAVE_RIGHT,
    BODY_RAISE_ARMS,
    BODY_COUNT,
};
constexpr Clip kBodyClips[BODY_COUNT] = {
    {"body_shake_head", kBodyShakeHead, std::size(kBodyShakeHead), true},
    {"body_look_left", kBodyLookLeft, std::size(kBodyLookLeft), false},
    {"body_look_right", kBodyLookRight, std::size(kBodyLookRight), false},
    {"body_wave_left", kBodyWaveLeft, std::size(kBodyWaveLeft), true},
    {"body_wave_right", kBodyWaveRight, std::size(kBodyWaveRight), true},
    {"body_raise_arms", kBodyRaiseArms, std::size(kBodyRaiseArms), false},
};

bool Contains(const char* text, const char* token) {
    return text != nullptr && std::strstr(text, token) != nullptr;
}
}  // namespace

bool PandaController::Initialize(PandaAudio* audio) {
    audio_ = audio;
    InitializeServos();
    action_queue_ = xQueueCreate(4, sizeof(ActionRequest));
    if (action_queue_ == nullptr ||
        xTaskCreate(TaskEntry, "panda_motion", 5120, this, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start panda motion task");
        return false;
    }
    // The board singleton is still being constructed here, so defer local
    // audio until the application and AudioService are fully initialized.
    TriggerAction(PandaAction::kDiscover, false);
    return true;
}

void PandaController::InitializeServos() {
    // 与旧工程 servo.cc 一致：先给舵机电源，再初始化 50Hz/13bit PWM 通道。
    gpio_config_t power_config = {};
    power_config.pin_bit_mask = 1ULL << SERVO_POWER_GPIO;
    power_config.mode = GPIO_MODE_OUTPUT;
    power_config.pull_up_en = GPIO_PULLUP_DISABLE;
    power_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&power_config));
    ESP_ERROR_CHECK(gpio_set_level(SERVO_POWER_GPIO, 1));

    ledc_timer_config_t timer_config = {};
    timer_config.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_config.duty_resolution = SERVO_DUTY_RES;
    timer_config.timer_num = SERVO_TIMER;
    timer_config.freq_hz = SERVO_FREQ_HZ;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    for (size_t i = 0; i < std::size(kServoPins); ++i) {
        ledc_channel_config_t channel_config = {};
        channel_config.gpio_num = kServoPins[i];
        channel_config.speed_mode = LEDC_LOW_SPEED_MODE;
        channel_config.channel = kServoChannels[i];
        channel_config.timer_sel = SERVO_TIMER;
        channel_config.duty = AngleToDuty(90);
        channel_config.hpoint = 0;
        channel_config.flags = {.output_invert = 0};
        ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
    }
    SetServoAngles(SERVO_HEAD_DEFAULT, SERVO_LEFT_ARM_DEFAULT, SERVO_RIGHT_ARM_DEFAULT);
    ESP_LOGI(TAG, "Servos initialized");
}

void PandaController::SetServoAngle(int idx, int angle) {
    if (idx < 0 || idx >= static_cast<int>(std::size(kServoPins))) {
        return;
    }
    angle = std::clamp(angle, 0, 180);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, kServoChannels[idx], AngleToDuty(angle));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, kServoChannels[idx]);
}

void PandaController::SetServoAngles(int head, int left_arm, int right_arm) {
    SetServoAngle(0, head);
    SetServoAngle(1, left_arm);
    SetServoAngle(2, right_arm);
}

void PandaController::OutputPose(const wzh_panda::Pose& pose) {
    // 与旧工程 output_pose 一致：限位 8~172 后按 头/左臂/右臂 顺序输出。
    const int head = static_cast<int>(std::lround(std::clamp(pose.head, 8.0f, 172.0f)));
    const int left = static_cast<int>(std::lround(std::clamp(pose.left_arm, 8.0f, 172.0f)));
    const int right = static_cast<int>(std::lround(std::clamp(pose.right_arm, 8.0f, 172.0f)));
    SetServoAngles(head, left, right);
}

int PandaController::ChooseClip(Behaviour behaviour) {
    const size_t behaviour_index = static_cast<size_t>(behaviour);
    ShuffleBag& bag = bags_[behaviour_index];
    if (bag.position >= bag.count) {
        bag.count = 0;
        bag.position = 0;
        for (int i = 0; i < 4 && kFamily[behaviour_index][i] >= 0; ++i) {
            bag.items[bag.count++] = kFamily[behaviour_index][i];
        }
        for (int i = bag.count - 1; i > 0; --i) {
            std::swap(bag.items[i], bag.items[irnd(i + 1)]);
        }
        if (bag.count > 1 && bag.items[0] == bag.previous) {
            std::swap(bag.items[0], bag.items[1]);
        }
    }
    const int selected = bag.items[bag.position++];
    bag.previous = static_cast<int8_t>(selected);
    return selected;
}

void PandaController::StartClip(int clip_id, uint32_t now_ms) {
    // 常规行为剪辑在 kClips；身体部位剪辑从 C_COUNT 起编号，索引 kBodyClips。
    const Clip* clip = nullptr;
    if (clip_id >= 0 && clip_id < static_cast<int>(C_COUNT)) {
        clip = &kClips[clip_id];
    } else if (clip_id >= static_cast<int>(C_COUNT) &&
               clip_id < static_cast<int>(C_COUNT) + BODY_COUNT) {
        clip = &kBodyClips[clip_id - C_COUNT];
    }
    if (clip == nullptr) {
        ESP_LOGW(TAG, "Ignoring invalid clip id %d", clip_id);
        return;
    }
    from_ = pose_;
    clip_ = clip;
    frame_ = 0;
    frame_started_ms_ = now_ms;
    // 所有动作都带随机节奏，保证“灵动”而非生硬。
    tempo_ = frand(0.88f, 1.14f);
    frame_duration_ms_ = std::max<uint32_t>(180, static_cast<uint32_t>(clip->frames[0].duration_ms * tempo_));
    if (!clip->mirrorable) {
        mirror_ = false;
    } else {
        // 避免随机镜像造成长期单侧偏置。
        if (mirror_balance_ >= 2) {
            mirror_ = false;
        } else if (mirror_balance_ <= -2) {
            mirror_ = true;
        } else {
            mirror_ = irnd(2) != 0;
        }
        mirror_balance_ += mirror_ ? 1 : -1;
    }
    clip_active_ = true;
    ESP_LOGI(TAG, "clip=%s mirror=%d tempo=%.2f", clip->name, mirror_, static_cast<double>(tempo_));
}

void PandaController::UpdatePlayer(uint32_t now_ms) {
    if (!clip_active_ || clip_ == nullptr) {
        return;
    }
    while (clip_active_ && now_ms - frame_started_ms_ >= frame_duration_ms_) {
        pose_ = TargetPose(*clip_, frame_, mirror_);
        from_ = pose_;
        frame_started_ms_ += frame_duration_ms_;
        if (++frame_ >= clip_->count) {
            clip_active_ = false;
            return;
        }
        frame_duration_ms_ = std::max<uint32_t>(
            180, static_cast<uint32_t>(clip_->frames[frame_].duration_ms * tempo_));
    }
    const Pose target = TargetPose(*clip_, frame_, mirror_);
    const float mix = ease_motion(
        static_cast<float>(now_ms - frame_started_ms_) / static_cast<float>(frame_duration_ms_));
    pose_.head = from_.head + (target.head - from_.head) * mix;
    pose_.left_arm = from_.left_arm + (target.left_arm - from_.left_arm) * mix;
    pose_.right_arm = from_.right_arm + (target.right_arm - from_.right_arm) * mix;
}

bool PandaController::QueueRandomSound(const char* category) {
    if (audio_ == nullptr || !audio_->PlayRandomSound(category)) {
        return false;
    }
    ESP_LOGI(TAG, "queued random [%s] sound", category);
    return true;
}

wzh_panda::Behaviour PandaController::ClassifySound(int index) const {
    const char* name = audio_->SoundName(index);
    const int duration = audio_->SoundDurationMs(index);
    if (Contains(name, "吃竹") || Contains(name, "咀嚼") || Contains(name, "eat") ||
        Contains(name, "chew")) {
        return Behaviour::kEat;
    }
    if (Contains(name, "宝宝") || Contains(name, "嘤") || Contains(name, "baby")) {
        return Behaviour::kBaby;
    }
    if (Contains(name, "撒娇") || Contains(name, "亲近") || Contains(name, "Cat") ||
        Contains(name, "cat")) {
        return Behaviour::kAffection;
    }
    if (Contains(name, "成年") || Contains(name, "威风") || Contains(name, "adult")) {
        return Behaviour::kProud;
    }
    if (duration > 6500) {
        return Behaviour::kBaby;
    }
    if (duration > 0 && duration < 1700) {
        return Behaviour::kCurious;
    }
    return irnd(100) < 50 ? Behaviour::kCurious : Behaviour::kPlayful;
}

wzh_panda::Pose PandaController::ApplyAudioMotion(const wzh_panda::Pose& pose,
                                                  const PandaMotionData& audio, uint32_t now_ms) {
    // 与旧工程 auto_run.cc 的 apply_audio_motion 完全一致：onset 尖峰驱动
    // 头部/前肢的交替扒动，慢包络驱动呼吸起伏。
    static uint32_t last_ms = now_ms;
    static uint32_t last_onset_ms = 0;
    static uint32_t arms_due_ms = 0;
    static float head_accent = 0.0f, left_accent = 0.0f, right_accent = 0.0f;
    static float pending_arm = 0.0f, slow_phase = 0.0f;
    static int onset_side = 1;

    const uint32_t elapsed = now_ms - last_ms;
    last_ms = now_ms;
    Pose result = pose;
    if (!audio.playing) {
        head_accent *= 0.88f;
        left_accent *= 0.94f;
        right_accent *= 0.94f;
        arms_due_ms = 0;
        return result;
    }
    if (audio.attack > 0.16f && now_ms - last_onset_ms > 320) {
        onset_side = -onset_side;
        head_accent = onset_side * (5.0f + 10.0f * audio.attack);
        pending_arm = 5.0f + 9.0f * audio.attack;
        arms_due_ms = now_ms + static_cast<uint32_t>(irand(160, 320));
        last_onset_ms = now_ms;
    }
    if (arms_due_ms != 0 && now_ms >= arms_due_ms) {
        if (onset_side > 0) {
            left_accent = -pending_arm;
        } else {
            right_accent = pending_arm;
        }
        arms_due_ms = 0;
    }
    head_accent *= 0.90f;
    left_accent *= 0.955f;
    right_accent *= 0.955f;
    slow_phase += static_cast<float>(elapsed) * (0.00045f + 0.0011f * audio.level);
    const float breathing = std::sin(slow_phase) * std::min(5.0f, audio.level * 9.0f);
    result.head += head_accent;
    result.left_arm += left_accent - breathing - std::min(7.0f, audio.level * 12.0f);
    result.right_arm += right_accent + breathing + std::min(7.0f, audio.level * 12.0f);
    return result;
}

void PandaController::TaskEntry(void* arg) { static_cast<PandaController*>(arg)->Task(); }

void PandaController::Task() {
    const int animal_count = audio_ != nullptr ? audio_->SoundCountByCategory("animal") : 0;
    const int ambient_count = audio_ != nullptr ? audio_->SoundCountByCategory("ambient") : 0;
    ESP_LOGI(TAG, "living motion: animal=%d ambient=%d", animal_count, ambient_count);
    uint32_t now_ms = esp_log_timestamp();
    // 开机后尽快播放第一个声音（开机叫声），之后由自主调度继续播放。
    next_sound_ms_ = now_ms + 2000;
    next_ambient_ms_ = now_ms + 300000;
    next_idle_ms_ = now_ms + 1800;
    StartClip(C_SETTLE, now_ms);
    while (true) {
        now_ms = esp_log_timestamp();
        const bool offline = !conversation_active_.load();

        // 明确请求（情绪/MCP/按键/身体部位）优先级最高，在线时不播本地声音以免与 TTS 重叠。
        ActionRequest request;
        if (xQueueReceive(action_queue_, &request, 0) == pdTRUE) {
            if (request.is_body) {
                // 身体部位指令：直接播放对应剪辑（编号从 C_COUNT 起），
                // 不经过行为族随机挑选。
                StartClip(static_cast<int>(C_COUNT) +
                              BodyClipId(static_cast<PandaBodyAction>(request.action)),
                          now_ms);
            } else {
                if (request.with_local_sound && offline && audio_ != nullptr) {
                    audio_->PlayForAction(request.action);
                }
                StartClip(ChooseClip(ActionToBehaviour(request.action)), now_ms);
            }
            // 开机引导动作(kDiscover)不推迟开机叫声，其余动作给足间隔。
            if (!request.is_body && request.action != PandaAction::kDiscover) {
                next_sound_ms_ = now_ms + 6000 + esp_random() % 6000;
            }
            next_ambient_ms_ = now_ms + 120000;
            next_idle_ms_ = now_ms + 4000 + esp_random() % 5000;
            was_playing_ = false;
        }

        PandaMotionData motion = audio_ != nullptr ? audio_->GetMotionData() : PandaMotionData{};

        // 在线说话期间用 AudioService 的真实 TTS 包络驱动舞动，走与旧工程
        // apply_audio_motion 完全相同的包络跟随逻辑，让爬爬熊猫跟着语音动。
        if (!offline && speaking_.load()) {
            PandaMotionData tts = audio_->GetTtsMotionData();
            if (tts.playing) {
                motion = tts;
            }
        }
        const bool playing = motion.playing;

        // 离线自主模式：与旧工程 auto_run.cc 完全一致的声音驱动行为 + 随机待机动作。
        if (offline) {
            if (playing && !was_playing_) {
                const char* category = audio_->SoundCategory(motion.sound_index);
                sound_behaviour_ =
                    std::strcmp(category, "ambient") == 0 ? Behaviour::kAmbient : ClassifySound(motion.sound_index);
                StartClip(ChooseClip(sound_behaviour_), now_ms);
                ESP_LOGI(TAG, "audio start #%d %s -> behaviour %d", motion.sound_index,
                         audio_->SoundName(motion.sound_index), sound_behaviour_);
            }
            if (!playing && was_playing_) {
                StartClip(ChooseClip(Behaviour::kSettle), now_ms);
                next_sound_ms_ = now_ms + static_cast<uint32_t>(irand(20000, 40000));
                next_idle_ms_ = now_ms + static_cast<uint32_t>(irand(2000, 4200));
            }
            was_playing_ = playing;
            if (!clip_active_) {
                if (playing) {
                    StartClip(ChooseClip(sound_behaviour_), now_ms);
                } else if (now_ms >= next_idle_ms_) {
                    StartClip(ChooseClip(Behaviour::kIdle), now_ms);
                    next_idle_ms_ = now_ms + static_cast<uint32_t>(irand(4800, 10000));
                }
            }
            if (!playing && now_ms >= next_ambient_ms_ && ambient_count > 0) {
                if (QueueRandomSound("ambient")) {
                    next_ambient_ms_ = now_ms + static_cast<uint32_t>(irand(480000, 780000));
                    next_sound_ms_ = now_ms + 12000;
                } else {
                    next_ambient_ms_ = now_ms + 30000;
                }
            } else if (!playing && now_ms >= next_sound_ms_ && animal_count > 0) {
                next_sound_ms_ = now_ms + (QueueRandomSound("animal") ? 10000 : 1000);
            }
        } else {
            // 在线模式：情绪/MCP 触发的剪辑播完后，补一个安静的聆听剪辑，
            // 避免动作停在奇怪的姿态；TTS 包络叠加在剪辑上跟随语音。
            was_playing_ = false;
            if (!clip_active_) {
                StartClip(ChooseClip(Behaviour::kAmbient), now_ms);
            }
        }

        UpdatePlayer(now_ms);
        const wzh_panda::Pose out = ApplyAudioMotion(pose_, motion, now_ms);
        OutputPose(out);
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

wzh_panda::Behaviour PandaController::ActionToBehaviour(PandaAction action) {
    switch (action) {
        case PandaAction::kAffection:
            return Behaviour::kAffection;
        case PandaAction::kHappy:
            return Behaviour::kPlayful;
        case PandaAction::kProud:
            return Behaviour::kProud;
        case PandaAction::kEat:
            return Behaviour::kEat;
        case PandaAction::kListen:
            return Behaviour::kAmbient;
        case PandaAction::kStartled:
            return Behaviour::kCurious;
        case PandaAction::kSleepy:
            return Behaviour::kSettle;
        case PandaAction::kDiscover:
        default:
            return Behaviour::kCurious;
    }
}

bool PandaController::TriggerAction(PandaAction action, bool with_local_sound) {
    if (action_queue_ == nullptr) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    // 1.2 秒内的同类情绪通常是同一次服务端状态更新，直接合并以免抽动。
    if (action == last_action_ && now - last_action_time_us_ < 1200000) {
        return true;
    }
    last_action_ = action;
    last_action_time_us_ = now;
    ActionRequest request{action, with_local_sound};
    if (xQueueSend(action_queue_, &request, 0) == pdTRUE) {
        return true;
    }
    // 队列满时丢弃最旧请求，优先响应用户最新的情绪或动作指令。
    ActionRequest discarded;
    xQueueReceive(action_queue_, &discarded, 0);
    return xQueueSend(action_queue_, &request, 0) == pdTRUE;
}

bool PandaController::TriggerBodyAction(PandaBodyAction action) {
    if (action_queue_ == nullptr) {
        return false;
    }
    ESP_LOGI(TAG, "Body action: %s", BodyActionName(action));
    ActionRequest request{static_cast<PandaAction>(action), false, true};
    if (xQueueSend(action_queue_, &request, 0) == pdTRUE) {
        return true;
    }
    ActionRequest discarded;
    xQueueReceive(action_queue_, &discarded, 0);
    return xQueueSend(action_queue_, &request, 0) == pdTRUE;
}

int PandaController::BodyClipId(PandaBodyAction action) {
    switch (action) {
        case PandaBodyAction::kShakeHead:
            return BODY_SHAKE_HEAD;
        case PandaBodyAction::kLookLeft:
            return BODY_LOOK_LEFT;
        case PandaBodyAction::kLookRight:
            return BODY_LOOK_RIGHT;
        case PandaBodyAction::kWaveLeft:
            return BODY_WAVE_LEFT;
        case PandaBodyAction::kWaveRight:
            return BODY_WAVE_RIGHT;
        case PandaBodyAction::kRaiseArms:
            return BODY_RAISE_ARMS;
        default:
            break;
    }
    return BODY_SHAKE_HEAD;
}

bool PandaController::ParseBodyAction(const std::string& name, PandaBodyAction& action) {
    static const struct {
        const char* name;
        PandaBodyAction action;
    } actions[] = {{"shake_head", PandaBodyAction::kShakeHead},
                   {"look_left", PandaBodyAction::kLookLeft},
                   {"look_right", PandaBodyAction::kLookRight},
                   {"wave_left", PandaBodyAction::kWaveLeft},
                   {"wave_right", PandaBodyAction::kWaveRight},
                   {"raise_arms", PandaBodyAction::kRaiseArms}};
    for (const auto& item : actions) {
        if (name == item.name) {
            action = item.action;
            return true;
        }
    }
    return false;
}

const char* PandaController::BodyActionName(PandaBodyAction action) {
    switch (action) {
        case PandaBodyAction::kShakeHead:
            return "shake_head";
        case PandaBodyAction::kLookLeft:
            return "look_left";
        case PandaBodyAction::kLookRight:
            return "look_right";
        case PandaBodyAction::kWaveLeft:
            return "wave_left";
        case PandaBodyAction::kWaveRight:
            return "wave_right";
        case PandaBodyAction::kRaiseArms:
            return "raise_arms";
        default:
            break;
    }
    return "shake_head";
}

void PandaController::SetConversationActive(bool active) {
    const bool changed = conversation_active_.exchange(active) != active;
    if (!changed) {
        return;
    }
    ESP_LOGI(TAG, "Mode changed to %s", active ? "online/conversation" : "offline/autonomous");
    if (!active) {
        speaking_.store(false);
    }
    if (active && audio_ != nullptr) {
        audio_->StopPlayback();
    }
    // 开始对话用倾听动作迎接用户；回到待机后立即恢复自主模式。
    TriggerAction(active ? PandaAction::kListen : PandaAction::kDiscover, !active);
}

void PandaController::SetSpeaking(bool speaking) {
    speaking_.store(speaking);
}

void PandaController::SetDefaultPose() {
    SetServoAngles(SERVO_HEAD_DEFAULT, SERVO_LEFT_ARM_DEFAULT, SERVO_RIGHT_ARM_DEFAULT);
}

PandaAction PandaController::EmotionToAction(const std::string& emotion) const {
    if (emotion == "happy" || emotion == "laughing" || emotion == "winking" ||
        emotion == "funny" || emotion == "excited" || emotion == "delighted" ||
        emotion == "joyful" || emotion == "proud") {
        return PandaAction::kHappy;
    }
    if (emotion == "loving" || emotion == "embarrassed" || emotion == "relaxed" ||
        emotion == "grateful" || emotion == "gratitude" || emotion == "love") {
        return PandaAction::kAffection;
    }
    if (emotion == "sad" || emotion == "crying" || emotion == "sleepy" ||
        emotion == "tired" || emotion == "bored" || emotion == "depressed" ||
        emotion == "lonely") {
        return PandaAction::kSleepy;
    }
    if (emotion == "angry" || emotion == "confident" || emotion == "cool" ||
        emotion == "annoyed" || emotion == "frustrated") {
        return PandaAction::kProud;
    }
    if (emotion == "surprised" || emotion == "shocked" || emotion == "fearful" ||
        emotion == "scared" || emotion == "worried") {
        return PandaAction::kStartled;
    }
    if (emotion == "thinking" || emotion == "confused" || emotion == "curious") {
        return PandaAction::kDiscover;
    }
    return PandaAction::kDiscover;
}

void PandaController::HandleLlmEmotion(const char* emotion) {
    // 离线时 llm.emotion 已失去时效性，应继续由自主逻辑控制动作。
    if (!conversation_active_.load() || emotion == nullptr || emotion[0] == '\0') {
        return;
    }
    // neutral 是状态机/界面的复位信号，不是可表演的情绪，忽略它。
    if (std::strcmp(emotion, "neutral") == 0 || std::strcmp(emotion, "normal") == 0) {
        return;
    }
    TriggerAction(EmotionToAction(emotion), false);
}

void PandaController::HandleUserSpeechEmotion(const char* text) {
    if (!conversation_active_.load() || text == nullptr || text[0] == '\0') {
        return;
    }
    // 身体部位指令优先级最高："摇摇头/向左看/挥挥手/举起前肢"等。
    static const struct {
        PandaBodyAction action;
        const char* words[4];
    } body_rules[] = {
        {PandaBodyAction::kShakeHead, {"摇头", "晃晃头", "甩头", "晃头"}},
        {PandaBodyAction::kLookLeft, {"向左看", "看左边", "看左", "左看"}},
        {PandaBodyAction::kLookRight, {"向右看", "看右边", "看右", "右看"}},
        {PandaBodyAction::kWaveLeft, {"挥左手", "招招手", "挥挥手", "左手"}},
        {PandaBodyAction::kWaveRight, {"挥右手", "右手", "举手", "抬抬手"}},
        {PandaBodyAction::kRaiseArms, {"举起前肢", "举起手", "举手", "举手手"}},
    };
    int body_best = -1;
    int body_score = 0;
    for (size_t rule = 0; rule < sizeof(body_rules) / sizeof(body_rules[0]); ++rule) {
        int score = 0;
        for (size_t word = 0; word < 4 && body_rules[rule].words[word] != nullptr; ++word) {
            if (std::strstr(text, body_rules[rule].words[word]) != nullptr) {
                ++score;
            }
        }
        if (score > body_score) {
            body_score = score;
            body_best = static_cast<int>(rule);
        }
    }
    if (body_score > 0) {
        ESP_LOGI(TAG, "User speech body: %s -> %s", text,
                 BodyActionName(body_rules[body_best].action));
        TriggerBodyAction(body_rules[body_best].action);
        return;
    }
    // 服务端 llm.emotion 经常缺失，这里对用户转写文本做一次轻量关键词
    // 兜底判断，让明显的情绪也能立刻驱动身体语言。未命中则不干预。
    static const struct {
        PandaAction action;
        const char* words[6];
    } rules[] = {
        {PandaAction::kHappy, {"满分", "太棒", "真棒", "厉害", "恭喜", "太好了"}},
        {PandaAction::kHappy, {"开心", "高兴", "惊喜", "嘻嘻", "哈哈", "万岁"}},
        {PandaAction::kSleepy, {"难过", "伤心", "想哭", "委屈", "难受", "不开心"}},
        {PandaAction::kProud, {"生气", "气死", "讨厌", "可恶", "气愤", "恼火"}},
        {PandaAction::kAffection, {"爱你", "喜欢你", "想你", "亲亲", "抱抱", "好想你"}},
        {PandaAction::kStartled, {"吓死", "吓到", "天啊", "震惊", "没想到", "吃惊"}},
        {PandaAction::kSleepy, {"困了", "好困", "睡觉", "晚安", "好累", "累死"}},
        {PandaAction::kEat, {"吃饭", "好吃", "饿了", "好饿", "干饭", "美味"}},
        {PandaAction::kProud, {"骄傲", "自豪", "炫耀", "冠军", "第一", "赢了"}},
    };
    int best = -1;
    int best_score = 0;
    for (size_t rule = 0; rule < sizeof(rules) / sizeof(rules[0]); ++rule) {
        int score = 0;
        for (size_t word = 0; word < 6 && rules[rule].words[word] != nullptr; ++word) {
            if (std::strstr(text, rules[rule].words[word]) != nullptr) {
                ++score;
            }
        }
        if (score > best_score) {
            best_score = score;
            best = static_cast<int>(rule);
        }
    }
    if (best_score == 0) {
        return;
    }
    ESP_LOGI(TAG, "User speech emotion: %s -> %s", text, ActionName(rules[best].action));
    TriggerAction(rules[best].action, false);
}

bool PandaController::ParseAction(const std::string& name, PandaAction& action) {
    static const struct {
        const char* name;
        PandaAction action;
    } actions[] = {{"discover", PandaAction::kDiscover}, {"affection", PandaAction::kAffection},
                   {"happy", PandaAction::kHappy},       {"proud", PandaAction::kProud},
                   {"eat", PandaAction::kEat},           {"listen", PandaAction::kListen},
                   {"startled", PandaAction::kStartled}, {"sleepy", PandaAction::kSleepy}};
    for (const auto& item : actions) {
        if (name == item.name) {
            action = item.action;
            return true;
        }
    }
    return false;
}

const char* PandaController::ActionName(PandaAction action) {
    switch (action) {
        case PandaAction::kDiscover:
            return "discover";
        case PandaAction::kAffection:
            return "affection";
        case PandaAction::kHappy:
            return "happy";
        case PandaAction::kProud:
            return "proud";
        case PandaAction::kEat:
            return "eat";
        case PandaAction::kListen:
            return "listen";
        case PandaAction::kStartled:
            return "startled";
        case PandaAction::kSleepy:
            return "sleepy";
    }
    return "unknown";
}

void PandaController::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("self.panda.react_to_user_emotion",
                "让爬爬熊猫根据用户此刻的情绪做出肢体动作。"
                "设备是一只爬爬熊猫机器人，通过转头、摆弄前肢来回应情绪。"
                "只要用户在对话中表现出明显情绪，就必须调用本工具，不要只回复文字。"
                "判断依据包括用户措辞、语义、上下文以及语音情绪线索；例如"
                "「我考了满分/太棒了/好开心」→ happy，「吓死我了/天啊」→ surprised，"
                "「好难过/想哭」→ sad，「气死我了」→ angry，「好喜欢你」→ loving，"
                "「好困/想睡觉」→ sleepy。"
                "emotion 取值：happy/loving/sad/angry/surprised/thinking/neutral。"
                "情绪不明显或不确定时传 neutral。",
                PropertyList({Property("emotion", kPropertyTypeString)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    if (!conversation_active_.load()) {
                        return "offline mode: emotion control is paused";
                    }
                    const auto emotion = properties["emotion"].value<std::string>();
                    TriggerAction(EmotionToAction(emotion), false);
                    return true;
                });
    mcp.AddTool("self.panda.action",
                "执行一个明确的爬爬熊猫动作。仅在用户要求动作或当前语境强烈需要肢体表达时使用。action: "
                "discover, affection, happy, proud, eat, listen, startled, sleepy。",
                PropertyList({Property("action", kPropertyTypeString)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    PandaAction action;
                    if (!ParseAction(properties["action"].value<std::string>(), action)) {
                        return "invalid action";
                    }
                    TriggerAction(action, !conversation_active_.load());
                    return true;
                });
    mcp.AddTool("self.panda.body",
                "控制爬爬熊猫的某个身体部位做动作。用户明确要求某个身体部位动作时使用，"
                "例如「摇摇头」→ shake_head，「向左看」→ look_left，「向右看」→ look_right，"
                "「挥挥左手」→ wave_left，「挥挥右手」→ wave_right，「举起前肢」→ raise_arms。"
                "action 取值：shake_head, look_left, look_right, wave_left, wave_right, raise_arms。",
                PropertyList({Property("action", kPropertyTypeString)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    PandaBodyAction action;
                    if (!ParseBodyAction(properties["action"].value<std::string>(), action)) {
                        return "invalid body action";
                    }
                    TriggerBodyAction(action);
                    return true;
                });
    mcp.AddTool("self.panda.get_mode", "返回爬爬熊猫当前 online 或 offline 模式。", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    return std::string(conversation_active_.load() ? "online" : "offline");
                });
}
