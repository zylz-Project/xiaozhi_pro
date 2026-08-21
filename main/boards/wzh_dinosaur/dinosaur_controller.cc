#include "dinosaur_controller.h"

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
#include "dinosaur_audio.h"
#include "mcp_server.h"

#define TAG "DinosaurMotion"

namespace {
constexpr uint32_t kTickMs = 20;
constexpr int kServoMaxDuty = (1 << 13) - 1;
constexpr int kServoPeriodUs = 20000;
// 数组顺序必须与 Pose 字段顺序一致：颈俯仰、颈侧倾、头转向、尾上下、尾左右。
constexpr gpio_num_t kServoPins[] = {SERVO_NECK_TILT_GPIO, SERVO_NECK_LEAN_GPIO,
                                     SERVO_HEAD_TURN_GPIO, SERVO_TAIL_UD_GPIO, SERVO_TAIL_LR_GPIO};
constexpr ledc_channel_t kServoChannels[] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2,
                                             LEDC_CHANNEL_3, LEDC_CHANNEL_4};

uint32_t AngleToDuty(int angle) {
    // 常见 180 度舵机使用 20 ms 周期和约 0.5～2.5 ms 脉宽。
    // 真机若使用不同型号，应先调整脉宽范围，不要只扩大动作角度。
    angle = std::clamp(angle, 0, 180);
    const int pulse_us = 500 + angle * 2000 / 180;
    return pulse_us * (kServoMaxDuty + 1) / kServoPeriodUs;
}

float SmoothStep(float value) {
    // 三次平滑插值让动作起止速度为零，减少舵机突然冲击机械结构。
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

float EaseValue(wzh_dino::MotionEase ease, float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    if (ease == wzh_dino::MotionEase::kFastOut) {
        const float inv = 1.0f - value;
        return 1.0f - inv * inv * inv;
    }
    if (ease == wzh_dino::MotionEase::kLinear) {
        return value;
    }
    return SmoothStep(value);
}

inline float frnd() {
    return static_cast<float>(esp_random()) / 4294967296.0f;
}

int irnd(int count) { return count > 0 ? static_cast<int>(esp_random() % static_cast<uint32_t>(count)) : 0; }
int irand(int low, int high) { return low + irnd(high - low + 1); }

// ============================================================================
// 关键帧编舞库。KEEP 表示该轴沿用上一帧，由此形成“头先动、脖子后随、
// 尾巴最后响应”的动物时序。角度均为本板舵机直接角度：
//   tilt:  0=最高 70=默认 180=最低； lean/head/tail_lr: 90=中位
//   tail_ud: 55=最低 90=默认 180=最高
// ============================================================================
using wzh_dino::Keyframe;
using wzh_dino::MotionClip;
using wzh_dino::MotionEase;
using wzh_dino::kKeep;

constexpr Keyframe kDiscoverFrames[] = {
    {kKeep, kKeep, 125, kKeep, kKeep, 140, MotionEase::kFastOut},   // 头先发现动静
    {kKeep, kKeep, 125, kKeep, kKeep, 170, MotionEase::kLinear},    // 短暂定格
    {55, 115, 118, 130, 65, 320, MotionEase::kSmooth},              // 脖子跟进，尾巴撑住
    {62, 150, 55, 128, 145, 440, MotionEase::kSmooth},              // 巡视一侧地平线
    {78, 34, 142, 128, 40, 480, MotionEase::kSmooth},               // 整条脖子横跨中位
    {100, 105, 85, 138, 115, 380, MotionEase::kSmooth},             // 确认安全，探向用户
    {78, 92, 92, 118, 92, 560, MotionEase::kSmooth},                // 警觉站姿
};

constexpr Keyframe kDiscoverPeekFrames[] = {
    {kKeep, kKeep, 132, kKeep, kKeep, 110, MotionEase::kFastOut},
    {45, 78, 135, 135, 122, 240, MotionEase::kSmooth},              // 缩回 S 形警戒
    {45, 78, 135, 130, 122, 210, MotionEase::kLinear},
    {62, 145, 50, 132, 45, 390, MotionEase::kSmooth},               // 高脖子窥探
    {72, 38, 142, 130, 145, 480, MotionEase::kSmooth},              // 核实另一侧
    {108, 110, 84, 138, 64, 420, MotionEase::kSmooth},              // 好奇前探
    {78, 96, 92, 118, 92, 620, MotionEase::kSmooth},
};

constexpr Keyframe kAffectionFrames[] = {
    {95, 98, 92, 100, 92, 240, MotionEase::kSmooth},                // 先对视
    {125, 112, 80, 130, 68, 480, MotionEase::kSmooth},              // 长颈鞠躬
    {150, 100, 86, 118, 120, 520, MotionEase::kSmooth},             // 额头压向手
    {140, 112, 84, 122, 70, 420, MotionEase::kLinear},              // 贴近停留
    {105, 95, 92, 112, 108, 500, MotionEase::kSmooth},              // 抬起确认用户
    {80, 92, 92, 105, 92, 640, MotionEase::kSmooth},
};

constexpr Keyframe kAffectionNuzzleFrames[] = {
    {92, 100, 92, 102, 92, 220, MotionEase::kSmooth},
    {118, 128, 75, 128, 45, 400, MotionEase::kSmooth},              // 送出一侧脸颊
    {130, 50, 138, 120, 145, 460, MotionEase::kSmooth},             // 一次横跨中位的轻蹭
    {142, 108, 84, 112, 55, 360, MotionEase::kLinear},              // 贴近等待抚摸
    {110, 78, 118, 120, 132, 500, MotionEase::kSmooth},             // 蹭完回看一眼
    {80, 94, 92, 105, 92, 660, MotionEase::kSmooth},
};

constexpr Keyframe kHappyFrames[] = {
    {50, 92, 92, 135, 92, 220, MotionEase::kFastOut},               // 骄傲抬头，尾巴上翘
    {80, 150, 55, 125, 40, 420, MotionEase::kSmooth},               // 大幅展示一侧
    {112, 30, 148, 128, 148, 520, MotionEase::kSmooth},             // 横跨身体的完整摆身
    {135, 105, 84, 130, 60, 380, MotionEase::kSmooth},              // 活泼前探
    {55, 92, 92, 140, 92, 300, MotionEase::kFastOut},               // 再次扬起
    {80, 92, 92, 112, 92, 640, MotionEase::kSmooth},
};

constexpr Keyframe kHappyChaseFrames[] = {
    {kKeep, kKeep, 15, kKeep, kKeep, 110, MotionEase::kFastOut},    // 头先窜出
    {40, 140, 168, kKeep, kKeep, 210, MotionEase::kSmooth},         // 脖子反向跟随
    {65, 172, 30, 165, 20, 340, MotionEase::kSmooth},               // 尾巴最后启动
    {150, 25, 160, 155, 172, 500, MotionEase::kSmooth},             // 大对角线追逐
    {108, 88, 92, 145, 35, 260, MotionEase::kLinear},               // 小惊讶停顿
    {20, 115, 38, 168, 158, 460, MotionEase::kFastOut},             // 向上弹起
    {132, 160, 145, 150, 15, 540, MotionEase::kSmooth},             // 顽皮第二追
    {80, 92, 92, 118, 92, 700, MotionEase::kSmooth},
};

constexpr Keyframe kProudFrames[] = {
    {40, 92, 92, 150, 92, 260, MotionEase::kSmooth},                // 吸气抬颈
    {22, 92, 92, 165, 92, 180, MotionEase::kFastOut},               // 仰天长啸
    {22, 92, 92, 165, 138, 500, MotionEase::kLinear},               // 保持长啸
    {105, 105, 112, 140, 120, 380, MotionEase::kSmooth},            // 巡视一侧
    {100, 78, 70, 135, 60, 420, MotionEase::kSmooth},               // 巡视另一侧
    {85, 92, 92, 112, 92, 600, MotionEase::kSmooth},                // 骄傲收势
};

constexpr Keyframe kEatFrames[] = {
    {125, 100, 95, 105, 100, 380, MotionEase::kSmooth},             // 视线追踪食物下移
    {155, 95, 92, 110, 85, 200, MotionEase::kFastOut},              // 果断第一口
    {135, 100, 95, 108, 110, 240, MotionEase::kSmooth},             // 抬离刚好能嚼
    {158, 100, 100, 110, 78, 200, MotionEase::kFastOut},            // 换侧第二口
    {136, 100, 92, 108, 108, 260, MotionEase::kSmooth},
    {155, 98, 92, 110, 85, 200, MotionEase::kFastOut},              // 最后小口
    {140, 100, 95, 108, 105, 300, MotionEase::kSmooth},
    {125, 95, 92, 105, 92, 360, MotionEase::kSmooth},               // 满足的低位咀嚼
};

constexpr Keyframe kListenFrames[] = {
    {kKeep, kKeep, kKeep, 115, kKeep, 100, MotionEase::kFastOut},   // 尾巴先绷紧
    {68, 92, 122, 112, 92, 220, MotionEase::kFastOut},              // 定位声音方向
    {75, 105, 118, 110, 100, 260, MotionEase::kSmooth},
    {78, 110, 105, 112, 108, 260, MotionEase::kSmooth},             // 侧耳倾听
    {78, 110, 105, 112, 108, 360, MotionEase::kLinear},             // 保持
    {75, 75, 55, 112, 78, 320, MotionEase::kSmooth},                // 检查反方向
    {80, 92, 92, 108, 92, 360, MotionEase::kSmooth},
};

constexpr Keyframe kStartledFrames[] = {
    {kKeep, kKeep, kKeep, kKeep, kKeep, 80, MotionEase::kLinear},   // 定格
    {38, 75, 62, 155, 140, 120, MotionEase::kFastOut},              // 缩颈，尾高撑
    {40, 75, 62, 150, 140, 300, MotionEase::kLinear},               // 评估威胁
    {55, 95, 120, 130, 60, 240, MotionEase::kSmooth},               // 头先检查
    {65, 140, 45, 140, 145, 360, MotionEase::kSmooth},              // 整条脖子定位
    {85, 45, 140, 135, 40, 420, MotionEase::kSmooth},               // 横跨扫描
    {95, 105, 88, 118, 112, 520, MotionEase::kSmooth},              // 谨慎重新探头
    {82, 92, 92, 108, 92, 560, MotionEase::kSmooth},
};

constexpr Keyframe kSleepyFrames[] = {
    {100, 100, 95, 82, 100, 600, MotionEase::kSmooth},              // 眼皮渐沉
    {130, 115, 75, 70, 68, 800, MotionEase::kSmooth},               // 长颈落入 S 形
    {138, 105, 85, 65, 110, 640, MotionEase::kSmooth},
    {128, 78, 122, 68, 75, 800, MotionEase::kSmooth},               // 缓慢呼吸横移
    {140, 100, 90, 62, 105, 900, MotionEase::kSmooth},
    {135, 98, 92, 70, 92, 760, MotionEase::kSmooth},                // 安静伏睡
};

constexpr MotionClip kDiscoverClips[] = {
    {"discover-direct", kDiscoverFrames, std::size(kDiscoverFrames)},
    {"discover-peek", kDiscoverPeekFrames, std::size(kDiscoverPeekFrames)},
};
constexpr MotionClip kAffectionClips[] = {
    {"affection-bow", kAffectionFrames, std::size(kAffectionFrames)},
    {"affection-nuzzle", kAffectionNuzzleFrames, std::size(kAffectionNuzzleFrames)},
};
constexpr MotionClip kHappyClips[] = {
    {"happy-display", kHappyFrames, std::size(kHappyFrames)},
    {"happy-chase", kHappyChaseFrames, std::size(kHappyChaseFrames)},
};
constexpr MotionClip kProudClips[] = {
    {"proud-call", kProudFrames, std::size(kProudFrames)},
};
constexpr MotionClip kEatClips[] = {
    {"eat-peck", kEatFrames, std::size(kEatFrames)},
};
constexpr MotionClip kListenClips[] = {
    {"listen-locate", kListenFrames, std::size(kListenFrames)},
};
constexpr MotionClip kStartledClips[] = {
    {"startled-recoil", kStartledFrames, std::size(kStartledFrames)},
};
constexpr MotionClip kSleepyClips[] = {
    {"sleepy-nod", kSleepyFrames, std::size(kSleepyFrames)},
};

struct ClipSet {
    const MotionClip* clips;
    uint8_t count;
};

constexpr ClipSet kActionClips[] = {
    {kDiscoverClips, 2}, {kAffectionClips, 2}, {kHappyClips, 2}, {kProudClips, 1},
    {kEatClips, 1},      {kListenClips, 1},    {kStartledClips, 1}, {kSleepyClips, 1},
};

// ============================================================================
// 身体部位级动作剪辑：从当前姿态起手，结束时回到默认姿态。方向性动作
// （看左/看右/抬头/低头）不参与随机镜像，保证朝向正确。
// ============================================================================
constexpr Keyframe kBodyShakeHeadFrames[] = {
    {kKeep, kKeep, 55, kKeep, kKeep, 130, MotionEase::kFastOut},
    {kKeep, kKeep, 125, kKeep, kKeep, 130, MotionEase::kFastOut},
    {kKeep, kKeep, 55, kKeep, kKeep, 130, MotionEase::kFastOut},
    {kKeep, kKeep, 125, kKeep, kKeep, 130, MotionEase::kFastOut},
    {kKeep, kKeep, 90, kKeep, kKeep, 420, MotionEase::kSmooth},
};
constexpr Keyframe kBodyLookLeftFrames[] = {
    {kKeep, kKeep, 140, kKeep, kKeep, 320, MotionEase::kFastOut},
    {kKeep, kKeep, 140, kKeep, kKeep, 550, MotionEase::kLinear},
    {kKeep, kKeep, 90, kKeep, kKeep, 420, MotionEase::kSmooth},
};
constexpr Keyframe kBodyLookRightFrames[] = {
    {kKeep, kKeep, 40, kKeep, kKeep, 320, MotionEase::kFastOut},
    {kKeep, kKeep, 40, kKeep, kKeep, 550, MotionEase::kLinear},
    {kKeep, kKeep, 90, kKeep, kKeep, 420, MotionEase::kSmooth},
};
constexpr Keyframe kBodyLookUpFrames[] = {
    {25, kKeep, kKeep, kKeep, kKeep, 300, MotionEase::kFastOut},
    {25, kKeep, kKeep, kKeep, kKeep, 500, MotionEase::kLinear},
    {70, kKeep, kKeep, kKeep, kKeep, 420, MotionEase::kSmooth},
};
constexpr Keyframe kBodyLookDownFrames[] = {
    {120, kKeep, kKeep, kKeep, kKeep, 300, MotionEase::kFastOut},
    {120, kKeep, kKeep, kKeep, kKeep, 500, MotionEase::kLinear},
    {70, kKeep, kKeep, kKeep, kKeep, 420, MotionEase::kSmooth},
};
constexpr Keyframe kBodyWagTailFrames[] = {
    {kKeep, kKeep, kKeep, kKeep, 55, 130, MotionEase::kFastOut},
    {kKeep, kKeep, kKeep, kKeep, 125, 130, MotionEase::kFastOut},
    {kKeep, kKeep, kKeep, kKeep, 55, 130, MotionEase::kFastOut},
    {kKeep, kKeep, kKeep, kKeep, 125, 130, MotionEase::kFastOut},
    {kKeep, kKeep, kKeep, kKeep, 90, 420, MotionEase::kSmooth},
};
constexpr Keyframe kBodyTwistNeckFrames[] = {
    {kKeep, 50, kKeep, kKeep, kKeep, 250, MotionEase::kSmooth},
    {kKeep, 130, kKeep, kKeep, kKeep, 250, MotionEase::kSmooth},
    {kKeep, 50, kKeep, kKeep, kKeep, 250, MotionEase::kSmooth},
    {kKeep, 90, kKeep, kKeep, kKeep, 400, MotionEase::kSmooth},
};

constexpr MotionClip kBodyClips[] = {
    {"body-shake-head", kBodyShakeHeadFrames, std::size(kBodyShakeHeadFrames)},
    {"body-look-left", kBodyLookLeftFrames, std::size(kBodyLookLeftFrames)},
    {"body-look-right", kBodyLookRightFrames, std::size(kBodyLookRightFrames)},
    {"body-look-up", kBodyLookUpFrames, std::size(kBodyLookUpFrames)},
    {"body-look-down", kBodyLookDownFrames, std::size(kBodyLookDownFrames)},
    {"body-wag-tail", kBodyWagTailFrames, std::size(kBodyWagTailFrames)},
    {"body-twist-neck", kBodyTwistNeckFrames, std::size(kBodyTwistNeckFrames)},
};

// 待机"个性姿态"：多轴协同的坐姿，轮换时身体缓慢漂移过去。
struct IdlePose {
    int neck_tilt;
    int neck_lean;
    int head_turn;
    int tail_ud;
    int tail_lr;
};
constexpr IdlePose kIdlePoses[] = {
    {70, 90, 90, 90, 90},     // 平静
    {64, 96, 104, 96, 100},   // 警觉
    {78, 84, 76, 82, 84},     // 困倦
    {60, 100, 112, 102, 74},  // 好奇
    {74, 88, 84, 94, 106},    // 放松
    {66, 94, 96, 98, 96},     // 轻快
};

// 待机“生命脉冲”：每次只做一次观察、甩尾、嗅闻或精神一振，随后完整
// 回落。它们打破待机正弦的可预测性，让静止的恐龙仍有生命感。
constexpr Keyframe kPulseGlanceFrames[] = {
    {kKeep, 100, 118, kKeep, 92, 220, MotionEase::kFastOut},
    {kKeep, 100, 118, kKeep, 92, 160, MotionEase::kLinear},
    {kKeep, 92, 92, kKeep, 92, 420, MotionEase::kSmooth},
};
constexpr Keyframe kPulseTailFrames[] = {
    {kKeep, kKeep, kKeep, kKeep, 118, 90, MotionEase::kFastOut},
    {kKeep, kKeep, kKeep, kKeep, 60, 90, MotionEase::kFastOut},
    {kKeep, kKeep, kKeep, kKeep, 125, 90, MotionEase::kFastOut},
    {kKeep, kKeep, kKeep, kKeep, 68, 90, MotionEase::kFastOut},
    {kKeep, kKeep, kKeep, kKeep, 92, 360, MotionEase::kSmooth},
};
constexpr Keyframe kPulsePerkFrames[] = {
    {58, kKeep, kKeep, 128, kKeep, 160, MotionEase::kFastOut},
    {78, kKeep, kKeep, 105, kKeep, 360, MotionEase::kSmooth},
};
constexpr Keyframe kPulseSniffFrames[] = {
    {85, 95, 95, kKeep, 95, 200, MotionEase::kFastOut},
    {75, 92, 92, kKeep, 92, 160, MotionEase::kSmooth},
    {85, 97, 97, kKeep, 97, 200, MotionEase::kFastOut},
    {78, 92, 92, kKeep, 92, 260, MotionEase::kSmooth},
};

constexpr MotionClip kPulseClips[] = {
    {"pulse-glance", kPulseGlanceFrames, std::size(kPulseGlanceFrames)},
    {"pulse-tail", kPulseTailFrames, std::size(kPulseTailFrames)},
    {"pulse-perk", kPulsePerkFrames, std::size(kPulsePerkFrames)},
    {"pulse-sniff", kPulseSniffFrames, std::size(kPulseSniffFrames)},
};
}  // namespace

bool DinosaurController::Initialize(DinosaurAudio* audio) {
    audio_ = audio;
    InitializeServos();
    action_queue_ = xQueueCreate(4, sizeof(ActionRequest));
    if (action_queue_ == nullptr ||
        xTaskCreate(TaskEntry, "dino_motion", 5120, this, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start dinosaur motion task");
        return false;
    }
    // The board singleton is still being constructed here, so defer local
    // audio until the application and AudioService are fully initialized.
    TriggerAction(DinoAction::kDiscover, false);
    return true;
}

void DinosaurController::InitializeServos() {
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

    const int initial_angles[] = {SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT,
                                  SERVO_HEAD_TURN_DEFAULT, SERVO_TAIL_UD_DEFAULT,
                                  SERVO_TAIL_LR_DEFAULT};
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
    SetPose(current_pose_);
}

void DinosaurController::SetPose(const Pose& requested) {
    Pose pose = requested;
    // 在线说话用 TTS 真实响度、离线本地声音用 Opus 包络：尾巴/头都随
    // 音频舞动，与参考工程的"声音跟随"层一致。
    if (audio_ != nullptr) {
        bool reactive = false;
        float level = 0.0f;
        float attack = 0.0f;
        if (speaking_.load()) {
            // TTS 包络对每个音节都有 attack 尖峰，直接使用会让头/尾抖动，
            // 这里压低幅度只保留"随语音轻摆"的效果。
            level = audio_->GetTtsLevel() * 0.6f;
            attack = audio_->GetTtsAttack() * 0.45f;
            reactive = level > 0.01f;
        } else {
            const DinoMotionData sound = audio_->GetMotionData();
            if (sound.playing) {
                level = sound.level;
                attack = sound.attack;
                reactive = true;
            }
        }
        if (reactive) {
            speak_phase_ += 0.30f + 0.45f * level;
            pose.tail_lr += static_cast<int>(
                std::lround(std::sin(speak_phase_) * std::min(14.0f, level * 26.0f)));
            pose.tail_ud += static_cast<int>(
                std::lround(std::sin(speak_phase_ * 0.7f + 0.6f) * std::min(5.0f, level * 9.0f)));
            if (attack > 0.15f) {
                pose.head_turn += (esp_random() & 1U) ? 4 : -4;
            }
        }
    }
    // 所有入口最终都在这里限幅。尾巴上下轴机械空间最紧，使用板级专用限位。
    pose.neck_tilt = std::clamp(pose.neck_tilt, 0, 180);
    pose.neck_lean = std::clamp(pose.neck_lean, 0, 180);
    pose.head_turn = std::clamp(pose.head_turn, 0, 180);
    pose.tail_ud = std::clamp(pose.tail_ud, SERVO_TAIL_UD_MIN, SERVO_TAIL_UD_MAX);
    pose.tail_lr = std::clamp(pose.tail_lr, 0, 180);
    const int angles[] = {pose.neck_tilt, pose.neck_lean, pose.head_turn, pose.tail_ud,
                          pose.tail_lr};
    for (size_t i = 0; i < std::size(kServoChannels); ++i) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, kServoChannels[i], AngleToDuty(angles[i]));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, kServoChannels[i]);
    }
    current_pose_ = pose;
}

void DinosaurController::MoveTo(const Pose& target, uint32_t duration_ms) {
    // 本函数只由 dino_motion 任务调用，可以使用延时完成连续插值；主任务、
    // 网络回调和音频任务不会在这里等待。
    const Pose start = current_pose_;
    const uint32_t steps = std::max<uint32_t>(1, duration_ms / kTickMs);
    for (uint32_t step = 1; step <= steps; ++step) {
        const float amount = SmoothStep(static_cast<float>(step) / steps);
        Pose pose = {
            .neck_tilt =
                static_cast<int>(start.neck_tilt + (target.neck_tilt - start.neck_tilt) * amount),
            .neck_lean =
                static_cast<int>(start.neck_lean + (target.neck_lean - start.neck_lean) * amount),
            .head_turn =
                static_cast<int>(start.head_turn + (target.head_turn - start.head_turn) * amount),
            .tail_ud = static_cast<int>(start.tail_ud + (target.tail_ud - start.tail_ud) * amount),
            .tail_lr = static_cast<int>(start.tail_lr + (target.tail_lr - start.tail_lr) * amount),
        };
        SetPose(pose);
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

DinosaurController::Pose DinosaurController::AttentionPose(DinoAction action, bool mirror) const {
    // 每个动作先短促地“看向用户/食物”建立互动，再进入编舞。
    Pose pose = {SERVO_NECK_TILT_DEFAULT, 90, SERVO_HEAD_TURN_DEFAULT, 90,
                 SERVO_TAIL_LR_DEFAULT};
    switch (action) {
        case DinoAction::kDiscover:
        case DinoAction::kAffection:
        case DinoAction::kHappy:
            pose.neck_tilt = 100;   // 向正前方用户靠近一点
            pose.tail_ud = 118;
            break;
        case DinoAction::kEat:
            pose.neck_tilt = 112;   // 先看用户手中的食物
            pose.tail_ud = 108;
            break;
        case DinoAction::kProud:
            pose.neck_tilt = 60;
            pose.tail_ud = 140;
            break;
        case DinoAction::kListen:
            pose.neck_tilt = 72;
            pose.tail_ud = 112;
            break;
        case DinoAction::kStartled:
            pose.neck_tilt = 50;
            pose.tail_ud = 150;
            break;
        case DinoAction::kSleepy:
        default:
            pose.neck_tilt = 82;
            pose.tail_ud = 100;
            break;
    }
    if (mirror) {
        pose.neck_lean = 180 - pose.neck_lean;
        pose.head_turn = 180 - pose.head_turn;
        pose.tail_lr = 180 - pose.tail_lr;
    }
    return pose;
}

DinosaurController::Pose DinosaurController::ResolveKeyframe(const wzh_dino::Keyframe& key,
                                                             const Pose& prev, bool mirror,
                                                             float amplitude) const {
    Pose pose = prev;
    auto apply = [amplitude](int& dst, int value) {
        if (value != wzh_dino::kKeep) {
            dst = static_cast<int>(std::lround(90 + (value - 90) * amplitude));
        }
    };
    apply(pose.neck_tilt, key.neck_tilt);
    apply(pose.neck_lean, key.neck_lean);
    apply(pose.head_turn, key.head_turn);
    apply(pose.tail_ud, key.tail_ud);
    apply(pose.tail_lr, key.tail_lr);
    if (mirror) {
        pose.neck_lean = 180 - pose.neck_lean;
        pose.head_turn = 180 - pose.head_turn;
        pose.tail_lr = 180 - pose.tail_lr;
    }
    return pose;
}

const wzh_dino::MotionClip* DinosaurController::PickClip(DinoAction action) const {
    const ClipSet& set = kActionClips[static_cast<uint8_t>(action)];
    return &set.clips[esp_random() % set.count];
}

const wzh_dino::MotionClip* DinosaurController::PickPulse() const {
    return &kPulseClips[esp_random() % std::size(kPulseClips)];
}

void DinosaurController::ApplyLiving(Pose& pose, float living_phase) const {
    // 动作进行中叠加的“呼吸/微摆”，频率互质，避免五个关节机械同步。
    const float seconds = esp_timer_get_time() / 1000000.0f;
    pose.neck_tilt += static_cast<int>(std::lround(std::sin(seconds * 2.1f + living_phase) * 2.0f));
    pose.neck_lean +=
        static_cast<int>(std::lround(std::sin(seconds * 1.7f + living_phase * 1.3f + 0.8f) * 1.5f));
    pose.head_turn +=
        static_cast<int>(std::lround(std::sin(seconds * 2.6f + living_phase * 0.7f) * 1.5f));
    pose.tail_ud +=
        static_cast<int>(std::lround(std::sin(seconds * 1.9f + living_phase * 1.7f) * 3.0f));
    pose.tail_lr +=
        static_cast<int>(std::lround(std::sin(seconds * 1.4f + living_phase * 2.3f + 0.5f) * 4.0f));
}

void DinosaurController::PlaySegment(const Pose& from, const Pose& to, uint32_t duration_ms,
                                     wzh_dino::MotionEase ease, float living_phase) {
    const uint32_t steps = std::max<uint32_t>(1, duration_ms / kTickMs);
    for (uint32_t step = 1; step <= steps; ++step) {
        const float amount = EaseValue(ease, static_cast<float>(step) / steps);
        Pose pose = {
            .neck_tilt =
                static_cast<int>(std::lround(from.neck_tilt + (to.neck_tilt - from.neck_tilt) * amount)),
            .neck_lean =
                static_cast<int>(std::lround(from.neck_lean + (to.neck_lean - from.neck_lean) * amount)),
            .head_turn = static_cast<int>(
                std::lround(from.head_turn + (to.head_turn - from.head_turn) * amount)),
            .tail_ud =
                static_cast<int>(std::lround(from.tail_ud + (to.tail_ud - from.tail_ud) * amount)),
            .tail_lr =
                static_cast<int>(std::lround(from.tail_lr + (to.tail_lr - from.tail_lr) * amount)),
        };
        ApplyLiving(pose, living_phase);
        SetPose(pose);
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

void DinosaurController::RunAction(DinoAction action) {
    // 每次表演都带随机变奏：镜像方向、幅度和节奏，避免复读同一套动作。
    const bool mirror = (esp_random() & 1U) != 0;
    const float amplitude = 0.92f + frnd() * 0.18f;   // 0.92~1.10
    const float tempo = 0.90f + frnd() * 0.25f;       // 0.90~1.15，>1 更快
    const float living_phase = frnd() * 6.2832f;
    const wzh_dino::MotionClip* clip = PickClip(action);
    ESP_LOGI(TAG, "Action: %s%s amp=%.2f tempo=%.2f", clip->name, mirror ? " (mirror)" : "",
             static_cast<double>(amplitude), static_cast<double>(tempo));

    Pose pose = AttentionPose(action, mirror);
    PlaySegment(current_pose_, pose, 240, wzh_dino::MotionEase::kSmooth, living_phase);

    for (uint8_t i = 0; i < clip->count; ++i) {
        const wzh_dino::Keyframe& key = clip->frames[i];
        const Pose target = ResolveKeyframe(key, pose, mirror, amplitude);
        const uint32_t duration =
            static_cast<uint32_t>(std::lround(key.duration_ms / tempo));
        PlaySegment(pose, target, duration, key.ease, living_phase);
        pose = target;
    }

    // 收势：自然回到板级默认姿态，不再硬性定格。
    const Pose rest = {SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT, SERVO_HEAD_TURN_DEFAULT,
                       SERVO_TAIL_UD_DEFAULT, SERVO_TAIL_LR_DEFAULT};
    PlaySegment(pose, rest, 500, wzh_dino::MotionEase::kSmooth, living_phase);
}

void DinosaurController::RunPulse(const wzh_dino::MotionClip& clip) {
    const bool mirror = (esp_random() & 1U) != 0;
    const float amplitude = 0.90f + frnd() * 0.30f;
    const float tempo = 0.90f + frnd() * 0.30f;
    const float living_phase = frnd() * 6.2832f;
    ESP_LOGI(TAG, "Pulse: %s%s", clip.name, mirror ? " (mirror)" : "");
    Pose pose = current_pose_;
    for (uint8_t i = 0; i < clip.count; ++i) {
        const wzh_dino::Keyframe& key = clip.frames[i];
        const Pose target = ResolveKeyframe(key, pose, mirror, amplitude);
        const uint32_t duration =
            static_cast<uint32_t>(std::lround(key.duration_ms / tempo));
        PlaySegment(pose, target, duration, key.ease, living_phase);
        pose = target;
    }
}

void DinosaurController::RunIdlePose(uint32_t tick) {
    // 每 4~8 秒轮换一个"个性姿态"，再叠加呼吸正弦，让待机有真实感。
    if (tick >= next_idle_pose_tick_) {
        idle_pose_index_ = (idle_pose_index_ + 1 + irnd(static_cast<int>(std::size(kIdlePoses)) - 1)) %
                           static_cast<int>(std::size(kIdlePoses));
        next_idle_pose_tick_ = tick + static_cast<uint32_t>(irand(200, 400));
    }
    const IdlePose& target = kIdlePoses[idle_pose_index_];
    const float k = 0.018f;
    idle_drift_[0] += (static_cast<float>(target.neck_tilt) - idle_drift_[0]) * k;
    idle_drift_[1] += (static_cast<float>(target.neck_lean) - idle_drift_[1]) * k;
    idle_drift_[2] += (static_cast<float>(target.head_turn) - idle_drift_[2]) * k;
    idle_drift_[3] += (static_cast<float>(target.tail_ud) - idle_drift_[3]) * k;
    idle_drift_[4] += (static_cast<float>(target.tail_lr) - idle_drift_[4]) * k;
    // 呼吸微动叠加在个性姿态上。
    const float seconds = tick * kTickMs / 1000.0f;
    Pose pose = {
        static_cast<int>(std::lround(idle_drift_[0] + 4 * std::sin(seconds * 1.3f) +
                                     2 * std::sin(seconds * 0.37f + 0.8f))),
        static_cast<int>(std::lround(idle_drift_[1] + 6 * std::sin(seconds * 0.53f + 0.5f) +
                                     2 * std::sin(seconds * 1.9f))),
        static_cast<int>(std::lround(idle_drift_[2] + 5 * std::sin(seconds * 0.77f + 1.1f))),
        static_cast<int>(std::lround(idle_drift_[3] + 8 * std::sin(seconds * 0.61f) +
                                     3 * std::sin(seconds * 2.3f + 0.4f))),
        static_cast<int>(std::lround(idle_drift_[4] + 12 * std::sin(seconds * 0.83f + 0.7f) +
                                     4 * std::sin(seconds * 2.7f + 1.5f))),
    };
    SetPose(pose);
}

void DinosaurController::TaskEntry(void* arg) { static_cast<DinosaurController*>(arg)->Task(); }

void DinosaurController::Task() {
    uint32_t tick = 0;
    // 开机后尽快执行第一个动作并播放声音（开机叫声），之后由自主调度继续。
    int64_t next_offline_action = esp_timer_get_time() + 2 * 1000000LL;
    next_pulse_ms_ = 3000 + esp_random() % 5000;
    while (true) {
        ActionRequest request;
        // 明确请求优先级最高。在线请求不播放本地音频，以免和 TTS 重叠。
        if (xQueueReceive(action_queue_, &request, 0) == pdTRUE) {
            if (request.is_body) {
                // 身体部位指令：直接播放对应剪辑，不随机镜像/幅度。
                RunBodyAction(static_cast<DinoBodyAction>(request.action));
            } else {
                if (request.with_local_sound && !conversation_active_.load() && audio_ != nullptr) {
                    audio_->PlayForAction(request.action);
                }
                RunAction(request.action);
            }
            // 开机引导动作(kDiscover)不推迟开机叫声，其余动作给足间隔。
            if (request.action != DinoAction::kDiscover) {
                next_offline_action = esp_timer_get_time() + (5 + esp_random() % 6) * 1000000LL;
            }
            // 动作结束后让随机脉冲也延后，避免“动作+脉冲”连轴转。
            next_pulse_ms_ = esp_timer_get_time() / 1000 + 4000 + esp_random() % 8000;
        } else if (!conversation_active_.load() && esp_timer_get_time() >= next_offline_action) {
            // 离线时定期随机动作，让设备在没有服务器的情况下仍有生命感。
            // 权重偏向活泼/亲近，减少发呆。
            static const DinoAction actions[] = {
                DinoAction::kDiscover, DinoAction::kAffection, DinoAction::kHappy,
                DinoAction::kProud,    DinoAction::kEat,       DinoAction::kListen,
                DinoAction::kSleepy,   DinoAction::kDiscover,  DinoAction::kAffection,
                DinoAction::kHappy};
            const DinoAction action = actions[esp_random() % std::size(actions)];
            if (audio_ != nullptr) {
                audio_->PlayForAction(action);
            }
            RunAction(action);
            next_offline_action = esp_timer_get_time() + (5 + esp_random() % 8) * 1000000LL;
        } else {
            // 待机时插入随机的“生命脉冲”：转头、甩尾、嗅闻、精神一振。
            // 在线聆听/说话时同样保留，让恐龙在对话中也有小表情。
            const uint32_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms >= next_pulse_ms_) {
                RunPulse(*PickPulse());
                next_pulse_ms_ = now_ms + 5000 + esp_random() % 9000;
            }
            RunIdlePose(tick++);
            vTaskDelay(pdMS_TO_TICKS(kTickMs));
        }
    }
}

bool DinosaurController::TriggerAction(DinoAction action, bool with_local_sound) {
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

bool DinosaurController::TriggerBodyAction(DinoBodyAction action) {
    if (action_queue_ == nullptr) {
        return false;
    }
    ESP_LOGI(TAG, "Body action: %s", BodyActionName(action));
    ActionRequest request{static_cast<DinoAction>(action), false, true};
    if (xQueueSend(action_queue_, &request, 0) == pdTRUE) {
        return true;
    }
    ActionRequest discarded;
    xQueueReceive(action_queue_, &discarded, 0);
    return xQueueSend(action_queue_, &request, 0) == pdTRUE;
}

const wzh_dino::MotionClip* DinosaurController::PickBodyClip(DinoBodyAction action) {
    return &kBodyClips[static_cast<uint8_t>(action)];
}

void DinosaurController::RunBodyAction(DinoBodyAction action) {
    const wzh_dino::MotionClip* clip = PickBodyClip(action);
    ESP_LOGI(TAG, "Body action: %s", clip->name);
    const float living_phase = frnd() * 6.2832f;
    Pose pose = current_pose_;
    for (uint8_t i = 0; i < clip->count; ++i) {
        const wzh_dino::Keyframe& key = clip->frames[i];
        // 不随机镜像/幅度，保证"向左看"确实是向左看。
        const Pose target = ResolveKeyframe(key, pose, false, 1.0f);
        PlaySegment(pose, target, key.duration_ms, key.ease, living_phase);
        pose = target;
    }
    const Pose rest = {SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT, SERVO_HEAD_TURN_DEFAULT,
                       SERVO_TAIL_UD_DEFAULT, SERVO_TAIL_LR_DEFAULT};
    PlaySegment(pose, rest, 360, wzh_dino::MotionEase::kSmooth, living_phase);
}

bool DinosaurController::ParseBodyAction(const std::string& name, DinoBodyAction& action) {
    static const struct {
        const char* name;
        DinoBodyAction action;
    } actions[] = {{"shake_head", DinoBodyAction::kShakeHead},
                   {"look_left", DinoBodyAction::kLookLeft},
                   {"look_right", DinoBodyAction::kLookRight},
                   {"look_up", DinoBodyAction::kLookUp},
                   {"look_down", DinoBodyAction::kLookDown},
                   {"wag_tail", DinoBodyAction::kWagTail},
                   {"twist_neck", DinoBodyAction::kTwistNeck}};
    for (const auto& item : actions) {
        if (name == item.name) {
            action = item.action;
            return true;
        }
    }
    return false;
}

const char* DinosaurController::BodyActionName(DinoBodyAction action) {
    switch (action) {
        case DinoBodyAction::kShakeHead:
            return "shake_head";
        case DinoBodyAction::kLookLeft:
            return "look_left";
        case DinoBodyAction::kLookRight:
            return "look_right";
        case DinoBodyAction::kLookUp:
            return "look_up";
        case DinoBodyAction::kLookDown:
            return "look_down";
        case DinoBodyAction::kWagTail:
            return "wag_tail";
        case DinoBodyAction::kTwistNeck:
            return "twist_neck";
        default:
            break;
    }
    return "shake_head";
}

void DinosaurController::SetConversationActive(bool active) {
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
    TriggerAction(active ? DinoAction::kListen : DinoAction::kDiscover, !active);
}

void DinosaurController::SetSpeaking(bool speaking) {
    speaking_.store(speaking);
}

void DinosaurController::SetDefaultPose() {
    const Pose rest = {SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT, SERVO_HEAD_TURN_DEFAULT,
                       SERVO_TAIL_UD_DEFAULT, SERVO_TAIL_LR_DEFAULT};
    SetPose(rest);
}

DinoAction DinosaurController::EmotionToAction(const std::string& emotion) const {
    // 同时兼容 XiaoZhi 常见的 llm.emotion 名称和 MCP 推荐的基础情绪名称。
    if (emotion == "happy" || emotion == "laughing" || emotion == "winking" ||
        emotion == "funny" || emotion == "excited" || emotion == "delighted" ||
        emotion == "joyful" || emotion == "proud") {
        return DinoAction::kHappy;
    }
    if (emotion == "loving" || emotion == "embarrassed" || emotion == "relaxed" ||
        emotion == "grateful" || emotion == "gratitude" || emotion == "love") {
        return DinoAction::kAffection;
    }
    if (emotion == "sad" || emotion == "crying" || emotion == "sleepy" ||
        emotion == "tired" || emotion == "bored" || emotion == "depressed" ||
        emotion == "lonely") {
        return DinoAction::kSleepy;
    }
    if (emotion == "angry" || emotion == "confident" || emotion == "cool" ||
        emotion == "annoyed" || emotion == "frustrated") {
        return DinoAction::kProud;
    }
    if (emotion == "surprised" || emotion == "shocked" || emotion == "fearful" ||
        emotion == "scared" || emotion == "worried") {
        return DinoAction::kStartled;
    }
    if (emotion == "thinking" || emotion == "confused" || emotion == "curious") {
        return DinoAction::kDiscover;
    }
    return DinoAction::kDiscover;
}

void DinosaurController::HandleLlmEmotion(const char* emotion) {
    // 离线时 llm.emotion 已失去时效性，应继续由自主逻辑控制动作。
    if (!conversation_active_.load() || emotion == nullptr || emotion[0] == '\0') {
        return;
    }
    // neutral 是状态机/界面的复位信号（进入聆听、回到待机都会下发），
    // 不是可表演的情绪。忽略它，避免每次说完话都被塞一次无意义动作。
    if (std::strcmp(emotion, "neutral") == 0 || std::strcmp(emotion, "normal") == 0) {
        return;
    }
    TriggerAction(EmotionToAction(emotion), false);
}

void DinosaurController::HandleUserSpeechEmotion(const char* text) {
    if (!conversation_active_.load() || text == nullptr || text[0] == '\0') {
        return;
    }
    // 身体部位指令优先级最高："摇摇头/向左看/抬头/低头/摇尾巴/扭脖子"等。
    static const struct {
        DinoBodyAction action;
        const char* words[4];
    } body_rules[] = {
        {DinoBodyAction::kShakeHead, {"摇头", "晃晃头", "甩头", "晃头"}},
        {DinoBodyAction::kLookLeft, {"向左看", "看左边", "看左", "左看"}},
        {DinoBodyAction::kLookRight, {"向右看", "看右边", "看右", "右看"}},
        {DinoBodyAction::kLookUp, {"抬头", "看看上面", "看上面", "仰望"}},
        {DinoBodyAction::kLookDown, {"低头", "看看下面", "看下面", "俯视"}},
        {DinoBodyAction::kWagTail, {"摇尾巴", "甩尾巴", "摆尾巴", "摇摇尾巴"}},
        {DinoBodyAction::kTwistNeck, {"扭脖子", "扭扭脖子", "歪脖子", "转转脖子"}},
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
        DinoAction action;
        const char* words[6];
    } rules[] = {
        {DinoAction::kHappy, {"满分", "太棒", "真棒", "厉害", "恭喜", "太好了"}},
        {DinoAction::kHappy, {"开心", "高兴", "惊喜", "嘻嘻", "哈哈", "万岁"}},
        {DinoAction::kSleepy, {"难过", "伤心", "想哭", "委屈", "难受", "不开心"}},
        {DinoAction::kProud, {"生气", "气死", "讨厌", "可恶", "气愤", "恼火"}},
        {DinoAction::kAffection, {"爱你", "喜欢你", "想你", "亲亲", "抱抱", "好想你"}},
        {DinoAction::kStartled, {"吓死", "吓到", "天啊", "震惊", "没想到", "吃惊"}},
        {DinoAction::kSleepy, {"困了", "好困", "睡觉", "晚安", "好累", "累死"}},
        {DinoAction::kEat, {"吃饭", "好吃", "饿了", "好饿", "干饭", "美味"}},
        {DinoAction::kProud, {"骄傲", "自豪", "炫耀", "冠军", "第一", "赢了"}},
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

bool DinosaurController::ParseAction(const std::string& name, DinoAction& action) {
    static const struct {
        const char* name;
        DinoAction action;
    } actions[] = {{"discover", DinoAction::kDiscover}, {"affection", DinoAction::kAffection},
                   {"happy", DinoAction::kHappy},       {"proud", DinoAction::kProud},
                   {"eat", DinoAction::kEat},           {"listen", DinoAction::kListen},
                   {"startled", DinoAction::kStartled}, {"sleepy", DinoAction::kSleepy}};
    for (const auto& item : actions) {
        if (name == item.name) {
            action = item.action;
            return true;
        }
    }
    return false;
}

const char* DinosaurController::ActionName(DinoAction action) {
    switch (action) {
        case DinoAction::kDiscover:
            return "discover";
        case DinoAction::kAffection:
            return "affection";
        case DinoAction::kHappy:
            return "happy";
        case DinoAction::kProud:
            return "proud";
        case DinoAction::kEat:
            return "eat";
        case DinoAction::kListen:
            return "listen";
        case DinoAction::kStartled:
            return "startled";
        case DinoAction::kSleepy:
            return "sleepy";
    }
    return "unknown";
}

void DinosaurController::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    // react_to_user_emotion 面向模型语义判断；action 面向明确动作请求；
    // get_mode 让模型在需要时先确认设备是否在线。
    mcp.AddTool("self.dinosaur.react_to_user_emotion",
                "让恐龙根据用户此刻的情绪做出肢体动作。"
                "设备是一个恐龙机器人，通过摇头、伸脖子、摆尾巴来回应情绪。"
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
    mcp.AddTool("self.dinosaur.action",
                "执行一个明确的恐龙动作。仅在用户要求动作或当前语境强烈需要肢体表达时使用。action: "
                "discover, affection, happy, proud, eat, listen, startled, sleepy。",
                PropertyList({Property("action", kPropertyTypeString)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    DinoAction action;
                    if (!ParseAction(properties["action"].value<std::string>(), action)) {
                        return "invalid action";
                    }
                    TriggerAction(action, !conversation_active_.load());
                    return true;
                });
    mcp.AddTool("self.dinosaur.body",
                "控制恐龙的某个身体部位做动作。用户明确要求某个身体部位动作时使用，"
                "例如「摇摇头」→ shake_head，「向左看」→ look_left，「向右看」→ look_right，"
                "「抬头」→ look_up，「低头」→ look_down，「摇摇尾巴」→ wag_tail，"
                "「扭扭脖子」→ twist_neck。"
                "action 取值：shake_head, look_left, look_right, look_up, look_down, wag_tail, twist_neck。",
                PropertyList({Property("action", kPropertyTypeString)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    DinoBodyAction action;
                    if (!ParseBodyAction(properties["action"].value<std::string>(), action)) {
                        return "invalid body action";
                    }
                    TriggerBodyAction(action);
                    return true;
                });
    mcp.AddTool("self.dinosaur.get_mode", "返回恐龙当前 online 或 offline 模式。", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    return std::string(conversation_active_.load() ? "online" : "offline");
                });
}
