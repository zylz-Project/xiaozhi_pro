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
// 数组顺序必须与三路舵机顺序一致：头、尾左右、尾上下。
constexpr gpio_num_t kServoPins[] = {SERVO_HEAD_GPIO, SERVO_TAIL_LR_GPIO, SERVO_TAIL_UD_GPIO};
constexpr ledc_channel_t kServoChannels[] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2};

int irnd(int count) { return count > 0 ? static_cast<int>(esp_random() % static_cast<uint32_t>(count)) : 0; }
int irand(int low, int high) { return low + irnd(high - low + 1); }
float frand(float low, float high) {
    return low + (static_cast<float>(esp_random()) / static_cast<float>(UINT32_MAX)) * (high - low);
}
float smoothstep(float value) {
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

Pose LerpPose(Pose a, Pose b, float t) {
    Pose result;
    result.head = a.head + (b.head - a.head) * t;
    result.tail_lr = a.tail_lr + (b.tail_lr - a.tail_lr) * t;
    result.tail_ud = a.tail_ud + (b.tail_ud - a.tail_ud) * t;
    return result;
}

Pose TargetPose(const Clip& clip, uint8_t frame, bool mirror) {
    const Keyframe& key = clip.frames[frame];
    Pose pose{static_cast<float>(key.head), static_cast<float>(key.tail_lr),
              static_cast<float>(key.tail_ud)};
    if (mirror && clip.mirrorable) {
        pose.head = 180.0f - pose.head;
        pose.tail_lr = 180.0f - pose.tail_lr;
    }
    return pose;
}

// 连续目标流：帧进度的最后 30% 提前滑向下一帧 → 动作连续流动、永不停顿。
Pose FlowTarget(const Clip& clip, uint8_t frame, float progress, bool mirror) {
    const uint8_t next = std::min<uint8_t>(frame + 1, clip.count - 1);
    const Pose cur = TargetPose(clip, frame, mirror);
    const Pose nxt = TargetPose(clip, next, mirror);
    const float lead = std::clamp((progress - 0.70f) / 0.30f, 0.0f, 1.0f);
    return LerpPose(cur, nxt, smoothstep(lead));
}

// ============================================================================
// 关键帧动作库（移植自旧工程 tailRedPanda 的 living-motion 引擎）。
// Keyframe 顺序为 duration_ms / head / tail_lr / tail_ud。
// head: 0=右 90=中 180=左；tail_lr: 0=左 90=中 180=右；
// tail_ud: 0=上 90=中 180=下，默认坐姿 150。
// ============================================================================
constexpr Keyframe kIdleGlance[] = {
    {650, 132, 82, 156}, {420, 145, 82, 156}, {820, 112, 112, 142}, {950, 90, 90, 150}};
constexpr Keyframe kIdleLowSweep[] = {
    {850, 78, 32, 170}, {1150, 108, 154, 154}, {950, 96, 55, 174}, {900, 90, 90, 152}};
constexpr Keyframe kIdlePerk[] = {
    {700, 118, 95, 148}, {850, 150, 142, 58}, {520, 136, 156, 44}, {1100, 96, 104, 126},
    {700, 90, 90, 150}};
constexpr Keyframe kIdleCircle[] = {
    {650, 62, 35, 126}, {650, 75, 76, 58},   {650, 105, 150, 78}, {650, 125, 158, 144},
    {650, 102, 78, 170}, {850, 90, 90, 148}};
constexpr Keyframe kCuriousPeek[] = {
    {380, 142, 90, 158}, {520, 154, 90, 158}, {700, 148, 138, 92}, {420, 62, 146, 74},
    {680, 128, 45, 120}, {950, 90, 90, 148}};
constexpr Keyframe kCuriousTrack[] = {
    {500, 38, 104, 158}, {820, 54, 150, 132}, {350, 145, 150, 132}, {760, 132, 45, 72},
    {950, 90, 90, 146}};
constexpr Keyframe kCuriousCircle[] = {
    {450, 150, 92, 158}, {600, 138, 34, 106}, {600, 92, 70, 42}, {600, 48, 148, 68},
    {600, 72, 158, 146}, {900, 90, 90, 150}};
constexpr Keyframe kEatSearch[] = {
    {700, 42, 70, 168}, {520, 128, 38, 174}, {420, 55, 118, 172}, {420, 138, 150, 165},
    {600, 72, 62, 174}, {850, 90, 90, 154}};
constexpr Keyframe kEatChew[] = {
    {620, 128, 74, 168}, {360, 56, 76, 168}, {470, 122, 108, 172}, {330, 68, 110, 172},
    {620, 145, 148, 158}, {900, 90, 90, 152}};
constexpr Keyframe kEatDelight[] = {
    {650, 45, 42, 174}, {500, 132, 148, 166}, {650, 55, 154, 138}, {620, 138, 36, 102},
    {850, 90, 90, 150}};
constexpr Keyframe kBabySeek[] = {
    {420, 35, 92, 160}, {360, 145, 92, 160}, {650, 128, 42, 128}, {520, 52, 148, 86},
    {750, 135, 122, 52}, {950, 90, 90, 146}};
constexpr Keyframe kBabyWiggle[] = {
    {500, 138, 48, 145}, {440, 58, 144, 116}, {500, 126, 32, 82}, {460, 65, 155, 58},
    {750, 120, 72, 126}, {900, 90, 90, 150}};
constexpr Keyframe kBabyComfort[] = {
    {800, 138, 128, 142}, {900, 150, 154, 94}, {1000, 126, 122, 52}, {1000, 104, 62, 96},
    {950, 90, 90, 148}};
constexpr Keyframe kProudDisplay[] = {
    {650, 45, 88, 158}, {750, 142, 40, 92}, {900, 155, 84, 28}, {650, 62, 156, 48},
    {850, 132, 138, 108}, {1000, 90, 90, 150}};
constexpr Keyframe kProudPatrol[] = {
    {900, 32, 148, 130}, {900, 148, 34, 98}, {750, 122, 70, 44}, {750, 52, 152, 66},
    {1000, 90, 90, 148}};
constexpr Keyframe kProudCall[] = {
    {500, 118, 90, 154}, {780, 155, 46, 92}, {900, 148, 90, 24}, {700, 48, 158, 54},
    {750, 128, 34, 118}, {1050, 90, 90, 150}};
constexpr Keyframe kNuzzle[] = {
    {800, 142, 118, 152}, {900, 158, 150, 118}, {850, 146, 128, 70}, {900, 120, 48, 102},
    {1050, 90, 90, 150}};
constexpr Keyframe kShyApproach[] = {
    {650, 38, 76, 164}, {900, 55, 42, 140}, {620, 142, 138, 96}, {850, 152, 154, 58},
    {1100, 90, 90, 150}};
constexpr Keyframe kSoftCurl[] = {
    {1000, 128, 36, 154}, {1100, 145, 58, 92}, {1200, 132, 122, 46}, {1000, 108, 156, 108},
    {1050, 90, 90, 150}};
constexpr Keyframe kPlayWag[] = {
    {480, 138, 28, 132}, {480, 55, 154, 112}, {520, 128, 36, 78}, {520, 62, 158, 54},
    {620, 145, 42, 98}, {850, 90, 90, 148}};
constexpr Keyframe kPlayChase[] = {
    {550, 42, 150, 154}, {550, 138, 42, 126}, {600, 52, 30, 76}, {600, 145, 148, 42},
    {650, 118, 158, 126}, {900, 90, 90, 150}};
constexpr Keyframe kPlayEight[] = {
    {520, 145, 35, 112}, {520, 72, 78, 48},   {520, 42, 150, 106}, {520, 128, 105, 164},
    {520, 152, 38, 104}, {520, 58, 104, 52},  {520, 36, 154, 116}, {850, 90, 90, 150}};
constexpr Keyframe kAmbientListen[] = {
    {1100, 42, 90, 166}, {1500, 55, 132, 138}, {1200, 145, 142, 84}, {1800, 132, 62, 56},
    {1600, 90, 90, 146}};
constexpr Keyframe kAmbientRain[] = {
    {1000, 132, 42, 168}, {800, 48, 148, 154}, {1300, 120, 154, 106}, {1400, 62, 84, 48},
    {1500, 104, 32, 112}, {1500, 90, 90, 152}};
constexpr Keyframe kAmbientScent[] = {
    {1300, 35, 54, 170}, {1500, 62, 148, 158}, {1300, 142, 128, 116}, {1500, 118, 38, 72},
    {1600, 90, 90, 150}};
constexpr Keyframe kAmbientStretch[] = {
    {1500, 122, 90, 174}, {1800, 145, 42, 116}, {1800, 132, 82, 34}, {1700, 55, 154, 68},
    {1800, 90, 90, 150}};
constexpr Keyframe kSettle[] = {{700, 90, 90, 170}, {1200, 102, 108, 146}, {850, 90, 90, 150}};
constexpr Keyframe kStartledFrames[] = {
    {80, 88, 92, 148},    // 定格瞬间
    {130, 42, 148, 132},  // 快速缩头、尾巴甩向一侧
    {220, 150, 42, 118},  // 惊疑望向另一侧
    {300, 132, 138, 96},  // 身体压低戒备
    {320, 52, 58, 120},   // 再确认另一侧
    {380, 118, 104, 132}, // 缓一口气
    {600, 90, 90, 146},   // 收势
    {700, 90, 90, 150},
};

enum ClipId {
    C_IDLE_GLANCE,
    C_IDLE_LOW_SWEEP,
    C_IDLE_PERK,
    C_IDLE_CIRCLE,
    C_CURIOUS_PEEK,
    C_CURIOUS_TRACK,
    C_CURIOUS_CIRCLE,
    C_EAT_SEARCH,
    C_EAT_CHEW,
    C_EAT_DELIGHT,
    C_BABY_SEEK,
    C_BABY_WIGGLE,
    C_BABY_COMFORT,
    C_PROUD_DISPLAY,
    C_PROUD_PATROL,
    C_PROUD_CALL,
    C_NUZZLE,
    C_SHY_APPROACH,
    C_SOFT_CURL,
    C_PLAY_WAG,
    C_PLAY_CHASE,
    C_PLAY_EIGHT,
    C_AMBIENT_LISTEN,
    C_AMBIENT_RAIN,
    C_AMBIENT_SCENT,
    C_AMBIENT_STRETCH,
    C_SETTLE,
    C_STARTLED,
    C_COUNT,
};

constexpr Clip kClips[C_COUNT] = {
    {"idle_glance", kIdleGlance, std::size(kIdleGlance), true},
    {"idle_low_sweep", kIdleLowSweep, std::size(kIdleLowSweep), true},
    {"idle_perk", kIdlePerk, std::size(kIdlePerk), true},
    {"idle_circle", kIdleCircle, std::size(kIdleCircle), true},
    {"curious_peek", kCuriousPeek, std::size(kCuriousPeek), true},
    {"curious_track", kCuriousTrack, std::size(kCuriousTrack), true},
    {"curious_circle", kCuriousCircle, std::size(kCuriousCircle), true},
    {"eat_search", kEatSearch, std::size(kEatSearch), true},
    {"eat_chew", kEatChew, std::size(kEatChew), true},
    {"eat_look_up", kEatDelight, std::size(kEatDelight), true},
    {"baby_seek", kBabySeek, std::size(kBabySeek), true},
    {"baby_wiggle", kBabyWiggle, std::size(kBabyWiggle), true},
    {"baby_comfort", kBabyComfort, std::size(kBabyComfort), true},
    {"proud_display", kProudDisplay, std::size(kProudDisplay), true},
    {"proud_patrol", kProudPatrol, std::size(kProudPatrol), true},
    {"proud_call", kProudCall, std::size(kProudCall), true},
    {"affection_nuzzle", kNuzzle, std::size(kNuzzle), true},
    {"affection_shy", kShyApproach, std::size(kShyApproach), true},
    {"affection_curl", kSoftCurl, std::size(kSoftCurl), true},
    {"playful_wag", kPlayWag, std::size(kPlayWag), true},
    {"playful_chase", kPlayChase, std::size(kPlayChase), true},
    {"playful_figure8", kPlayEight, std::size(kPlayEight), true},
    {"ambient_listen", kAmbientListen, std::size(kAmbientListen), true},
    {"ambient_rain", kAmbientRain, std::size(kAmbientRain), true},
    {"ambient_scent", kAmbientScent, std::size(kAmbientScent), true},
    {"ambient_stretch", kAmbientStretch, std::size(kAmbientStretch), true},
    {"settle", kSettle, std::size(kSettle), false},
    {"startled_flinch", kStartledFrames, std::size(kStartledFrames), true},
};

constexpr int8_t kFamily[kBehaviourCount][4] = {
    {C_IDLE_GLANCE, C_IDLE_LOW_SWEEP, C_IDLE_PERK, C_IDLE_CIRCLE},
    {C_CURIOUS_PEEK, C_CURIOUS_TRACK, C_CURIOUS_CIRCLE, -1},
    {C_EAT_SEARCH, C_EAT_CHEW, C_EAT_DELIGHT, -1},
    {C_BABY_SEEK, C_BABY_WIGGLE, C_BABY_COMFORT, -1},
    {C_PROUD_DISPLAY, C_PROUD_PATROL, C_PROUD_CALL, -1},
    {C_NUZZLE, C_SHY_APPROACH, C_SOFT_CURL, -1},
    {C_PLAY_WAG, C_PLAY_CHASE, C_PLAY_EIGHT, -1},
    {C_AMBIENT_LISTEN, C_AMBIENT_RAIN, C_AMBIENT_SCENT, C_AMBIENT_STRETCH},
    {C_SETTLE, -1, -1, -1},
    {C_STARTLED, -1, -1, -1},
};

// ============================================================================
// 身体部位级动作剪辑：从当前姿态起手，结束时回到默认坐姿 90/90/150。
// 方向性动作（看左/看右）不可镜像，避免反向。
// ============================================================================
constexpr Keyframe kBodyShakeHead[] = {
    {140, 60, 90, 150}, {140, 120, 90, 150}, {140, 60, 90, 150}, {140, 120, 90, 150},
    {320, 90, 90, 150}};
constexpr Keyframe kBodyLookLeft[] = {{300, 140, 90, 150}, {600, 140, 90, 150}, {400, 90, 90, 150}};
constexpr Keyframe kBodyLookRight[] = {
    {300, 40, 90, 150}, {600, 40, 90, 150}, {400, 90, 90, 150}};
constexpr Keyframe kBodyWagTail[] = {
    {160, 90, 55, 148}, {160, 90, 125, 148}, {160, 90, 55, 148}, {160, 90, 125, 148},
    {320, 90, 90, 150}};
constexpr Keyframe kBodyTwistNeck[] = {
    {280, 55, 95, 150}, {280, 125, 95, 150}, {280, 55, 95, 150}, {400, 90, 90, 150}};

enum BodyClipId {
    BODY_SHAKE_HEAD,
    BODY_LOOK_LEFT,
    BODY_LOOK_RIGHT,
    BODY_WAG_TAIL,
    BODY_TWIST_NECK,
    BODY_COUNT,
};
constexpr Clip kBodyClips[BODY_COUNT] = {
    {"body_shake_head", kBodyShakeHead, std::size(kBodyShakeHead), true},
    {"body_look_left", kBodyLookLeft, std::size(kBodyLookLeft), false},
    {"body_look_right", kBodyLookRight, std::size(kBodyLookRight), false},
    {"body_wag_tail", kBodyWagTail, std::size(kBodyWagTail), true},
    {"body_twist_neck", kBodyTwistNeck, std::size(kBodyTwistNeck), true},
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
    // 舵机电源与 PWM 分开控制：先给电源，再把各通道设置到安全默认角度。
    gpio_config_t power_config = {};
    power_config.pin_bit_mask = 1ULL << SERVO_POWER_GPIO;
    power_config.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&power_config));
    ESP_ERROR_CHECK(gpio_set_level(SERVO_POWER_GPIO, 1));

    ledc_timer_config_t timer_config = {};
    timer_config.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_config.duty_resolution = SERVO_DUTY_RES;
    timer_config.timer_num = SERVO_TIMER;
    timer_config.freq_hz = SERVO_FREQ_HZ;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    const int initial_angles[] = {SERVO_HEAD_DEFAULT, SERVO_TAIL_LR_DEFAULT, SERVO_TAIL_UD_DEFAULT};
    for (size_t i = 0; i < std::size(kServoPins); ++i) {
        ledc_channel_config_t channel_config = {};
        channel_config.gpio_num = kServoPins[i];
        channel_config.speed_mode = LEDC_LOW_SPEED_MODE;
        channel_config.channel = kServoChannels[i];
        channel_config.timer_sel = SERVO_TIMER;
        channel_config.duty = AngleToDuty(initial_angles[i]);
        channel_config.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
    }
    SetServoAngles(SERVO_HEAD_DEFAULT, SERVO_TAIL_LR_DEFAULT, SERVO_TAIL_UD_DEFAULT);
}

void PandaController::SetServoAngles(int head, int tail_lr, int tail_ud) {
    const int angles[] = {head, tail_lr, tail_ud};
    for (size_t i = 0; i < std::size(kServoChannels); ++i) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, kServoChannels[i], AngleToDuty(angles[i]));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, kServoChannels[i]);
    }
}

void PandaController::OutputPose(const wzh_panda::Pose& pose) {
    const int head = static_cast<int>(std::lround(std::clamp(pose.head, static_cast<float>(SERVO_HEAD_MIN),
                                                             static_cast<float>(SERVO_HEAD_MAX))));
    const int lr = static_cast<int>(std::lround(std::clamp(pose.tail_lr, static_cast<float>(SERVO_TAIL_LR_MIN),
                                                           static_cast<float>(SERVO_TAIL_LR_MAX))));
    const int ud = static_cast<int>(std::lround(std::clamp(pose.tail_ud, static_cast<float>(SERVO_TAIL_UD_MIN),
                                                           static_cast<float>(SERVO_TAIL_UD_MAX))));
    SetServoAngles(head, lr, ud);
}

int PandaController::ChooseClip(Behaviour behaviour) {
    const size_t behaviour_index = static_cast<size_t>(behaviour);
    ShuffleBag& bag = bags_[behaviour_index];
    if (bag.position >= bag.count) {
        bag.count = 0;
        for (int i = 0; i < 4 && kFamily[behaviour_index][i] >= 0; ++i) {
            bag.items[bag.count++] = kFamily[behaviour_index][i];
        }
        for (int i = bag.count - 1; i > 0; --i) {
            std::swap(bag.items[i], bag.items[irnd(i + 1)]);
        }
        if (bag.count > 1 && bag.items[0] == last_clip_) {
            std::swap(bag.items[0], bag.items[1]);
        }
        bag.position = 0;
    }
    const int result = bag.items[bag.position++];
    last_clip_ = result;
    return result;
}

void PandaController::StartClip(int clip_id, uint32_t now_ms) {
    const Clip& clip = kClips[clip_id];
    clip_ = &clip;
    frame_ = 0;
    frame_started_ms_ = now_ms;
    // 所有动作都带随机节奏；硬摆动只在真机校准阶段由板级配置决定，这里
    // 统一使用柔和节奏，保证“灵动”而非生硬。
    tempo_ = frand(0.88f, 1.14f);
    frame_duration_ms_ = std::max<uint32_t>(180, static_cast<uint32_t>(clip.frames[0].duration_ms * tempo_));
    if (!clip.mirrorable) {
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
    ESP_LOGI(TAG, "clip=%s mirror=%d tempo=%.2f", clip.name, mirror_, static_cast<double>(tempo_));
}

void PandaController::UpdatePlayer(uint32_t now_ms) {
    if (clip_ == nullptr) {
        return;
    }
    while (clip_active_ && now_ms - frame_started_ms_ >= frame_duration_ms_) {
        frame_started_ms_ += frame_duration_ms_;
        if (++frame_ >= clip_->count) {
            clip_active_ = false;
            break;
        }
        frame_duration_ms_ = std::max<uint32_t>(
            180, static_cast<uint32_t>(clip_->frames[frame_].duration_ms * tempo_));
    }
    const uint8_t frame = std::min<uint8_t>(frame_, clip_->count - 1);
    const float progress =
        std::clamp(static_cast<float>(now_ms - frame_started_ms_) / static_cast<float>(frame_duration_ms_),
                   0.0f, 1.0f);
    const Pose target = FlowTarget(*clip_, frame, progress, mirror_);
    // 指数追踪：姿态持续逼近目标流，自然缓入缓出，软着陆无硬停。
    const uint32_t tau = std::max<uint32_t>(70, frame_duration_ms_ / 3);
    const float k = 1.0f - std::exp(-20.0f / static_cast<float>(tau));
    pose_.head += (target.head - pose_.head) * k;
    pose_.tail_lr += (target.tail_lr - pose_.tail_lr) * k;
    pose_.tail_ud += (target.tail_ud - pose_.tail_ud) * k;
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
    if (Contains(name, "宝宝") || Contains(name, "嘤嘤") || Contains(name, "baby")) {
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
        return Behaviour::kProud;
    }
    if (duration > 0 && duration < 1700) {
        return Behaviour::kCurious;
    }
    return irnd(100) < 55 ? Behaviour::kCurious : Behaviour::kPlayful;
}

void PandaController::AddMicroMotion(wzh_panda::Pose& pose, uint32_t now_ms, float life) {
    // 呼吸带动头部微幅起伏、尾巴轻轻浮沉。
    breath_phase_ += 20.0f;
    if (breath_phase_ > breath_period_) {
        breath_phase_ = 0.0f;
        breath_period_ = static_cast<uint32_t>(irand(2800, 4300));
    }
    const float b = std::sin(6.2831853f * breath_phase_ / static_cast<float>(breath_period_));
    pose.head += b * 0.7f * life;
    pose.tail_ud -= b * 1.4f * life;

    // 头部漫无目的地漂移凝视。
    if (now_ms >= gaze_next_ms_) {
        gaze_target_ = frand(-9.0f, 9.0f);
        gaze_next_ms_ = now_ms + static_cast<uint32_t>(irand(1200, 3800));
    }
    gaze_pos_ += (gaze_target_ - gaze_pos_) * 0.03f;
    pose.head += gaze_pos_ * life;

    // 尾巴轻摆。
    const float t = static_cast<float>(now_ms) * 0.001f;
    pose.tail_lr += (std::sin(t * 1.7f) + std::sin(t * 0.9f + 1.3f) * 0.7f) * 1.1f * life;
    pose.tail_ud += std::sin(t * 2.1f + 0.7f) * 0.5f * life;

    // 肌肉微颤，让静止的身体仍有生命感。
    tremor_phase_ += 0.9f;
    pose.head += (std::sin(tremor_phase_ * 3.1f) + std::sin(tremor_phase_ * 5.7f + 2.0f)) * 0.15f * life;
    pose.tail_lr += (std::sin(tremor_phase_ * 4.3f + 1.1f) + std::sin(tremor_phase_ * 2.3f)) * 0.12f * life;
    pose.tail_ud += std::sin(tremor_phase_ * 3.7f + 0.4f) * 0.12f * life;
}

wzh_panda::Pose PandaController::ApplyAudioMotion(const wzh_panda::Pose& pose,
                                                  const PandaMotionData& audio, uint32_t now_ms) {
    static uint32_t last_ms = now_ms;
    static uint32_t last_onset_ms = 0;
    static uint32_t tail_due_ms = 0;
    static float onset_offset = 0, tail_onset_offset = 0, pending_tail_offset = 0, tail_phase = 0;
    static int onset_side = 1;

    const uint32_t elapsed = now_ms - last_ms;
    last_ms = now_ms;
    Pose result = pose;
    if (!audio.playing) {
        onset_offset *= 0.88f;
        tail_onset_offset *= 0.94f;
        tail_due_ms = 0;
        return result;
    }
    if (audio.attack > 0.20f && now_ms - last_onset_ms > 420) {
        onset_side = -onset_side;
        onset_offset = onset_side * (4.0f + 8.0f * audio.attack);
        pending_tail_offset = -onset_side * (4.0f + 6.0f * audio.attack);
        tail_due_ms = now_ms + static_cast<uint32_t>(irand(180, 360));
        last_onset_ms = now_ms;
    }
    if (tail_due_ms != 0 && now_ms >= tail_due_ms) {
        tail_onset_offset = pending_tail_offset;
        tail_due_ms = 0;
    }
    onset_offset *= 0.90f;
    tail_onset_offset *= 0.955f;
    tail_phase += static_cast<float>(elapsed) * (0.00055f + 0.00125f * audio.level);
    result.head += onset_offset;
    result.tail_lr += tail_onset_offset + std::sin(tail_phase) * std::min(14.0f, audio.level * 24.0f);
    result.tail_ud -= std::min(11.0f, audio.level * 18.0f);
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
                // 身体部位指令：直接播放对应剪辑，不经过行为族随机挑选。
                StartClip(BodyClipId(static_cast<PandaBodyAction>(request.action)), now_ms);
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
            clip_finished_ms_ = 0;
            was_playing_ = false;
        }

        PandaMotionData motion = audio_ != nullptr ? audio_->GetMotionData() : PandaMotionData{};
        const bool playing = motion.playing;

        // 离线自主模式：声音驱动行为 + 随机待机动作。
        if (offline) {
            const bool clip_was_active = clip_active_;
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
                next_sound_ms_ = now_ms + static_cast<uint32_t>(irand(4000, 6000));
                next_idle_ms_ = now_ms + static_cast<uint32_t>(irand(1800, 4200));
            }
            was_playing_ = playing;
            if (!clip_active_ && clip_was_active) {
                clip_finished_ms_ = now_ms;
            }
            if (!clip_active_) {
                if (playing) {
                    StartClip(ChooseClip(sound_behaviour_), now_ms);
                } else if (now_ms >= next_idle_ms_) {
                    StartClip(ChooseClip(Behaviour::kIdle), now_ms);
                    next_idle_ms_ = now_ms + static_cast<uint32_t>(irand(4200, 9000));
                } else if (clip_finished_ms_ != 0 && now_ms - clip_finished_ms_ > 1400) {
                    // 表演结束后短暂停留，微动层接管，再轻轻收尾。
                    StartClip(ChooseClip(Behaviour::kSettle), now_ms);
                    clip_finished_ms_ = 0;
                    next_idle_ms_ = now_ms + static_cast<uint32_t>(irand(1500, 3200));
                }
            }
            if (!playing && now_ms >= next_ambient_ms_ && ambient_count > 0) {
                if (QueueRandomSound("ambient")) {
                    next_ambient_ms_ = now_ms + static_cast<uint32_t>(irand(480000, 780000));
                    next_sound_ms_ = now_ms + 10000;
                } else {
                    next_ambient_ms_ = now_ms + 30000;
                }
            } else if (!playing && now_ms >= next_sound_ms_ && animal_count > 0) {
                next_sound_ms_ = now_ms + (QueueRandomSound("animal") ? 10000 : 1000);
            }
        }

        // 在线说话期间用 AudioService 的真实 TTS 包络驱动舞动。
        // 包络对语音的每个音节都会产生 attack 尖峰，直接使用会让头/尾巴
        // 抖动，这里把 TTS 包络整体压低并平滑，只保留"随语音轻摆"的幅度。
        if (!offline && speaking_.load()) {
            PandaMotionData tts = audio_->GetTtsMotionData();
            if (tts.playing) {
                tts.level *= 0.6f;
                tts.attack *= 0.45f;
                motion = tts;
            }
        }

        UpdatePlayer(now_ms);
        const bool lively = motion.playing;
        const float life = lively ? 0.55f : 1.0f;
        AddMicroMotion(pose_, now_ms, life);
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
            return Behaviour::kStartled;
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
        case PandaBodyAction::kWagTail:
            return BODY_WAG_TAIL;
        case PandaBodyAction::kTwistNeck:
            return BODY_TWIST_NECK;
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
                   {"wag_tail", PandaBodyAction::kWagTail},
                   {"twist_neck", PandaBodyAction::kTwistNeck}};
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
        case PandaBodyAction::kWagTail:
            return "wag_tail";
        case PandaBodyAction::kTwistNeck:
            return "twist_neck";
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
    SetServoAngles(SERVO_HEAD_DEFAULT, SERVO_TAIL_LR_DEFAULT, SERVO_TAIL_UD_DEFAULT);
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
    // 身体部位指令优先级最高："摇摇头/向左看/摇尾巴/扭扭脖子"等。
    static const struct {
        PandaBodyAction action;
        const char* words[4];
    } body_rules[] = {
        {PandaBodyAction::kShakeHead, {"摇头", "晃晃头", "甩头", "晃头"}},
        {PandaBodyAction::kLookLeft, {"向左看", "看左边", "看左", "左看"}},
        {PandaBodyAction::kLookRight, {"向右看", "看右边", "看右", "右看"}},
        {PandaBodyAction::kWagTail, {"摇尾巴", "甩尾巴", "摆尾巴", "摇摇尾巴"}},
        {PandaBodyAction::kTwistNeck, {"扭脖子", "扭扭脖子", "歪脖子", "转转脖子"}},
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
                "让小熊猫根据用户此刻的情绪做出肢体动作。"
                "设备是一个小熊猫机器人，通过转头、摆尾巴来回应情绪。"
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
                "执行一个明确的小熊猫动作。仅在用户要求动作或当前语境强烈需要肢体表达时使用。action: "
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
                "控制小熊猫的某个身体部位做动作。用户明确要求某个身体部位动作时使用，"
                "例如「摇摇头」→ shake_head，「向左看」→ look_left，「向右看」→ look_right，"
                "「摇摇尾巴」→ wag_tail，「扭扭脖子」→ twist_neck。"
                "action 取值：shake_head, look_left, look_right, wag_tail, twist_neck。",
                PropertyList({Property("action", kPropertyTypeString)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    PandaBodyAction action;
                    if (!ParseBodyAction(properties["action"].value<std::string>(), action)) {
                        return "invalid body action";
                    }
                    TriggerBodyAction(action);
                    return true;
                });
    mcp.AddTool("self.panda.get_mode", "返回小熊猫当前 online 或 offline 模式。", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    return std::string(conversation_active_.load() ? "online" : "offline");
                });
}