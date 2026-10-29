#include "dinosaur_controller.h"
#include "wzh_body_commands.h"
#include "wzh_pet_feedback.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <iterator>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include "application.h"
#include "config.h"
#include "dinosaur_audio.h"
#include "mcp_server.h"
#include "wzh_sensors.h"

#define TAG "DinosaurMotion"

namespace {
constexpr uint32_t kTickMs = 20;
// 待机动作不需要按每个 PWM 周期重新计算。舵机 PWM 仍保持标准 50 Hz，
// 这里只把姿态目标的刷新降到 20 Hz，让缓慢漂移更安静、也减少无效写入。
constexpr uint32_t kIdleUpdateMs = 50;
constexpr size_t kMaxClipFrames = 16;
// Legacy story validation and the minimum moving frame for new long-neck stories.
constexpr uint32_t kMinAuthoredStoryFrameMs = 420;
// 编舞中的每一次实际位移都受这两个参数约束。关键帧可以决定姿态和停留时间，
// 但不能让任一舵机以高频率来回扫动。

// 数组顺序必须与 Pose 字段顺序一致：颈俯仰、颈侧倾、头转向、尾上下、尾左右。
constexpr gpio_num_t kServoPins[] = {SERVO_NECK_TILT_GPIO, SERVO_NECK_LEAN_GPIO,
                                     SERVO_HEAD_TURN_GPIO, SERVO_TAIL_UD_GPIO, SERVO_TAIL_LR_GPIO};
constexpr ledc_channel_t kServoChannels[] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2,
                                             LEDC_CHANNEL_3, LEDC_CHANNEL_4};

float SmoothStep(float value) {
    // 恢复产品原有的三次平滑手感：起止速度仍为零，中段比五次曲线
    // 更快建立姿态，避免整条恐龙看起来绵软、迟钝。
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

float EaseValue(wzh_dino::MotionEase ease, float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    if (ease == wzh_dino::MotionEase::kFastOut) {
        // 保留少量“先到位、后观察”的动物感，但整体时长仍受统一限速，
        // 且与普通曲线一样从零速度起步、以零速度停止。
        const float smooth = SmoothStep(value);
        const float early = 1.0f - (1.0f - smooth) * (1.0f - smooth);
        return smooth * 0.68f + early * 0.32f;
    }
    if (ease == wzh_dino::MotionEase::kLinear) {
        return value;
    }
    return SmoothStep(value);
}

inline float frnd() { return static_cast<float>(esp_random()) / 4294967296.0f; }

int irnd(int count) {
    return count > 0 ? static_cast<int>(esp_random() % static_cast<uint32_t>(count)) : 0;
}
int irand(int low, int high) { return low + irnd(high - low + 1); }

bool ContainsText(const char* text, const char* token) {
    return text != nullptr && token != nullptr && std::strstr(text, token) != nullptr;
}

float BreathingWave(float seconds) {
    // 12 秒一轮：缓慢吸气、缓慢呼气，最后约 4 秒停在中位。与一直
    // 摇摆的正弦相比，这段自然停顿更像动物呼吸，也保证默认姿态常回中。
    const float phase = std::fmod(seconds, 12.0f) / 12.0f;
    if (phase < 0.30f) {
        return SmoothStep(phase / 0.30f);
    }
    if (phase < 0.64f) {
        return 1.0f - SmoothStep((phase - 0.30f) / 0.34f);
    }
    return 0.0f;
}

// ============================================================================
// 关键帧编舞库。KEEP 表示该轴在这一帧维持上一姿态；同帧内有变化的轴
// 共用插值进度并同步起停。角度均为本板舵机直接角度：
//   tilt: 0=最高/仰，70=默认，180=最低/俯
//   lean/head: 0=右，90=中位，180=左
//   tail_lr: 0=左，90=中位，180=右（与 lean/head 的左右方向相反）
//   tail_ud: 55=最低，90=默认，180=最高
// ============================================================================
using wzh_dino::Keyframe;
using wzh_dino::kKeep;
using wzh_dino::MotionClip;
using wzh_dino::MotionEase;

// 旧工程动作表只有 tail_ud 使用反向内部坐标：0=尾巴最高、125=最低；
// 当前引擎的关键帧全部是舵机实际角度：180=最高、55=最低。其余四轴
// 不做这个转换。显式保留函数，避免移植旧动作时把 0/180 方向抄反。
constexpr int LegacyTailUd(int legacy_angle) { return 180 - legacy_angle; }
static_assert(LegacyTailUd(0) == 180 && LegacyTailUd(125) == 55);

constexpr Keyframe kDiscoverFrames[] = {
    {kKeep, kKeep, 125, kKeep, kKeep, 140, MotionEase::kFastOut},  // 头先发现动静
    {kKeep, kKeep, 125, kKeep, kKeep, 170, MotionEase::kLinear},   // 短暂定格
    {55, 115, 118, 130, 65, 320, MotionEase::kSmooth},             // 脖子跟进，尾巴撑住
    {62, 150, 55, 128, 145, 440, MotionEase::kSmooth},             // 巡视一侧地平线
    {78, 34, 142, 128, 40, 480, MotionEase::kSmooth},              // 整条脖子横跨中位
    {100, 105, 85, 138, 115, 380, MotionEase::kSmooth},            // 确认安全，探向用户
    {78, 92, 92, 118, 92, 560, MotionEase::kSmooth},               // 警觉站姿
};

constexpr Keyframe kDiscoverPeekFrames[] = {
    {kKeep, kKeep, 132, kKeep, kKeep, 110, MotionEase::kFastOut},
    {45, 78, 135, 135, 122, 240, MotionEase::kSmooth},  // 缩回 S 形警戒
    {45, 78, 135, 130, 122, 210, MotionEase::kLinear},
    {62, 145, 50, 132, 45, 390, MotionEase::kSmooth},   // 高脖子窥探
    {72, 38, 142, 130, 145, 480, MotionEase::kSmooth},  // 核实另一侧
    {108, 110, 84, 138, 64, 420, MotionEase::kSmooth},  // 好奇前探
    {78, 96, 92, 118, 92, 620, MotionEase::kSmooth},
};

constexpr Keyframe kAffectionFrames[] = {
    {95, 98, 92, 100, 92, 240, MotionEase::kSmooth},     // 先对视
    {125, 112, 80, 130, 68, 480, MotionEase::kSmooth},   // 长颈鞠躬
    {150, 100, 86, 118, 120, 520, MotionEase::kSmooth},  // 额头压向手
    {140, 112, 84, 122, 70, 420, MotionEase::kLinear},   // 贴近停留
    {105, 95, 92, 112, 108, 500, MotionEase::kSmooth},   // 抬起确认用户
    {80, 92, 92, 105, 92, 640, MotionEase::kSmooth},
};

constexpr Keyframe kAffectionNuzzleFrames[] = {
    {92, 100, 92, 102, 92, 220, MotionEase::kSmooth},
    {118, 128, 75, 128, 45, 400, MotionEase::kSmooth},   // 送出一侧脸颊
    {130, 50, 138, 120, 145, 460, MotionEase::kSmooth},  // 一次横跨中位的轻蹭
    {142, 108, 84, 112, 55, 360, MotionEase::kLinear},   // 贴近等待抚摸
    {110, 78, 118, 120, 132, 500, MotionEase::kSmooth},  // 蹭完回看一眼
    {80, 94, 92, 105, 92, 660, MotionEase::kSmooth},
};

constexpr Keyframe kHappyFrames[] = {
    {50, 92, 92, 135, 92, 220, MotionEase::kFastOut},    // 骄傲抬头，尾巴上翘
    {80, 150, 55, 125, 40, 420, MotionEase::kSmooth},    // 大幅展示一侧
    {112, 30, 148, 128, 148, 520, MotionEase::kSmooth},  // 横跨身体的完整摆身
    {135, 105, 84, 130, 60, 380, MotionEase::kSmooth},   // 活泼前探
    {55, 92, 92, 140, 92, 300, MotionEase::kFastOut},    // 再次扬起
    {80, 92, 92, 112, 92, 640, MotionEase::kSmooth},
};

constexpr Keyframe kHappyChaseFrames[] = {
    {kKeep, kKeep, 15, kKeep, kKeep, 110, MotionEase::kFastOut},  // 头先窜出
    {40, 140, 168, kKeep, kKeep, 210, MotionEase::kSmooth},       // 脖子反向跟随
    {65, 172, 30, 165, 20, 340, MotionEase::kSmooth},             // 尾巴最后启动
    {150, 25, 160, 155, 172, 500, MotionEase::kSmooth},           // 大对角线追逐
    {108, 88, 92, 145, 35, 260, MotionEase::kLinear},             // 小惊讶停顿
    {20, 115, 38, 168, 158, 460, MotionEase::kFastOut},           // 向上弹起
    {132, 160, 145, 150, 15, 540, MotionEase::kSmooth},           // 顽皮第二追
    {80, 92, 92, 118, 92, 700, MotionEase::kSmooth},
};

constexpr Keyframe kProudFrames[] = {
    {40, 92, 92, 150, 92, 260, MotionEase::kSmooth},      // 吸气抬颈
    {22, 92, 92, 165, 92, 180, MotionEase::kFastOut},     // 仰天长啸
    {22, 92, 92, 165, 138, 500, MotionEase::kLinear},     // 保持长啸
    {105, 105, 112, 140, 120, 380, MotionEase::kSmooth},  // 巡视一侧
    {100, 78, 70, 135, 60, 420, MotionEase::kSmooth},     // 巡视另一侧
    {85, 92, 92, 112, 92, 600, MotionEase::kSmooth},      // 骄傲收势
};

constexpr Keyframe kEatFrames[] = {
    {125, 100, 95, 105, 100, 380, MotionEase::kSmooth},   // 视线追踪食物下移
    {155, 95, 92, 110, 85, 200, MotionEase::kFastOut},    // 果断第一口
    {135, 100, 95, 108, 110, 240, MotionEase::kSmooth},   // 抬离刚好能嚼
    {158, 100, 100, 110, 78, 200, MotionEase::kFastOut},  // 换侧第二口
    {136, 100, 92, 108, 108, 260, MotionEase::kSmooth},
    {155, 98, 92, 110, 85, 200, MotionEase::kFastOut},  // 最后小口
    {140, 100, 95, 108, 105, 300, MotionEase::kSmooth},
    {125, 95, 92, 105, 92, 360, MotionEase::kSmooth},  // 满足的低位咀嚼
};

constexpr Keyframe kListenFrames[] = {
    {kKeep, kKeep, kKeep, 115, kKeep, 100, MotionEase::kFastOut},  // 尾巴先绷紧
    {68, 92, 122, 112, 92, 220, MotionEase::kFastOut},             // 定位声音方向
    {75, 105, 118, 110, 100, 260, MotionEase::kSmooth},
    {78, 110, 105, 112, 108, 260, MotionEase::kSmooth},  // 侧耳倾听
    {78, 110, 105, 112, 108, 360, MotionEase::kLinear},  // 保持
    {75, 75, 55, 112, 78, 320, MotionEase::kSmooth},     // 检查反方向
    {80, 92, 92, 108, 92, 360, MotionEase::kSmooth},
};

constexpr Keyframe kStartledFrames[] = {
    {kKeep, kKeep, kKeep, kKeep, kKeep, 80, MotionEase::kLinear},  // 定格
    {38, 75, 62, 155, 140, 120, MotionEase::kFastOut},             // 缩颈，尾高撑
    {40, 75, 62, 150, 140, 300, MotionEase::kLinear},              // 评估威胁
    {55, 95, 120, 130, 60, 240, MotionEase::kSmooth},              // 头先检查
    {65, 140, 45, 140, 145, 360, MotionEase::kSmooth},             // 整条脖子定位
    {85, 45, 140, 135, 40, 420, MotionEase::kSmooth},              // 横跨扫描
    {95, 105, 88, 118, 112, 520, MotionEase::kSmooth},             // 谨慎重新探头
    {82, 92, 92, 108, 92, 560, MotionEase::kSmooth},
};

// A sneeze drops the long neck toward the ground, holds briefly, then snaps
// back up with a tail counterweight. This is intentionally different from a
// startled scan.
constexpr Keyframe kSneezeFrames[] = {
    {70, 90, 90, 150, 90, 240, MotionEase::kSmooth},
    {150, 90, 90, 162, 90, 340, MotionEase::kFastOut},
    {160, 90, 90, 152, 90, 300, MotionEase::kLinear},
    {26, 88, 92, 132, 96, 500, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 760, MotionEase::kSmooth},
};
constexpr MotionClip kSneezeClip = {"sneeze-down", kSneezeFrames, std::size(kSneezeFrames)};

constexpr Keyframe kSleepyFrames[] = {    {100, 100, 95, 82, 100, 600, MotionEase::kSmooth},  // 眼皮渐沉
    {130, 115, 75, 70, 68, 800, MotionEase::kSmooth},   // 长颈落入 S 形
    {138, 105, 85, 65, 110, 640, MotionEase::kSmooth},
    {128, 78, 122, 68, 75, 800, MotionEase::kSmooth},  // 缓慢呼吸横移
    {140, 100, 90, 62, 105, 900, MotionEase::kSmooth},
    {135, 98, 92, 70, 92, 760, MotionEase::kSmooth},  // 安静伏睡
};

// 第二批完整编舞：保留旧动作不动，为每个行为族再增加一个明显不同的故事。
constexpr Keyframe kDiscoverTrackFrames[] = {
    {58, 96, 116, 124, 68, 620, MotionEase::kSmooth},   // 抬颈捕捉远处动静
    {58, 96, 116, 124, 68, 420, MotionEase::kLinear},   // 凝视确认
    {72, 132, 142, 132, 48, 760, MotionEase::kSmooth},  // 全身同步转向一侧
    {82, 52, 42, 126, 142, 920, MotionEase::kSmooth},   // 沿地平线追到另一侧
    {108, 104, 82, 118, 72, 720, MotionEase::kSmooth},  // 向前靠近观察
    {78, 92, 92, 112, 92, 760, MotionEase::kSmooth},    // 带着警觉收回
};

constexpr Keyframe kAffectionNestFrames[] = {
    {96, 88, 86, 108, 106, 620, MotionEase::kSmooth},    // 温柔对视
    {120, 62, 126, 126, 142, 820, MotionEase::kSmooth},  // 把脸颊送向用户
    {146, 76, 114, 116, 126, 900, MotionEase::kSmooth},  // 慢慢依偎下去
    {146, 76, 114, 116, 126, 620, MotionEase::kLinear},  // 安静停靠
    {116, 122, 66, 122, 48, 940, MotionEase::kSmooth},   // 换侧轻蹭
    {82, 94, 92, 104, 92, 820, MotionEase::kSmooth},     // 抬头回看
};

constexpr Keyframe kHappyArcFrames[] = {
    {48, 112, 126, 146, 54, 660, MotionEase::kSmooth},   // 抬头扬尾招呼
    {76, 54, 44, 132, 146, 900, MotionEase::kSmooth},    // 大弧线摆向另一侧
    {122, 132, 146, 138, 42, 980, MotionEase::kSmooth},  // 低头追过中位
    {56, 74, 62, 156, 136, 820, MotionEase::kFastOut},   // 再精神地昂起
    {86, 108, 112, 126, 66, 720, MotionEase::kSmooth},   // 俏皮回望
    {78, 92, 92, 112, 92, 820, MotionEase::kSmooth},     // 舒缓收势
};

constexpr Keyframe kProudSurveyFrames[] = {
    {46, 78, 70, 146, 126, 720, MotionEase::kSmooth},   // 侧身蓄势
    {24, 94, 98, 166, 88, 820, MotionEase::kFastOut},   // 昂首展示
    {24, 94, 98, 166, 88, 720, MotionEase::kLinear},    // 保持威风姿态
    {62, 132, 142, 148, 44, 920, MotionEase::kSmooth},  // 巡视一侧领地
    {72, 54, 40, 140, 146, 980, MotionEase::kSmooth},   // 平稳巡视另一侧
    {84, 92, 92, 116, 92, 860, MotionEase::kSmooth},    // 从容回正
};

constexpr Keyframe kEatForageFrames[] = {
    {108, 112, 116, 108, 70, 680, MotionEase::kSmooth},  // 嗅到食物
    {142, 124, 128, 102, 58, 820, MotionEase::kSmooth},  // 低头找寻
    {158, 82, 76, 110, 122, 720, MotionEase::kFastOut},  // 从另一侧取食
    {136, 104, 100, 106, 76, 620, MotionEase::kSmooth},  // 抬起咀嚼
    {154, 96, 92, 112, 116, 720, MotionEase::kFastOut},  // 再吃一口
    {126, 90, 88, 106, 88, 820, MotionEase::kSmooth},    // 满足地抬起
};

constexpr Keyframe kListenEchoFrames[] = {
    {62, 116, 132, 124, 62, 680, MotionEase::kSmooth},  // 向一侧竖起身体
    {62, 116, 132, 124, 62, 620, MotionEase::kLinear},  // 屏息聆听
    {72, 58, 46, 118, 138, 980, MotionEase::kSmooth},   // 同步转向回声
    {72, 58, 46, 118, 138, 560, MotionEase::kLinear},   // 再次确认
    {96, 102, 108, 112, 76, 780, MotionEase::kSmooth},  // 探向声源
    {80, 92, 92, 108, 92, 820, MotionEase::kSmooth},    // 安静回正
};

constexpr Keyframe kStartledRecoverFrames[] = {
    {36, 68, 54, 160, 146, 720, MotionEase::kFastOut},  // 全身同步后缩
    {36, 68, 54, 160, 146, 580, MotionEase::kLinear},   // 定住判断危险
    {58, 126, 138, 142, 50, 900, MotionEase::kSmooth},  // 谨慎检查一侧
    {88, 54, 42, 130, 140, 980, MotionEase::kSmooth},   // 换侧确认
    {110, 104, 98, 120, 72, 820, MotionEase::kSmooth},  // 勇敢向前复核
    {84, 92, 92, 110, 92, 900, MotionEase::kSmooth},    // 放松下来
};

constexpr Keyframe kSleepyDreamFrames[] = {
    {96, 86, 82, 84, 108, 820, MotionEase::kSmooth},     // 呼吸渐慢
    {126, 62, 128, 72, 136, 1080, MotionEase::kSmooth},  // 向一侧慢慢打盹
    {142, 78, 112, 62, 118, 980, MotionEase::kSmooth},   // 伏下长颈
    {142, 78, 112, 62, 118, 760, MotionEase::kLinear},   // 梦中停留
    {130, 118, 68, 68, 60, 1120, MotionEase::kSmooth},   // 睡梦中换个方向
    {118, 96, 92, 76, 92, 980, MotionEase::kSmooth},     // 安静蜷伏
};

// 按用户确认的五轴实机方向编排：颈/头反向补偿视线，尾左右与颈左右
// 采用同号角度偏移，形成物理反向配重。每帧五轴同步，不增加启动延迟。
constexpr Keyframe kDiscoverCanopyFrames[] = {
    {42, 114, 76, 132, 116, 780, MotionEase::kSmooth},  // 仰望树冠
    {12, 132, 64, 152, 136, 880, MotionEase::kSmooth},  // 高处凝视
    {12, 132, 64, 152, 136, 460, MotionEase::kLinear},
    {70, 66, 108, 124, 60, 1120, MotionEase::kSmooth},   // 沿斜弧追踪到另一侧
    {132, 76, 104, 104, 72, 1040, MotionEase::kSmooth},  // 低头确认地面
    {86, 100, 82, 116, 102, 920, MotionEase::kSmooth},   // 回看用户
};
constexpr Keyframe kHappyInviteFrames[] = {
    {38, 108, 78, 150, 114, 800, MotionEase::kSmooth},   // 昂头招呼
    {104, 126, 64, 128, 132, 980, MotionEase::kSmooth},  // 送出脸颊邀请互动
    {130, 104, 82, 114, 106, 720, MotionEase::kSmooth},  // 前探鞠躬
    {130, 104, 82, 114, 106, 460, MotionEase::kLinear},
    {68, 60, 114, 144, 54, 1100, MotionEase::kSmooth},  // 从另一侧舒展扬起
    {30, 84, 96, 160, 80, 760, MotionEase::kSmooth},
    {78, 90, 90, 112, 90, 940, MotionEase::kSmooth},
};
constexpr Keyframe kEatGrazeFrames[] = {
    {128, 108, 78, 112, 112, 900, MotionEase::kSmooth},  // 嗅探地面
    {180, 90, 90, 120, 90, 1100, MotionEase::kSmooth},   // 正面完全低头取食
    {180, 90, 90, 120, 90, 560, MotionEase::kLinear},
    {154, 112, 72, 116, 118, 820, MotionEase::kSmooth},  // 侧向慢嚼，不连续快速啄头
    {174, 72, 104, 124, 66, 940, MotionEase::kSmooth},   // 换一处取食
    {174, 72, 104, 124, 66, 460, MotionEase::kLinear},
    {96, 104, 80, 112, 108, 1240, MotionEase::kSmooth},  // 抬头确认周围
    {78, 90, 90, 104, 90, 760, MotionEase::kSmooth},
};
constexpr Keyframe kAffectionCheckInFrames[] = {
    {104, 112, 74, 114, 118, 820, MotionEase::kSmooth},   // 送出脸颊
    {146, 120, 66, 122, 126, 1000, MotionEase::kSmooth},  // 轻靠过来
    {146, 120, 66, 122, 126, 700, MotionEase::kLinear},   // 等待抚摸
    {94, 92, 90, 110, 92, 1000, MotionEase::kSmooth},     // 抬起看用户反应
    {94, 92, 90, 110, 92, 440, MotionEase::kLinear},
    {130, 68, 108, 120, 62, 920, MotionEase::kSmooth},  // 换侧再亲近一次
    {78, 90, 90, 104, 90, 1000, MotionEase::kSmooth},
};
constexpr Keyframe kListenConfirmFrames[] = {
    {56, 118, 70, 132, 122, 860, MotionEase::kSmooth},  // 侧身锁定声音
    {56, 118, 70, 132, 122, 620, MotionEase::kLinear},  // 留意声源
    {88, 100, 84, 114, 104, 800, MotionEase::kSmooth},  // 向前探听
    {68, 114, 74, 126, 118, 760, MotionEase::kSmooth},  // 同侧二次确认而非反复扫头
    {68, 114, 74, 126, 118, 480, MotionEase::kLinear},
    {98, 78, 100, 112, 74, 900, MotionEase::kSmooth},  // 放松回望
    {78, 90, 90, 106, 90, 820, MotionEase::kSmooth},
};
template <size_t N>
constexpr bool ValidatePhysicalStory(const Keyframe (&frames)[N]) {
    if (N > kMaxClipFrames) {
        return false;
    }
    for (const auto& frame : frames) {
        if (frame.neck_tilt < 0 || frame.neck_tilt > 180 || frame.neck_lean < 0 ||
            frame.neck_lean > 180 || frame.head_turn < 0 || frame.head_turn > 180 ||
            frame.tail_ud < SERVO_TAIL_UD_MIN || frame.tail_ud > SERVO_TAIL_UD_MAX ||
            frame.tail_lr < 0 || frame.tail_lr > 180 ||
            frame.duration_ms < kMinAuthoredStoryFrameMs) {
            return false;
        }
    }
    return true;
}
static_assert(ValidatePhysicalStory(kDiscoverCanopyFrames));
static_assert(ValidatePhysicalStory(kHappyInviteFrames));
static_assert(ValidatePhysicalStory(kEatGrazeFrames));
static_assert(ValidatePhysicalStory(kAffectionCheckInFrames));
static_assert(ValidatePhysicalStory(kListenConfirmFrames));
static_assert(kEatGrazeFrames[1].neck_tilt == 180 && kEatGrazeFrames[1].neck_lean == 90 &&
              kEatGrazeFrames[1].head_turn == 90);

// Spatially distinct full stories, selected by the existing action families.
constexpr Keyframe kRichDiscoverScent[] = {
    {90, 105, 125, 110, 115, 600, MotionEase::kSmooth},
    {120, 115, 145, 125, 140, 700, MotionEase::kSmooth},
    {90, 85, 65, 115, 80, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
static_assert(ValidatePhysicalStory(kRichDiscoverScent));
constexpr Keyframe kRichDiscoverListenBack[] = {
    {50, 75, 40, 105, 65, 600, MotionEase::kSmooth},
    {35, 70, 60, 120, 80, 600, MotionEase::kSmooth},
    {60, 105, 135, 110, 115, 750, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
static_assert(ValidatePhysicalStory(kRichDiscoverListenBack));
constexpr Keyframe kRichAffectionCheekNest[] = {
    {85, 110, 125, 115, 110, 650, MotionEase::kSmooth},
    {105, 120, 140, 110, 135, 700, MotionEase::kSmooth},
    {85, 95, 110, 100, 105, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
static_assert(ValidatePhysicalStory(kRichAffectionCheekNest));
constexpr Keyframe kRichAffectionReassure[] = {
    {55, 75, 50, 110, 70, 650, MotionEase::kSmooth},
    {85, 85, 75, 120, 85, 700, MotionEase::kSmooth},
    {65, 100, 115, 105, 110, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
static_assert(ValidatePhysicalStory(kRichAffectionReassure));
constexpr Keyframe kRichHappyPlayBow[] = {
    {100, 100, 120, 140, 125, 600, MotionEase::kSmooth},
    {125, 110, 140, 155, 145, 700, MotionEase::kSmooth},
    {50, 80, 65, 130, 55, 800, MotionEase::kSmooth},
    {35, 90, 90, 115, 90, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
static_assert(ValidatePhysicalStory(kRichHappyPlayBow));
constexpr Keyframe kRichHappyTailFanfare[] = {
    {40, 105, 130, 145, 140, 650, MotionEase::kSmooth},
    {30, 90, 100, 160, 110, 650, MotionEase::kSmooth},
    {50, 70, 45, 135, 40, 850, MotionEase::kSmooth},
    {55, 85, 75, 110, 75, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
static_assert(ValidatePhysicalStory(kRichHappyTailFanfare));

#include "dinosaur_reference_motion.h"
#include "dinosaur_story_motion.h"

constexpr MotionClip kDiscoverClips[] = {
    {"discover-direct", kDiscoverFrames, std::size(kDiscoverFrames)},
    {"discover-peek", kDiscoverPeekFrames, std::size(kDiscoverPeekFrames)},
    {"discover-track", kDiscoverTrackFrames, std::size(kDiscoverTrackFrames)},
    {"discover-canopy", kDiscoverCanopyFrames, std::size(kDiscoverCanopyFrames)},
    {"discover-scent-trail", kRichDiscoverScent, std::size(kRichDiscoverScent)},
    {"discover-listen-back", kRichDiscoverListenBack, std::size(kRichDiscoverListenBack)},
    {"ref-discover-direct", reference_motion::kDiscoverFrames,
     std::size(reference_motion::kDiscoverFrames), true},
    {"ref-discover-peek", reference_motion::kDiscoverPeekFrames,
     std::size(reference_motion::kDiscoverPeekFrames), true},
    {"ref-discover-sky-track", reference_motion::kDiscoverSkyFrames,
     std::size(reference_motion::kDiscoverSkyFrames), true},
    {"story-horizon-ladder", story_motion::kHorizonLadder, std::size(story_motion::kHorizonLadder),
     true},
    {"story-scentcompass", story_motion::kScentCompass, std::size(story_motion::kScentCompass),
     true},
};
constexpr MotionClip kAffectionClips[] = {
    {"affection-bow", kAffectionFrames, std::size(kAffectionFrames)},
    {"affection-nuzzle", kAffectionNuzzleFrames, std::size(kAffectionNuzzleFrames)},
    {"affection-nest", kAffectionNestFrames, std::size(kAffectionNestFrames)},
    {"affection-check-in", kAffectionCheckInFrames, std::size(kAffectionCheckInFrames)},
    {"affection-cheek-nest", kRichAffectionCheekNest, std::size(kRichAffectionCheekNest)},
    {"affection-reassure", kRichAffectionReassure, std::size(kRichAffectionReassure)},
    {"ref-affection-neck-bow", reference_motion::kAffectionFrames,
     std::size(reference_motion::kAffectionFrames), true},
    {"ref-affection-gentle-reach", reference_motion::kAffectionCuddleFrames,
     std::size(reference_motion::kAffectionCuddleFrames), true},
    {"ref-affection-playful-nuzzle", reference_motion::kAffectionNuzzleFrames,
     std::size(reference_motion::kAffectionNuzzleFrames), true},
    {"ref-affection-shy-approach", reference_motion::kAffectionShyFrames,
     std::size(reference_motion::kAffectionShyFrames), true},
    {"story-forehead-offer", story_motion::kForeheadOffer, std::size(story_motion::kForeheadOffer),
     true},
    {"story-gentleinvitation", story_motion::kGentleInvitation,
     std::size(story_motion::kGentleInvitation), true},
};
constexpr MotionClip kHappyClips[] = {
    {"happy-display", kHappyFrames, std::size(kHappyFrames)},
    {"happy-chase", kHappyChaseFrames, std::size(kHappyChaseFrames)},
    {"happy-arc", kHappyArcFrames, std::size(kHappyArcFrames)},
    {"happy-invite", kHappyInviteFrames, std::size(kHappyInviteFrames)},
    {"happy-play-bow", kRichHappyPlayBow, std::size(kRichHappyPlayBow)},
    {"happy-tail-fanfare", kRichHappyTailFanfare, std::size(kRichHappyTailFanfare)},
    {"ref-happy-juvenile-display", reference_motion::kHappyFrames,
     std::size(reference_motion::kHappyFrames), true},
    {"ref-happy-long-neck-sweep", reference_motion::kHappyDanceFrames,
     std::size(reference_motion::kHappyDanceFrames), true},
    {"ref-happy-orbit", reference_motion::kHappyOrbitFrames,
     std::size(reference_motion::kHappyOrbitFrames), true},
    {"ref-happy-tail-chase", reference_motion::kHappyChaseFrames,
     std::size(reference_motion::kHappyChaseFrames), true},
    {"story-spiral-invite", story_motion::kSpiralInvite, std::size(story_motion::kSpiralInvite),
     true},
    {"story-cometdance", story_motion::kCometDance, std::size(story_motion::kCometDance), true},
};
constexpr MotionClip kProudClips[] = {
    {"proud-call", kProudFrames, std::size(kProudFrames)},
    {"proud-survey", kProudSurveyFrames, std::size(kProudSurveyFrames)},
    {"ref-proud-front", reference_motion::kProudCallFrames,
     std::size(reference_motion::kProudCallFrames), true},
    {"ref-proud-sweep", reference_motion::kProudSweepFrames,
     std::size(reference_motion::kProudSweepFrames), true},
    {"ref-proud-orbit", reference_motion::kProudOrbitFrames,
     std::size(reference_motion::kProudOrbitFrames), true},
    {"ref-proud-echo-call", reference_motion::kProudEchoFrames,
     std::size(reference_motion::kProudEchoFrames), true},
    {"story-crest-salute", story_motion::kCrestSalute, std::size(story_motion::kCrestSalute), true},
    {"story-royallookout", story_motion::kRoyalLookout, std::size(story_motion::kRoyalLookout),
     true},
};
constexpr MotionClip kEatClips[] = {
    {"eat-peck", kEatFrames, std::size(kEatFrames)},
    {"eat-forage", kEatForageFrames, std::size(kEatForageFrames)},
    {"eat-graze", kEatGrazeFrames, std::size(kEatGrazeFrames)},
    {"ref-eat-peck", reference_motion::kEatFrames, std::size(reference_motion::kEatFrames), true},
    {"ref-eat-sniff", reference_motion::kEatSniffFrames,
     std::size(reference_motion::kEatSniffFrames), true},
    {"ref-eat-look-up", reference_motion::kEatLookUpFrames,
     std::size(reference_motion::kEatLookUpFrames), true},
    {"ref-eat-low-graze", reference_motion::kEatGrazeFrames,
     std::size(reference_motion::kEatGrazeFrames), true},
    {"story-graze-share", story_motion::kGrazeShare, std::size(story_motion::kGrazeShare), true},
    {"story-biteandoffer", story_motion::kBiteAndOffer, std::size(story_motion::kBiteAndOffer),
     true},
};
constexpr MotionClip kListenClips[] = {
    {"listen-locate", kListenFrames, std::size(kListenFrames)},
    {"listen-echo", kListenEchoFrames, std::size(kListenEchoFrames)},
    {"listen-confirm", kListenConfirmFrames, std::size(kListenConfirmFrames)},
    {"ref-listen-locate", reference_motion::kListenFrames,
     std::size(reference_motion::kListenFrames), true},
    {"ref-listen-track", reference_motion::kListenTrackFrames,
     std::size(reference_motion::kListenTrackFrames), true},
    {"ref-listen-double-check", reference_motion::kListenDoubleCheckFrames,
     std::size(reference_motion::kListenDoubleCheckFrames), true},
    {"ref-listen-overhead", reference_motion::kListenOverheadFrames,
     std::size(reference_motion::kListenOverheadFrames), true},
    {"story-near-far-answer", story_motion::kNearFarAnswer, std::size(story_motion::kNearFarAnswer),
     true},
    {"story-questionandanswer", story_motion::kQuestionAndAnswer,
     std::size(story_motion::kQuestionAndAnswer), true},
};
constexpr MotionClip kStartledClips[] = {
    {"startled-recoil", kStartledFrames, std::size(kStartledFrames)},
    {"startled-recover", kStartledRecoverFrames, std::size(kStartledRecoverFrames)},
    {"ref-startled-recoil", reference_motion::kStartledFrames,
     std::size(reference_motion::kStartledFrames), true},
    {"ref-startled-jump", reference_motion::kStartledJumpFrames,
     std::size(reference_motion::kStartledJumpFrames), true},
    {"ref-startled-low-duck", reference_motion::kStartledDuckFrames,
     std::size(reference_motion::kStartledDuckFrames), true},
    {"story-guard-and-trust", story_motion::kGuardAndTrust, std::size(story_motion::kGuardAndTrust),
     true},
    {"story-recoverconfidence", story_motion::kRecoverConfidence,
     std::size(story_motion::kRecoverConfidence), true},
};
constexpr MotionClip kSleepyClips[] = {
    {"sleepy-nod", kSleepyFrames, std::size(kSleepyFrames)},
    {"sleepy-dream", kSleepyDreamFrames, std::size(kSleepyDreamFrames)},
    {"ref-sleepy-neck-rest", reference_motion::kSleepyFrames,
     std::size(reference_motion::kSleepyFrames), true},
    {"ref-sleepy-long-neck-nod", reference_motion::kSleepyNodFrames,
     std::size(reference_motion::kSleepyNodFrames), true},
    {"ref-sleepy-neck-wrap", reference_motion::kSleepyWrapFrames,
     std::size(reference_motion::kSleepyWrapFrames), true},
    {"story-crescent-rest", story_motion::kCrescentRest, std::size(story_motion::kCrescentRest),
     true},
    {"story-dreamturn", story_motion::kDreamTurn, std::size(story_motion::kDreamTurn), true},
};

struct ClipSet {
    const MotionClip* clips;
    uint8_t count;
};

constexpr ClipSet kActionClips[] = {
    {kDiscoverClips, std::size(kDiscoverClips)}, {kAffectionClips, std::size(kAffectionClips)},
    {kHappyClips, std::size(kHappyClips)},       {kProudClips, std::size(kProudClips)},
    {kEatClips, std::size(kEatClips)},           {kListenClips, std::size(kListenClips)},
    {kStartledClips, std::size(kStartledClips)}, {kSleepyClips, std::size(kSleepyClips)},
};

constexpr bool ValidateActionPools() {
    for (const auto& set : kActionClips) {
        if (set.count == 0 || set.count > 16)
            return false;
        for (uint8_t i = 0; i < set.count; ++i) {
            const auto& clip = set.clips[i];
            if (clip.count == 0 || clip.count > kMaxClipFrames)
                return false;
            for (uint8_t frame = 0; frame < clip.count; ++frame) {
                const auto& key = clip.frames[frame];
                const int angles[] = {key.neck_tilt, key.neck_lean, key.head_turn, key.tail_ud,
                                      key.tail_lr};
                if (key.duration_ms == 0)
                    return false;
                for (unsigned axis = 0; axis < 5; ++axis) {
                    if (angles[axis] != wzh_dino::kKeep &&
                        (angles[axis] < (axis == 3 ? SERVO_TAIL_UD_MIN : 0) || angles[axis] > 180))
                        return false;
                }
            }
        }
    }
    return true;
}
static_assert(ValidateActionPools(), "Motion pools must fit the player and physical angle limits");

// ============================================================================
// Flash 短音频专用动作。它们不是替换原完整编舞，而是给 1~4 秒叫声、
// 咀嚼声和提示性声音准备的紧凑表演；每一帧变化轴仍同步起停。
// ============================================================================
constexpr Keyframe kSoundDiscoverFrames[] = {
    {60, 104, 120, 116, 68, 540, MotionEase::kFastOut},
};
constexpr Keyframe kSoundAffectionFrames[] = {
    {104, 108, 78, 110, 72, 560, MotionEase::kSmooth},
    {128, 102, 84, 120, 118, 680, MotionEase::kSmooth},
    {128, 102, 84, 120, 118, 380, MotionEase::kLinear},
};
constexpr Keyframe kSoundHappyFrames[] = {
    {52, 120, 130, 140, 56, 620, MotionEase::kSmooth},
    {68, 66, 52, 126, 134, 720, MotionEase::kSmooth},
};
constexpr Keyframe kSoundProudFrames[] = {
    {40, 90, 90, 150, 90, 620, MotionEase::kSmooth},
    {24, 104, 112, 165, 136, 720, MotionEase::kFastOut},
    {24, 104, 112, 165, 136, 680, MotionEase::kLinear},
};
constexpr Keyframe kSoundEatFrames[] = {
    {118, 100, 98, 106, 80, 560, MotionEase::kSmooth},
    {150, 92, 88, 112, 108, 620, MotionEase::kFastOut},
    {132, 106, 102, 108, 74, 600, MotionEase::kSmooth},
};
constexpr Keyframe kSoundListenFrames[] = {
    {66, 106, 128, 118, 70, 620, MotionEase::kSmooth},
    {66, 106, 128, 118, 70, 460, MotionEase::kLinear},
    {72, 74, 56, 112, 126, 760, MotionEase::kSmooth},
};
constexpr Keyframe kSoundStartledFrames[] = {
    {38, 76, 62, 156, 140, 680, MotionEase::kFastOut},
    {38, 76, 62, 156, 140, 440, MotionEase::kLinear},
    {66, 114, 124, 130, 56, 760, MotionEase::kSmooth},
};
constexpr Keyframe kSoundSleepyFrames[] = {
    {104, 100, 96, 82, 104, 720, MotionEase::kSmooth},
    {132, 112, 78, 68, 70, 900, MotionEase::kSmooth},
    {132, 112, 78, 68, 70, 620, MotionEase::kLinear},
};

// 每类短声再提供一个空间轨迹不同的变体，连续播放同类声音也不会只复读
// 一套姿态。帧数保持紧凑，声音包络负责在其中加入实时强弱变化。
constexpr Keyframe kSoundDiscoverSweepFrames[] = {
    {64, 122, 136, 124, 52, 680, MotionEase::kSmooth},
    {74, 62, 46, 118, 136, 820, MotionEase::kSmooth},
};
constexpr Keyframe kSoundAffectionLeanFrames[] = {
    {112, 68, 122, 116, 132, 720, MotionEase::kSmooth},
    {138, 82, 108, 108, 118, 820, MotionEase::kSmooth},
    {138, 82, 108, 108, 118, 420, MotionEase::kLinear},
};
constexpr Keyframe kSoundHappyGreetingFrames[] = {
    {50, 72, 58, 148, 138, 720, MotionEase::kSmooth},
    {70, 122, 132, 134, 50, 820, MotionEase::kSmooth},
    {58, 92, 96, 152, 104, 680, MotionEase::kFastOut},
};
constexpr Keyframe kSoundProudGuardFrames[] = {
    {34, 70, 62, 158, 140, 760, MotionEase::kSmooth},
    {26, 92, 94, 168, 92, 820, MotionEase::kFastOut},
    {26, 92, 94, 168, 92, 620, MotionEase::kLinear},
};

// 参考旧恐龙工程 proud-front/sweep/orbit/echo 的实机语义：IO17=0°
// 是仰到最高，IO8=180° 是尾巴最高。七套怒吼都先建立完整仰天剪影
//（颈 0°、尾 180°），再按声音长度展开正面、侧仰、高位扫视或二次回应。
constexpr Keyframe kRoarFrontFrames[] = {
    {0, 90, 90, 178, 90, 900, MotionEase::kSmooth},   // 第一段直接正面仰天
    {0, 90, 90, 178, 90, 360, MotionEase::kLinear},   // 留在最高位发声
    {22, 104, 82, 168, 112, 560, MotionEase::kSmooth},  // 余音中向一侧展开
};
constexpr Keyframe kRoarQuickAnswerFrames[] = {
    {0, 90, 90, 178, 90, 900, MotionEase::kSmooth},  // 短声也先完整仰天
    {0, 90, 90, 178, 90, 300, MotionEase::kLinear},
    {28, 96, 86, 170, 102, 520, MotionEase::kSmooth},  // 微侧收声，视线向前
};
constexpr Keyframe kRoarSideRiseFrames[] = {
    {0, 90, 90, 178, 90, 900, MotionEase::kSmooth},  // 先建立正面仰天
    {0, 90, 90, 178, 90, 420, MotionEase::kLinear},
    {12, 178, 6, 176, 172, 660, MotionEase::kSmooth},   // 大幅侧仰，尾巴反向配重
    {26, 6, 174, 164, 8, 720, MotionEase::kSmooth},     // 跨过中线到另一侧
    {34, 90, 90, 140, 90, 560, MotionEase::kSmooth},    // 高位收声
};
constexpr Keyframe kRoarHighSweepFrames[] = {
    {0, 90, 90, 178, 90, 900, MotionEase::kSmooth},
    {0, 90, 90, 178, 90, 520, MotionEase::kLinear},
    {10, 174, 8, 176, 174, 720, MotionEase::kSmooth},   // 高位向右扫视
    {24, 8, 172, 166, 8, 760, MotionEase::kSmooth},     // 从另一侧抬起，尾反向
    {32, 120, 60, 150, 120, 700, MotionEase::kSmooth},  // 弧线中部回望
    {34, 90, 90, 132, 90, 620, MotionEase::kSmooth},
};
constexpr Keyframe kRoarEchoFrames[] = {
    {0, 90, 90, 178, 90, 900, MotionEase::kSmooth},
    {0, 90, 90, 178, 90, 480, MotionEase::kLinear},
    {42, 178, 6, 166, 172, 760, MotionEase::kSmooth},   // 余音中大幅巡视
    {48, 6, 174, 172, 8, 760, MotionEase::kSmooth},     // 跨到另一侧
    {18, 34, 140, 172, 140, 480, MotionEase::kLinear},
    {0, 90, 90, 178, 90, 420, MotionEase::kFastOut},    // 第二次完整仰天回应
    {30, 120, 60, 150, 120, 720, MotionEase::kSmooth},
};
// 短吼增加向另一侧轻收声；长吼增加高位展开再回正的弧线故事。
// 都保留正面首帧，不镜像、不把短声音塞进多段长动作。
constexpr Keyframe kRoarSoftAnswerFrames[] = {
    {0, 90, 90, 178, 90, 900, MotionEase::kSmooth},
    {0, 90, 90, 178, 90, 320, MotionEase::kLinear},
    {24, 106, 80, 170, 114, 600, MotionEase::kSmooth},
};
constexpr Keyframe kRoarCrownArcFrames[] = {
    {0, 90, 90, 178, 90, 900, MotionEase::kSmooth},
    {0, 90, 90, 178, 90, 460, MotionEase::kLinear},
    {18, 176, 8, 174, 172, 760, MotionEase::kSmooth},   // 先向右大幅展开
    {28, 8, 172, 164, 8, 740, MotionEase::kSmooth},     // 越过中线到另一侧
    {12, 178, 6, 168, 178, 900, MotionEase::kSmooth},   // 高位跨中巡视
    {12, 178, 6, 168, 178, 460, MotionEase::kLinear},
    {42, 90, 90, 132, 90, 900, MotionEase::kSmooth},    // 舒展后正面收声
};
constexpr MotionClip kSkyRoarClips[] = {
    {"roar-front", kRoarFrontFrames, std::size(kRoarFrontFrames)},
    {"roar-quick-answer", kRoarQuickAnswerFrames, std::size(kRoarQuickAnswerFrames)},
    {"roar-side-rise", kRoarSideRiseFrames, std::size(kRoarSideRiseFrames)},
    {"roar-high-sweep", kRoarHighSweepFrames, std::size(kRoarHighSweepFrames)},
    {"roar-echo", kRoarEchoFrames, std::size(kRoarEchoFrames)},
    {"roar-soft-answer", kRoarSoftAnswerFrames, std::size(kRoarSoftAnswerFrames)},
    {"roar-crown-arc", kRoarCrownArcFrames, std::size(kRoarCrownArcFrames)},
};
// Two-beat recordings cannot fit a lateral story after the mandatory skyward
// onset. Give each borrowed path an authored compact tail height instead.
// Values stay near the top of the tail range so short roars still read as a
// skyward call, while remaining distinct for trajectory diversity.
constexpr float kCompactRoarTailAngles[] = {178, 170, 174, 166, 172, 176, 168};
static_assert(std::size(kCompactRoarTailAngles) == std::size(kSkyRoarClips));

constexpr bool ValidateRoarClips() {
    for (const auto& clip : kSkyRoarClips) {
        const auto& first = clip.frames[0];
        if (first.neck_tilt != 0 || first.neck_lean != 90 || first.head_turn != 90 ||
            first.tail_lr != 90 || first.duration_ms < 900 || clip.count > kMaxClipFrames) {
            return false;
        }
        for (uint8_t i = 0; i < clip.count; ++i) {
            const auto& frame = clip.frames[i];
            // Roars may use the full mechanical range, mirroring the reference
            // project's proud-call paths (neck 18..172, head 18..176, tail 180).
            if (frame.tail_ud < SERVO_TAIL_UD_MIN || frame.tail_ud > SERVO_TAIL_UD_MAX ||
                frame.neck_tilt < 0 || frame.neck_tilt > 70 || frame.neck_lean < 0 ||
                frame.neck_lean > 180 || frame.head_turn < 0 || frame.head_turn > 180 ||
                frame.tail_lr < 0 || frame.tail_lr > 180) {
                return false;
            }
        }
    }
    return true;
}
static_assert(ValidateRoarClips(), "Roars must start skyward and keep lateral travel bounded");
constexpr Keyframe kSoundEatNibbleFrames[] = {
    {126, 116, 118, 106, 64, 680, MotionEase::kSmooth},
    {154, 84, 80, 112, 122, 760, MotionEase::kFastOut},
    {134, 102, 100, 106, 78, 680, MotionEase::kSmooth},
};
constexpr Keyframe kSoundListenTrackFrames[] = {
    {60, 72, 54, 126, 134, 720, MotionEase::kSmooth},
    {60, 72, 54, 126, 134, 480, MotionEase::kLinear},
    {76, 120, 132, 116, 56, 860, MotionEase::kSmooth},
};
constexpr Keyframe kSoundStartledPeekFrames[] = {
    {40, 122, 136, 154, 48, 760, MotionEase::kFastOut},
    {40, 122, 136, 154, 48, 460, MotionEase::kLinear},
    {92, 70, 56, 126, 136, 900, MotionEase::kSmooth},
};
constexpr Keyframe kSoundSleepySighFrames[] = {
    {112, 72, 122, 78, 132, 880, MotionEase::kSmooth},
    {142, 88, 106, 62, 112, 1080, MotionEase::kSmooth},
    {142, 88, 106, 62, 112, 620, MotionEase::kLinear},
};

// 第三批短声动作参考旧工程中最有辨识度的空间故事重新编排。这里只把
// tail_ud 通过 LegacyTailUd() 反向换算，其余四轴保持原物理方向；每帧
// 都显式给出五轴目标，使全身同步起步、同步到位，不照搬旧工程的跟随延迟。
constexpr Keyframe kSoundDiscoverSkyFrames[] = {
    {38, 106, 126, LegacyTailUd(34), 64, 560, MotionEase::kSmooth},    // 抬颈捕捉高处
    {12, 138, 154, LegacyTailUd(12), 128, 720, MotionEase::kFastOut},  // 仰望追踪
    {52, 62, 44, LegacyTailUd(28), 54, 820, MotionEase::kSmooth},      // 高位跨中确认
};
constexpr Keyframe kSoundAffectionShyFrames[] = {
    {52, 122, 54, LegacyTailUd(32), 128, 620, MotionEase::kSmooth},  // 害羞偷看
    {146, 106, 84, LegacyTailUd(12), 54, 920, MotionEase::kSmooth},  // 全身一起贴近
    {146, 106, 84, LegacyTailUd(12), 54, 460, MotionEase::kLinear},  // 安静等候抚摸
};
constexpr Keyframe kSoundHappyOrbitFrames[] = {
    {44, 116, 122, LegacyTailUd(24), 58, 620, MotionEase::kSmooth},  // 精神地打开姿态
    {92, 146, 60, LegacyTailUd(42), 142, 780, MotionEase::kSmooth},  // 半圈空间弧线
    {54, 54, 132, LegacyTailUd(20), 46, 840, MotionEase::kSmooth},   // 从反侧昂起
};
constexpr Keyframe kSoundProudSentryFrames[] = {
    {38, 116, 124, LegacyTailUd(18), 58, 620, MotionEase::kSmooth},  // 侧向蓄势
    {18, 72, 64, LegacyTailUd(4), 130, 760, MotionEase::kFastOut},   // 高位威严展示
    {34, 96, 94, LegacyTailUd(20), 90, 680, MotionEase::kSmooth},    // 看回正前方
};
constexpr Keyframe kSoundEatLookUpFrames[] = {
    {154, 82, 76, LegacyTailUd(82), 60, 680, MotionEase::kFastOut},   // 低头取食
    {84, 104, 92, LegacyTailUd(58), 124, 760, MotionEase::kSmooth},   // 抬头看用户
    {142, 112, 108, LegacyTailUd(72), 62, 720, MotionEase::kSmooth},  // 换一侧继续寻找
};
constexpr Keyframe kSoundListenOverheadFrames[] = {
    {38, 68, 42, LegacyTailUd(36), 118, 620, MotionEase::kSmooth},   // 捕捉上方声音
    {6, 112, 138, LegacyTailUd(18), 54, 760, MotionEase::kFastOut},  // 仰头锁定声源
    {54, 92, 90, LegacyTailUd(42), 90, 720, MotionEase::kSmooth},    // 确认后回望用户
};
constexpr Keyframe kSoundStartledDuckFrames[] = {
    {150, 58, 38, LegacyTailUd(42), 40, 720, MotionEase::kFastOut},   // 同步低伏躲避
    {150, 58, 38, LegacyTailUd(42), 40, 440, MotionEase::kLinear},    // 屏息观察
    {62, 126, 138, LegacyTailUd(24), 142, 920, MotionEase::kSmooth},  // 从反侧谨慎抬起
};
constexpr Keyframe kSoundSleepyWrapFrames[] = {
    {118, 126, 54, LegacyTailUd(72), 134, 860, MotionEase::kSmooth},   // 长颈落向一侧
    {150, 142, 38, LegacyTailUd(92), 112, 1040, MotionEase::kSmooth},  // 低位慢慢蜷伏
    {136, 64, 130, LegacyTailUd(104), 62, 1040, MotionEase::kSmooth},  // 跨身寻找睡姿
};

// Short counterparts for real TTS/short Flash expressions.
constexpr Keyframe kRichSoundDiscoverScent[] = {
    {100, 110, 130, 115, 125, 600, MotionEase::kSmooth},
    {75, 95, 105, 105, 100, 600, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 700, MotionEase::kSmooth},
};
constexpr Keyframe kRichSoundDiscoverBack[] = {
    {40, 75, 45, 115, 65, 600, MotionEase::kSmooth},
    {55, 95, 110, 105, 105, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 700, MotionEase::kSmooth},
};
constexpr Keyframe kRichSoundAffectionCheek[] = {
    {95, 110, 130, 115, 120, 650, MotionEase::kSmooth},
    {80, 95, 110, 105, 100, 600, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 700, MotionEase::kSmooth},
};
constexpr Keyframe kRichSoundAffectionReassure[] = {
    {55, 75, 55, 115, 70, 650, MotionEase::kSmooth},
    {80, 95, 105, 110, 100, 600, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 700, MotionEase::kSmooth},
};
constexpr Keyframe kRichSoundHappyBow[] = {
    {115, 105, 125, 150, 130, 650, MotionEase::kSmooth},
    {35, 85, 65, 120, 65, 800, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 750, MotionEase::kSmooth},
};
constexpr Keyframe kRichSoundHappyFanfare[] = {
    {35, 105, 135, 155, 145, 650, MotionEase::kSmooth},
    {50, 75, 50, 125, 50, 900, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 750, MotionEase::kSmooth},
};

constexpr MotionClip kSoundDiscoverClips[] = {
    {"sound-discover", kSoundDiscoverFrames, std::size(kSoundDiscoverFrames)},
    {"sound-discover-sweep", kSoundDiscoverSweepFrames, std::size(kSoundDiscoverSweepFrames)},
    {"sound-discover-sky", kSoundDiscoverSkyFrames, std::size(kSoundDiscoverSkyFrames)},
    {"sound-discover-scent", kRichSoundDiscoverScent, std::size(kRichSoundDiscoverScent)},
    {"sound-discover-listen-back", kRichSoundDiscoverBack, std::size(kRichSoundDiscoverBack)},
};
constexpr MotionClip kSoundAffectionClips[] = {
    {"sound-affection", kSoundAffectionFrames, std::size(kSoundAffectionFrames)},
    {"sound-affection-lean", kSoundAffectionLeanFrames, std::size(kSoundAffectionLeanFrames)},
    {"sound-affection-shy", kSoundAffectionShyFrames, std::size(kSoundAffectionShyFrames)},
    {"sound-affection-cheek", kRichSoundAffectionCheek, std::size(kRichSoundAffectionCheek)},
    {"sound-affection-reassure", kRichSoundAffectionReassure,
     std::size(kRichSoundAffectionReassure)},
};
constexpr MotionClip kSoundHappyClips[] = {
    {"sound-happy", kSoundHappyFrames, std::size(kSoundHappyFrames)},
    {"sound-happy-greeting", kSoundHappyGreetingFrames, std::size(kSoundHappyGreetingFrames)},
    {"sound-happy-orbit", kSoundHappyOrbitFrames, std::size(kSoundHappyOrbitFrames)},
    {"sound-happy-bow", kRichSoundHappyBow, std::size(kRichSoundHappyBow)},
    {"sound-happy-fanfare", kRichSoundHappyFanfare, std::size(kRichSoundHappyFanfare)},
};
constexpr MotionClip kSoundProudClips[] = {
    {"sound-proud", kSoundProudFrames, std::size(kSoundProudFrames)},
    {"sound-proud-guard", kSoundProudGuardFrames, std::size(kSoundProudGuardFrames)},
    {"sound-proud-sentry", kSoundProudSentryFrames, std::size(kSoundProudSentryFrames)},
};
constexpr MotionClip kSoundEatClips[] = {
    {"sound-eat", kSoundEatFrames, std::size(kSoundEatFrames)},
    {"sound-eat-nibble", kSoundEatNibbleFrames, std::size(kSoundEatNibbleFrames)},
    {"sound-eat-look-up", kSoundEatLookUpFrames, std::size(kSoundEatLookUpFrames)},
};
constexpr MotionClip kSoundListenClips[] = {
    {"sound-listen", kSoundListenFrames, std::size(kSoundListenFrames)},
    {"sound-listen-track", kSoundListenTrackFrames, std::size(kSoundListenTrackFrames)},
    {"sound-listen-overhead", kSoundListenOverheadFrames, std::size(kSoundListenOverheadFrames)},
};
constexpr MotionClip kSoundStartledClips[] = {
    {"sound-startled", kSoundStartledFrames, std::size(kSoundStartledFrames)},
    {"sound-startled-peek", kSoundStartledPeekFrames, std::size(kSoundStartledPeekFrames)},
    {"sound-startled-duck", kSoundStartledDuckFrames, std::size(kSoundStartledDuckFrames)},
};
constexpr MotionClip kSoundSleepyClips[] = {
    {"sound-sleepy", kSoundSleepyFrames, std::size(kSoundSleepyFrames)},
    {"sound-sleepy-sigh", kSoundSleepySighFrames, std::size(kSoundSleepySighFrames)},
    {"sound-sleepy-wrap", kSoundSleepyWrapFrames, std::size(kSoundSleepyWrapFrames)},
};

constexpr ClipSet kSoundActionClips[] = {
    {kSoundDiscoverClips, std::size(kSoundDiscoverClips)},
    {kSoundAffectionClips, std::size(kSoundAffectionClips)},
    {kSoundHappyClips, std::size(kSoundHappyClips)},
    {kSoundProudClips, std::size(kSoundProudClips)},
    {kSoundEatClips, std::size(kSoundEatClips)},
    {kSoundListenClips, std::size(kSoundListenClips)},
    {kSoundStartledClips, std::size(kSoundStartledClips)},
    {kSoundSleepyClips, std::size(kSoundSleepyClips)},
};
static_assert(std::size(kSoundActionClips) == std::size(kActionClips));

// ============================================================================
// 身体部位级动作剪辑：从当前姿态起手，结束时回到默认姿态。方向性动作
// （看左/看右/抬头/低头）不参与随机镜像，保证朝向正确。
// ============================================================================
// One full head sweep with visible holds; directional commands never mirror.
constexpr Keyframe kBodyShakeHeadFrames[] = {
    {kKeep, kKeep, 160, kKeep, kKeep, 700, MotionEase::kSmooth},
    {kKeep, kKeep, 160, kKeep, kKeep, 420, MotionEase::kLinear},
    {kKeep, kKeep, 20, kKeep, kKeep, 1200, MotionEase::kSmooth},
    {kKeep, kKeep, 20, kKeep, kKeep, 420, MotionEase::kLinear},
    {kKeep, kKeep, 90, kKeep, kKeep, 800, MotionEase::kSmooth},
};
constexpr Keyframe kBodyLookLeftFrames[] = {
    {kKeep, kKeep, 160, kKeep, kKeep, 700, MotionEase::kSmooth},
    {kKeep, kKeep, 160, kKeep, kKeep, 700, MotionEase::kLinear},
    {kKeep, kKeep, 90, kKeep, kKeep, 800, MotionEase::kSmooth},
};
constexpr Keyframe kBodyLookRightFrames[] = {
    {kKeep, kKeep, 20, kKeep, kKeep, 700, MotionEase::kSmooth},
    {kKeep, kKeep, 20, kKeep, kKeep, 700, MotionEase::kLinear},
    {kKeep, kKeep, 90, kKeep, kKeep, 800, MotionEase::kSmooth},
};
constexpr Keyframe kBodyLookUpFrames[] = {
    {10, kKeep, kKeep, kKeep, kKeep, 300, MotionEase::kFastOut},
    {10, kKeep, kKeep, kKeep, kKeep, 500, MotionEase::kLinear},
    {70, kKeep, kKeep, kKeep, kKeep, 420, MotionEase::kSmooth},
};
constexpr Keyframe kBodyLookDownFrames[] = {
    {150, kKeep, kKeep, kKeep, kKeep, 300, MotionEase::kFastOut},
    {150, kKeep, kKeep, kKeep, kKeep, 500, MotionEase::kLinear},
    {70, kKeep, kKeep, kKeep, kKeep, 420, MotionEase::kSmooth},
};
constexpr Keyframe kBodyWagTailFrames[] = {
    {kKeep, kKeep, kKeep, 112, 32, 700, MotionEase::kSmooth},
    {kKeep, kKeep, kKeep, 148, 152, 900, MotionEase::kSmooth},
    {kKeep, kKeep, kKeep, 174, 100, 650, MotionEase::kSmooth},
    {kKeep, kKeep, kKeep, 174, 100, 350, MotionEase::kLinear},
    {kKeep, kKeep, kKeep, 132, 20, 950, MotionEase::kSmooth},
    {kKeep, kKeep, kKeep, 96, 144, 900, MotionEase::kSmooth},
    {kKeep, kKeep, kKeep, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kBodyTwistNeckFrames[] = {
    {kKeep, 35, kKeep, kKeep, kKeep, 250, MotionEase::kSmooth},
    {kKeep, 145, kKeep, kKeep, kKeep, 250, MotionEase::kSmooth},
    {kKeep, 35, kKeep, kKeep, kKeep, 250, MotionEase::kSmooth},
    {kKeep, 90, kKeep, kKeep, kKeep, 400, MotionEase::kSmooth},
};

// Explicit joint commands: finite, directional, with a quiet centered finish.
constexpr Keyframe kJointHeadCenterFrames[] = {
    {70, 90, 90, 90, 90, 1200, MotionEase::kSmooth},
};
constexpr Keyframe kJointHeadSwayLeftFrames[] = {
    {70, 90, 132, 90, 90, 650, MotionEase::kSmooth},
    {70, 90, 108, 90, 90, 520, MotionEase::kSmooth},
    {70, 90, 146, 90, 90, 650, MotionEase::kSmooth},
    {70, 90, 124, 90, 90, 500, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
};
constexpr Keyframe kJointHeadSwayRightFrames[] = {
    {70, 90, 48, 90, 90, 650, MotionEase::kSmooth},  {70, 90, 72, 90, 90, 520, MotionEase::kSmooth},
    {70, 90, 34, 90, 90, 650, MotionEase::kSmooth},  {70, 90, 56, 90, 90, 500, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
};
constexpr Keyframe kJointHeadScanFrames[] = {
    {70, 90, 144, 90, 90, 900, MotionEase::kSmooth},
    {70, 90, 144, 90, 90, 550, MotionEase::kSmooth},
    {70, 90, 36, 90, 90, 1500, MotionEase::kSmooth},
    {70, 90, 36, 90, 90, 550, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1200, MotionEase::kSmooth},
};
constexpr Keyframe kJointCenterFrames[] = {
    {70, 90, 90, 90, 90, 1200, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailUpFrames[] = {
    {70, 90, 90, 166, 90, 1300, MotionEase::kSmooth},
    {70, 90, 90, 166, 90, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1600, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailDownFrames[] = {
    {70, 90, 90, 62, 90, 1000, MotionEase::kSmooth},
    {70, 90, 90, 62, 90, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1300, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailLeftFrames[] = {
    {70, 90, 90, 90, 24, 1100, MotionEase::kSmooth},
    {70, 90, 90, 90, 24, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1500, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailRightFrames[] = {
    {70, 90, 90, 90, 156, 1100, MotionEase::kSmooth},
    {70, 90, 90, 90, 156, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1500, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailCenterFrames[] = {
    {70, 90, 90, 90, 90, 1200, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailLevelFrames[] = {
    {70, 90, 90, 90, 90, 1200, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 600, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailSwayLeftFrames[] = {
    {70, 90, 90, 90, 30, 1000, MotionEase::kSmooth},
    {70, 90, 90, 90, 72, 900, MotionEase::kSmooth},
    {70, 90, 90, 90, 22, 1000, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1400, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailSwayRightFrames[] = {
    {70, 90, 90, 90, 150, 1000, MotionEase::kSmooth},
    {70, 90, 90, 90, 108, 900, MotionEase::kSmooth},
    {70, 90, 90, 90, 158, 1000, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1400, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailCircleFrames[] = {
    {70, 90, 90, 140, 130, 900, MotionEase::kSmooth},
    {70, 90, 90, 118, 154, 950, MotionEase::kSmooth},
    {70, 90, 90, 95, 90, 950, MotionEase::kSmooth},
    {70, 90, 90, 118, 26, 950, MotionEase::kSmooth},
    {70, 90, 90, 159, 50, 950, MotionEase::kSmooth},
    {70, 90, 90, 171, 128, 950, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1300, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailEightFrames[] = {
    {70, 90, 90, 154, 140, 850, MotionEase::kSmooth},
    {70, 90, 90, 122, 90, 850, MotionEase::kSmooth},
    {70, 90, 90, 158, 35, 850, MotionEase::kSmooth},
    {70, 90, 90, 134, 90, 850, MotionEase::kSmooth},
    {70, 90, 90, 104, 146, 850, MotionEase::kSmooth},
    {70, 90, 90, 122, 90, 850, MotionEase::kSmooth},
    {70, 90, 90, 105, 34, 850, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1400, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailCurlFrames[] = {
    {70, 90, 90, 140, 90, 1100, MotionEase::kSmooth},
    {70, 90, 90, 172, 90, 900, MotionEase::kSmooth},
    {70, 90, 90, 172, 90, 600, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1800, MotionEase::kSmooth},
};
constexpr Keyframe kJointWagTailSlowFrames[] = {
    {70, 90, 90, 122, 26, 1500, MotionEase::kSmooth},
    {70, 90, 90, 138, 154, 2000, MotionEase::kSmooth},
    {70, 90, 90, 114, 48, 1600, MotionEase::kSmooth},
    {70, 90, 90, 142, 136, 1600, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1500, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailFanFrames[] = {
    {70, 90, 90, 164, 90, 1100, MotionEase::kSmooth},
    {70, 90, 90, 169, 30, 1000, MotionEase::kSmooth},
    {70, 90, 90, 157, 150, 1400, MotionEase::kSmooth},
    {70, 90, 90, 172, 40, 1200, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1500, MotionEase::kSmooth},
};
constexpr Keyframe kJointTailBounceFrames[] = {
    {70, 90, 90, 154, 90, 1000, MotionEase::kSmooth},
    {70, 90, 90, 74, 90, 1200, MotionEase::kSmooth},
    {70, 90, 90, 164, 90, 1400, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1600, MotionEase::kSmooth},
};
constexpr Keyframe kJointNeckLeftFrames[] = {
    {70, 150, 90, 90, 90, 1100, MotionEase::kSmooth},
    {70, 150, 90, 90, 90, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1600, MotionEase::kSmooth},
};
constexpr Keyframe kJointNeckRightFrames[] = {
    {70, 30, 90, 90, 90, 1100, MotionEase::kSmooth},
    {70, 30, 90, 90, 90, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1600, MotionEase::kSmooth},
};
constexpr Keyframe kJointNeckCenterFrames[] = {
    {70, 90, 90, 90, 90, 1200, MotionEase::kSmooth},
};
constexpr Keyframe kJointNeckCircleFrames[] = {
    {45, 132, 90, 90, 90, 1100, MotionEase::kSmooth},
    {100, 148, 90, 90, 90, 1100, MotionEase::kSmooth},
    {144, 90, 90, 90, 90, 1200, MotionEase::kSmooth},
    {96, 32, 90, 90, 90, 1200, MotionEase::kSmooth},
    {38, 60, 90, 90, 90, 1100, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1500, MotionEase::kSmooth},
};
constexpr Keyframe kJointNeckReachFrames[] = {
    {128, 90, 90, 90, 90, 1100, MotionEase::kSmooth},
    {128, 90, 90, 90, 90, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1700, MotionEase::kSmooth},
};
constexpr Keyframe kJointNeckStretchFrames[] = {
    {18, 90, 90, 90, 90, 1100, MotionEase::kSmooth},
    {18, 90, 90, 90, 90, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1600, MotionEase::kSmooth},
};
constexpr Keyframe kJointNodHeadFrames[] = {
    {40, 90, 90, 90, 90, 800, MotionEase::kSmooth},
    {102, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
    {44, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
    {94, 90, 90, 90, 90, 1200, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1500, MotionEase::kSmooth},
};

constexpr MotionClip kBodyClips[] = {
    {"body-shake-head", kBodyShakeHeadFrames, std::size(kBodyShakeHeadFrames)},
    {"body-look-left", kBodyLookLeftFrames, std::size(kBodyLookLeftFrames)},
    {"body-look-right", kBodyLookRightFrames, std::size(kBodyLookRightFrames)},
    {"body-look-up", kBodyLookUpFrames, std::size(kBodyLookUpFrames)},
    {"body-look-down", kBodyLookDownFrames, std::size(kBodyLookDownFrames)},
    {"body-wag-tail", kBodyWagTailFrames, std::size(kBodyWagTailFrames)},
    {"body-twist-neck", kBodyTwistNeckFrames, std::size(kBodyTwistNeckFrames)},
    {"body-head-center", kJointHeadCenterFrames, std::size(kJointHeadCenterFrames), true},
    {"body-head-sway-left", kJointHeadSwayLeftFrames, std::size(kJointHeadSwayLeftFrames), true},
    {"body-head-sway-right", kJointHeadSwayRightFrames, std::size(kJointHeadSwayRightFrames), true},
    {"body-head-scan", kJointHeadScanFrames, std::size(kJointHeadScanFrames), true},
    {"body-center", kJointCenterFrames, std::size(kJointCenterFrames), true},
    {"body-tail-up", kJointTailUpFrames, std::size(kJointTailUpFrames), true},
    {"body-tail-down", kJointTailDownFrames, std::size(kJointTailDownFrames), true},
    {"body-tail-left", kJointTailLeftFrames, std::size(kJointTailLeftFrames), true},
    {"body-tail-right", kJointTailRightFrames, std::size(kJointTailRightFrames), true},
    {"body-tail-center", kJointTailCenterFrames, std::size(kJointTailCenterFrames), true},
    {"body-tail-level", kJointTailLevelFrames, std::size(kJointTailLevelFrames), true},
    {"body-tail-sway-left", kJointTailSwayLeftFrames, std::size(kJointTailSwayLeftFrames), true},
    {"body-tail-sway-right", kJointTailSwayRightFrames, std::size(kJointTailSwayRightFrames), true},
    {"body-tail-circle", kJointTailCircleFrames, std::size(kJointTailCircleFrames), true},
    {"body-tail-eight", kJointTailEightFrames, std::size(kJointTailEightFrames), true},
    {"body-tail-curl", kJointTailCurlFrames, std::size(kJointTailCurlFrames), true},
    {"body-wag-tail-slow", kJointWagTailSlowFrames, std::size(kJointWagTailSlowFrames), true},
    {"body-tail-fan", kJointTailFanFrames, std::size(kJointTailFanFrames), true},
    {"body-tail-bounce", kJointTailBounceFrames, std::size(kJointTailBounceFrames), true},
    {"body-neck-left", kJointNeckLeftFrames, std::size(kJointNeckLeftFrames), true},
    {"body-neck-right", kJointNeckRightFrames, std::size(kJointNeckRightFrames), true},
    {"body-neck-center", kJointNeckCenterFrames, std::size(kJointNeckCenterFrames), true},
    {"body-neck-circle", kJointNeckCircleFrames, std::size(kJointNeckCircleFrames), true},
    {"body-neck-reach", kJointNeckReachFrames, std::size(kJointNeckReachFrames), true},
    {"body-neck-stretch", kJointNeckStretchFrames, std::size(kJointNeckStretchFrames), true},
    {"body-nod-head", kJointNodHeadFrames, std::size(kJointNodHeadFrames), true},
};

// Deliberate interaction has its own finite response, rather than restarting
// a full body command. All axes arrive together, hold, then return to 70/90/90/90/90.
constexpr std::array<Keyframe, 3> InteractionFrames(int tilt, int lean, int head, int tail_ud,
                                                    int tail_lr) {
    return {{{tilt, lean, head, tail_ud, tail_lr, 900, MotionEase::kSmooth},
             {tilt, lean, head, tail_ud, tail_lr, 420, MotionEase::kLinear},
             {70, 90, 90, 90, 90, 1000, MotionEase::kSmooth}}};
}
constexpr auto kAckTouch1Frames = InteractionFrames(40, 90, 155, 110, 90);
constexpr auto kAckTouch2Frames = InteractionFrames(60, 125, 145, 110, 125);
constexpr auto kAckTouch3Frames = InteractionFrames(60, 55, 35, 110, 55);
constexpr auto kAckTouch4Frames = InteractionFrames(30, 90, 115, 155, 150);
constexpr Keyframe kAckPlayfulFrames[] = {
    {35, 110, 145, 155, 155, 900, MotionEase::kSmooth},
    {35, 110, 145, 155, 155, 420, MotionEase::kLinear},
    {35, 70, 35, 155, 25, 1800, MotionEase::kSmooth},
    {35, 70, 35, 155, 25, 420, MotionEase::kLinear},
    {70, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
};
constexpr auto kAckHeldFrames = InteractionFrames(45, 95, 145, 105, 100);
constexpr auto kAckReleasedFrames = InteractionFrames(55, 90, 125, 105, 90);
constexpr auto kAckPickupFrames = InteractionFrames(30, 90, 145, 95, 90);
constexpr auto kAckShakeFrames = InteractionFrames(60, 90, 25, 95, 90);
constexpr auto kAckFlipFrames = InteractionFrames(45, 90, 145, 95, 90);
constexpr auto kAckSettledFrames = InteractionFrames(55, 90, 125, 100, 90);
constexpr auto kAckTouch1AltFrames = InteractionFrames(40, 90, 25, 110, 90);
constexpr auto kAckTouch2AltFrames = InteractionFrames(60, 110, 155, 145, 125);
constexpr auto kAckTouch3AltFrames = InteractionFrames(60, 70, 25, 145, 55);
constexpr auto kAckTouch4AltFrames = InteractionFrames(30, 90, 35, 155, 30);
// Additional interaction banks keep exact enum ordering.
constexpr Keyframe kRichAck_playful_alt[] = {
    {105, 105, 125, 145, 130, 650, MotionEase::kSmooth},
    {120, 100, 110, 155, 115, 600, MotionEase::kSmooth},
    {40, 75, 50, 125, 50, 850, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_held_alt[] = {
    {55, 80, 55, 105, 80, 650, MotionEase::kSmooth},
    {80, 90, 75, 115, 95, 600, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_released_alt[] = {
    {60, 80, 55, 110, 75, 600, MotionEase::kSmooth},
    {75, 95, 105, 100, 105, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_pickup_alt[] = {
    {35, 80, 55, 105, 75, 600, MotionEase::kSmooth},
    {50, 85, 75, 110, 85, 600, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_shake_alt[] = {
    {85, 95, 140, 85, 100, 550, MotionEase::kSmooth},
    {65, 90, 110, 100, 95, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_flip_alt[] = {
    {45, 80, 45, 100, 80, 600, MotionEase::kSmooth},
    {60, 85, 70, 110, 90, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_settled_alt[] = {
    {90, 80, 55, 105, 80, 600, MotionEase::kSmooth},
    {75, 90, 80, 100, 90, 600, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_touch1_story[] = {
    {45, 95, 125, 105, 100, 550, MotionEase::kSmooth},
    {35, 110, 145, 120, 125, 650, MotionEase::kSmooth},
    {60, 95, 110, 105, 100, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_touch2_story[] = {
    {70, 115, 125, 115, 120, 600, MotionEase::kSmooth},
    {95, 105, 110, 130, 105, 600, MotionEase::kSmooth},
    {50, 120, 145, 115, 135, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_touch3_story[] = {
    {70, 65, 55, 115, 60, 600, MotionEase::kSmooth},
    {95, 75, 70, 130, 75, 600, MotionEase::kSmooth},
    {50, 60, 35, 115, 45, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_touch4_story[] = {
    {95, 100, 125, 140, 130, 600, MotionEase::kSmooth},
    {115, 95, 105, 155, 105, 650, MotionEase::kSmooth},
    {45, 75, 55, 125, 55, 850, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_playful_story[] = {
    {40, 105, 130, 145, 140, 600, MotionEase::kSmooth},
    {30, 90, 100, 160, 110, 650, MotionEase::kSmooth},
    {55, 75, 50, 130, 45, 850, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_held_story[] = {
    {90, 100, 120, 110, 110, 650, MotionEase::kSmooth},
    {80, 95, 105, 120, 100, 600, MotionEase::kSmooth},
    {60, 85, 80, 105, 85, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_released_story[] = {
    {85, 105, 125, 105, 115, 600, MotionEase::kSmooth},
    {65, 95, 105, 115, 100, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_pickup_story[] = {
    {40, 100, 125, 110, 110, 600, MotionEase::kSmooth},
    {60, 105, 140, 120, 125, 650, MotionEase::kSmooth},
    {55, 90, 100, 105, 95, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_shake_story[] = {
    {85, 85, 60, 85, 80, 550, MotionEase::kSmooth},
    {55, 100, 125, 105, 110, 750, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_flip_story[] = {
    {45, 100, 130, 105, 115, 600, MotionEase::kSmooth},
    {80, 90, 105, 115, 100, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kRichAck_settled_story[] = {
    {90, 100, 120, 110, 110, 600, MotionEase::kSmooth},
    {65, 90, 100, 100, 95, 600, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};

constexpr MotionClip kInteractionClips[] = {
    {"ack-touch1", kAckTouch1Frames.data(), kAckTouch1Frames.size()},
    {"ack-touch2", kAckTouch2Frames.data(), kAckTouch2Frames.size()},
    {"ack-touch3", kAckTouch3Frames.data(), kAckTouch3Frames.size()},
    {"ack-touch4", kAckTouch4Frames.data(), kAckTouch4Frames.size()},
    {"ack-playful", kAckPlayfulFrames, std::size(kAckPlayfulFrames)},
    {"ack-held", kAckHeldFrames.data(), kAckHeldFrames.size()},
    {"ack-released", kAckReleasedFrames.data(), kAckReleasedFrames.size()},
    {"ack-pickup", kAckPickupFrames.data(), kAckPickupFrames.size()},
    {"ack-shake", kAckShakeFrames.data(), kAckShakeFrames.size()},
    {"ack-flip", kAckFlipFrames.data(), kAckFlipFrames.size()},
    {"ack-settled", kAckSettledFrames.data(), kAckSettledFrames.size()},
    {"ack-touch1-alt", kAckTouch1AltFrames.data(), kAckTouch1AltFrames.size()},
    {"ack-touch2-alt", kAckTouch2AltFrames.data(), kAckTouch2AltFrames.size()},
    {"ack-touch3-alt", kAckTouch3AltFrames.data(), kAckTouch3AltFrames.size()},
    {"ack-touch4-alt", kAckTouch4AltFrames.data(), kAckTouch4AltFrames.size()},
    {"ack-playful-alt", kRichAck_playful_alt, std::size(kRichAck_playful_alt)},
    {"ack-held-alt", kRichAck_held_alt, std::size(kRichAck_held_alt)},
    {"ack-released-alt", kRichAck_released_alt, std::size(kRichAck_released_alt)},
    {"ack-pickup-alt", kRichAck_pickup_alt, std::size(kRichAck_pickup_alt)},
    {"ack-shake-alt", kRichAck_shake_alt, std::size(kRichAck_shake_alt)},
    {"ack-flip-alt", kRichAck_flip_alt, std::size(kRichAck_flip_alt)},
    {"ack-settled-alt", kRichAck_settled_alt, std::size(kRichAck_settled_alt)},
    {"ack-touch1-story", kRichAck_touch1_story, std::size(kRichAck_touch1_story)},
    {"ack-touch2-story", kRichAck_touch2_story, std::size(kRichAck_touch2_story)},
    {"ack-touch3-story", kRichAck_touch3_story, std::size(kRichAck_touch3_story)},
    {"ack-touch4-story", kRichAck_touch4_story, std::size(kRichAck_touch4_story)},
    {"ack-playful-story", kRichAck_playful_story, std::size(kRichAck_playful_story)},
    {"ack-held-story", kRichAck_held_story, std::size(kRichAck_held_story)},
    {"ack-released-story", kRichAck_released_story, std::size(kRichAck_released_story)},
    {"ack-pickup-story", kRichAck_pickup_story, std::size(kRichAck_pickup_story)},
    {"ack-shake-story", kRichAck_shake_story, std::size(kRichAck_shake_story)},
    {"ack-flip-story", kRichAck_flip_story, std::size(kRichAck_flip_story)},
    {"ack-settled-story", kRichAck_settled_story, std::size(kRichAck_settled_story)},
    {"ack-touch1-expressive", story_motion::kForeheadOffer, std::size(story_motion::kForeheadOffer),
     true},
    {"ack-touch2-expressive", story_motion::kNearFarAnswer, std::size(story_motion::kNearFarAnswer),
     true},
    {"ack-touch3-expressive", story_motion::kGrazeShare, std::size(story_motion::kGrazeShare),
     true},
    {"ack-touch4-expressive", story_motion::kCrestSalute, std::size(story_motion::kCrestSalute),
     true},
    {"ack-playful-expressive", story_motion::kSpiralInvite, std::size(story_motion::kSpiralInvite),
     true},
    {"ack-held-expressive", story_motion::kCrescentRest, std::size(story_motion::kCrescentRest),
     true},
    {"ack-released-expressive", story_motion::kHorizonLadder,
     std::size(story_motion::kHorizonLadder), true},
    {"ack-pickup-expressive", story_motion::kForeheadOffer, std::size(story_motion::kForeheadOffer),
     true},
    {"ack-shake-expressive", story_motion::kGuardAndTrust, std::size(story_motion::kGuardAndTrust),
     true},
    {"ack-flip-expressive", story_motion::kNearFarAnswer, std::size(story_motion::kNearFarAnswer),
     true},
    {"ack-settled-expressive", story_motion::kCrescentRest, std::size(story_motion::kCrescentRest),
     true},
    {"ack-touch1-lively", story_motion::kGentleInvitation,
     std::size(story_motion::kGentleInvitation), true},
    {"ack-touch2-lively", story_motion::kQuestionAndAnswer,
     std::size(story_motion::kQuestionAndAnswer), true},
    {"ack-touch3-lively", story_motion::kBiteAndOffer, std::size(story_motion::kBiteAndOffer),
     true},
    {"ack-touch4-lively", story_motion::kRoyalLookout, std::size(story_motion::kRoyalLookout),
     true},
    {"ack-playful-lively", story_motion::kCometDance, std::size(story_motion::kCometDance), true},
    {"ack-held-lively", story_motion::kDreamTurn, std::size(story_motion::kDreamTurn), true},
    {"ack-released-lively", story_motion::kScentCompass, std::size(story_motion::kScentCompass),
     true},
    {"ack-pickup-lively", story_motion::kGentleInvitation,
     std::size(story_motion::kGentleInvitation), true},
    {"ack-shake-lively", story_motion::kRecoverConfidence,
     std::size(story_motion::kRecoverConfidence), true},
    {"ack-flip-lively", story_motion::kQuestionAndAnswer,
     std::size(story_motion::kQuestionAndAnswer), true},
    {"ack-settled-lively", story_motion::kDreamTurn, std::size(story_motion::kDreamTurn), true},
};
static_assert(std::size(kInteractionClips) == wzh::InteractionMotionSelector::kClipCount);

// 待机"个性姿态"：多轴协同的坐姿，轮换时身体缓慢漂移过去。
struct IdlePose {
    int neck_tilt;
    int neck_lean;
    int head_turn;
    int tail_ud;
    int tail_lr;
};
constexpr IdlePose kIdlePoses[] = {
    {70, 90, 90, 90, 90},   // 平静
    {67, 94, 99, 94, 97},   // 警觉
    {75, 87, 82, 85, 86},   // 困倦
    {64, 97, 105, 99, 81},  // 好奇
    {73, 88, 85, 92, 102},  // 放松
    {68, 93, 95, 95, 95},   // 轻快
    {58, 108, 122, 78, 66}, // 高位探看
    {84, 76, 62, 104, 118}, // 低尾沉思
    {61, 112, 56, 108, 34}, // 侧身警戒
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
// 好奇侧头：五轴同步形成 S 形重心，停一下确认目标后回中。
constexpr Keyframe kPulseCuriousFrames[] = {
    {62, 112, 118, 112, 70, 460, MotionEase::kSmooth},
    {58, 118, 126, 120, 62, 420, MotionEase::kFastOut},
    {58, 118, 126, 120, 62, 420, MotionEase::kLinear},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
// 友好招呼：抬颈与大尾摆同步发生，再从另一侧收回，像看见熟悉的人。
constexpr Keyframe kPulseGreetingFrames[] = {
    {54, 105, 112, 138, 58, 560, MotionEase::kSmooth},
    {62, 78, 72, 126, 130, 680, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 820, MotionEase::kSmooth},
};
// 二次确认：先观察近处，再换一个较高视角复核，动作宽但节奏舒缓。
constexpr Keyframe kPulseDoubleTakeFrames[] = {
    {80, 104, 120, 104, 68, 520, MotionEase::kSmooth},
    {80, 104, 120, 104, 68, 360, MotionEase::kLinear},
    {62, 80, 74, 122, 126, 680, MotionEase::kFastOut},
    {70, 90, 90, 90, 90, 840, MotionEase::kSmooth},
};
// 舒展：从中位同步打开颈和尾，再带一点侧向重心回落。
constexpr Keyframe kPulseStretchFrames[] = {
    {46, 90, 90, 146, 90, 620, MotionEase::kSmooth},
    {56, 114, 122, 136, 62, 720, MotionEase::kSmooth},
    {56, 114, 122, 136, 62, 380, MotionEase::kLinear},
    {70, 90, 90, 90, 90, 900, MotionEase::kSmooth},
};

constexpr MotionClip kPulseClips[] = {
    {"pulse-glance", kPulseGlanceFrames, std::size(kPulseGlanceFrames)},
    {"pulse-tail", kPulseTailFrames, std::size(kPulseTailFrames)},
    {"pulse-perk", kPulsePerkFrames, std::size(kPulsePerkFrames)},
    {"pulse-sniff", kPulseSniffFrames, std::size(kPulseSniffFrames)},
    {"pulse-curious", kPulseCuriousFrames, std::size(kPulseCuriousFrames)},
    {"pulse-greeting", kPulseGreetingFrames, std::size(kPulseGreetingFrames)},
    {"pulse-double-take", kPulseDoubleTakeFrames, std::size(kPulseDoubleTakeFrames)},
    {"pulse-stretch", kPulseStretchFrames, std::size(kPulseStretchFrames)},
};
}  // namespace

bool DinosaurController::Initialize(DinosaurAudio* audio) {
    audio_ = audio;
    InitializeServos();
    // 只保留一个待执行意图；表演期间的新请求替换旧请求，避免积压后连轴转。
    action_queue_ = xQueueCreate(1, sizeof(ActionRequest));
    if (action_queue_ == nullptr ||
        xTaskCreate(TaskEntry, "dino_motion", 5120, this, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start dinosaur motion task");
        servo_output_.Release();
        return false;
    }
    // The board singleton is still being constructed here, so defer local
    // audio until the application and AudioService are fully initialized.
    return true;
}

void DinosaurController::InitializeServos() {
    // 舵机电源与 PWM 分开控制：先给电源，再把各通道设置到安全默认角度。
    gpio_config_t power_config = {};
    power_config.pin_bit_mask = 1ULL << SERVO_POWER_GPIO;
    power_config.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&power_config));
    ESP_ERROR_CHECK(gpio_set_level(SERVO_POWER_GPIO, 0));

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
        channel_config.duty = 0;
        channel_config.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
    }
    servo_output_.Initialize(SERVO_POWER_GPIO, kServoChannels, std::size(kServoChannels));
    SetPose(current_pose_);
    ESP_LOGI(TAG,
             "Motion revision: stories-v4 / roar-onset-v3; tilt=IO17 lean=IO16 head=IO15 "
             "tailUD=IO8 tailLR=IO18");
}

void DinosaurController::SetPose(const Pose& requested) {
    // current_pose_ 始终保存不含声音叠加的基础姿态。声音偏移只能影响本次
    // PWM 输出，不能写回下一帧，否则同一方向会逐帧累积，最终看起来一直偏左。
    Pose base = requested;
    base.neck_tilt = std::clamp(base.neck_tilt, 0.0f, 180.0f);
    base.neck_lean = std::clamp(base.neck_lean, 0.0f, 180.0f);
    base.head_turn = std::clamp(base.head_turn, 0.0f, 180.0f);
    base.tail_ud = std::clamp(base.tail_ud, float(SERVO_TAIL_UD_MIN), float(SERVO_TAIL_UD_MAX));
    base.tail_lr = std::clamp(base.tail_lr, 0.0f, 180.0f);
    Pose pose = base;
    const bool commanded =
        request_active_.load() && running_priority_.load() == wzh::PetPriority::kExplicit;
    const bool talking = (speaking_.load() || speech_tail_.load()) && !servo_test_active_ &&
                         !commanded && audio_ != nullptr;
    const auto voice = audio_ != nullptr ? audio_->GetTtsMotionData() : DinoMotionData{};
    const auto gesture =
        voice_motion_.Update(wzh::VoiceMechanism::kDinosaur, talking && voice.playing, voice.level,
                             voice.attack, esp_timer_get_time() / 1000, speech_intent_.load());
    pose.neck_tilt += gesture[0] * SmoothStep((180.f - base.neck_tilt) / 30.f);
    pose.neck_lean += gesture[1];
    pose.head_turn += gesture[2];
    pose.tail_ud += gesture[3];
    pose.tail_lr += gesture[4];
    // 优先使用扬声器实际输出的 PCM 包络，因此在线 TTS、本地 Flash 声音和
    // 系统提示音都能同步动作；本地音频尚未开始输出时，用 Opus 包络预热。
    if (audio_ != nullptr && !servo_test_active_ && !commanded &&
        flash_performance_active_.load()) {
        float target_level = 0.0f;
        float target_emphasis = 0.0f;
        const int64_t now_us = esp_timer_get_time();
        const int64_t output_age_us = now_us - output_audio_updated_us_.load();
        if (output_age_us >= 0 && output_age_us < 180000) {
            // 对话声音略更有表现力；低音量也保留可见动作，不要求每句话都很响。
            const float gain = flash_performance_active_.load() ? 1.9f
                               : speaking_.load()               ? 2.6f
                                                                : 1.35f;
            target_level = std::clamp(output_audio_level_.load() * gain, 0.0f, 1.0f);
            target_emphasis = std::clamp(output_audio_attack_.load() * 0.75f, 0.0f, 1.0f);
        } else {
            const DinoMotionData sound = audio_->GetMotionData();
            if (sound.playing) {
                target_level = std::clamp(sound.level * 0.9f, 0.0f, 1.0f);
                target_emphasis = std::clamp(sound.attack * 0.5f, 0.0f, 1.0f);
            }
        }
        const float level_smoothing = target_level > audio_motion_level_ ? 0.10f : 0.04f;
        const float emphasis_smoothing = target_emphasis > audio_motion_emphasis_ ? 0.09f : 0.045f;
        audio_motion_level_ += (target_level - audio_motion_level_) * level_smoothing;
        audio_motion_emphasis_ += (target_emphasis - audio_motion_emphasis_) * emphasis_smoothing;

        const float dt =
            audio_motion_last_update_us_ > 0
                ? std::clamp((now_us - audio_motion_last_update_us_) / 1000000.0f, 0.0f, 0.10f)
                : 0.0f;
        audio_motion_last_update_us_ = now_us;
        if (audio_motion_level_ > 0.01f) {
            const float energy = std::sqrt(std::clamp(audio_motion_level_, 0.0f, 1.0f));
            if (!audio_motion_active_) {
                // 每段声音随机选择观察方向，同一段话内保持方向连续，避免乱抖。
                audio_motion_direction_ = (esp_random() & 1U) != 0 ? 1.0f : -1.0f;
                audio_motion_phase_ = frnd() * 1.2f;
                audio_motion_active_ = true;
            }
            // 五轴同步响应同一份声音包络，再用不同相位形成自然的协调姿态；
            // 怒吼时只让包络加强“仰”和“抬”，不叠加额外横向偏移，
            // 但保留当前怒吼编舞自己的小幅侧仰或高位扫视。
            const float flash_scale = flash_performance_active_.load() ? 1.35f : 1.0f;
            audio_motion_phase_ += dt * (1.35f + energy * 0.80f);
            const float lead = std::sin(audio_motion_phase_) * audio_motion_direction_;
            const float follow = std::sin(audio_motion_phase_ - 0.55f) * audio_motion_direction_;
            const float tail = std::sin(audio_motion_phase_ - 1.05f) * audio_motion_direction_;
            // 深低头时不能让声音把取食姿态拉起；150..180 度平滑减弱抬头
            // 叠加，180 度保持取食落点，尾巴和其他轴仍然响应真实声音。
            const float lift_room = SmoothStep((180.0f - base.neck_tilt) / 30.0f);
            pose.neck_tilt -=
                std::min(32.0f, (energy * 24.0f + audio_motion_emphasis_ * 8.0f) * flash_scale) *
                lift_room;
            pose.tail_ud += ((energy * 36.0f + follow * std::min(20.0f, energy * 26.0f) +
                              audio_motion_emphasis_ * 3.0f) *
                             flash_scale);
            if (!sky_roar_active_.load()) {
                pose.head_turn += (lead * std::min(64.0f, energy * 64.0f * flash_scale));
                pose.neck_lean += (follow * std::min(42.0f, energy * 42.0f * flash_scale));
                pose.tail_lr += (tail * std::min(62.0f, energy * 60.0f * flash_scale));
            }
        } else {
            audio_motion_active_ = false;
        }
    }
    // 所有入口最终都在这里限幅。尾巴上下轴机械空间最紧，使用板级专用限位。
    const auto pet_state = pet_.Get(esp_timer_get_time() / 1000);
    const float desired_gain =
        pet_state.gentle || pet_state.mode == wzh::PetMode::kQuiet ? .60f : 1.0f;
    const float gain = servo_test_active_ || quiet_center_.active() ? 1.0f
                       : request_active_.load() ? std::min(request_gain_, desired_gain)
                                                : desired_gain;
    pose.neck_tilt = (wzh::ExpressiveAngle(pose.neck_tilt, SERVO_NECK_TILT_DEFAULT, 0, 180, gain));
    pose.neck_lean = wzh::ExpressiveAngle(pose.neck_lean, 90, 0, 180, gain);
    pose.head_turn = wzh::ExpressiveAngle(pose.head_turn, 90, 0, 180, gain);
    pose.tail_ud =
        wzh::ExpressiveAngle(pose.tail_ud, 90, SERVO_TAIL_UD_MIN, SERVO_TAIL_UD_MAX, gain);
    pose.tail_lr = wzh::ExpressiveAngle(pose.tail_lr, 90, 0, 180, gain);
    pose.neck_tilt = std::clamp(pose.neck_tilt, 0.0f, 180.0f);
    pose.neck_lean = std::clamp(pose.neck_lean, 0.0f, 180.0f);
    pose.head_turn = std::clamp(pose.head_turn, 0.0f, 180.0f);
    pose.tail_ud = std::clamp(pose.tail_ud, float(SERVO_TAIL_UD_MIN), float(SERVO_TAIL_UD_MAX));
    pose.tail_lr = std::clamp(pose.tail_lr, 0.0f, 180.0f);
    const uint32_t now_ms = esp_timer_get_time() / 1000;
    const uint32_t elapsed =
        last_output_ms_ == 0 ? kTickMs : std::min<uint32_t>(now_ms - last_output_ms_, 50);
    float targets[] = {static_cast<float>(pose.neck_tilt), static_cast<float>(pose.neck_lean),
                       static_cast<float>(pose.head_turn), static_cast<float>(pose.tail_ud),
                       static_cast<float>(pose.tail_lr)};
    float travel = 0;
    for (size_t i = 0; i < std::size(targets); ++i) {
        travel = std::max(travel, std::abs(targets[i] - output_angles_[i]));
    }
    const float allowed = wzh::kPetServoSpeed * elapsed / 1000.0f;
    const float mix = travel > allowed && travel > 0 ? allowed / travel : 1.0f;
    for (size_t i = 0; i < std::size(targets); ++i) {
        targets[i] = output_angles_[i] + (targets[i] - output_angles_[i]) * mix;
    }
    if (servo_output_.WriteDegrees(targets, std::size(targets))) {
        for (size_t i = 0; i < std::size(targets); ++i) {
            output_angles_[i] = targets[i];
        }
        last_output_ms_ = now_ms;
        if (now_ms - last_pose_log_ms_ >= 500) {
            ESP_LOGD(
                TAG,
                "Servo command IO17/16/15/8/18=%.1f/%.1f/%.1f/%.1f/%.1f gentle=%d faults=0x%lx",
                targets[0], targets[1], targets[2], targets[3], targets[4], pet_state.gentle,
                static_cast<unsigned long>(servo_output_.faults()));
            last_pose_log_ms_ = now_ms;
        }
        current_pose_ = base;
        last_output_pose_ = {targets[0], targets[1], targets[2], targets[3], targets[4]};
    }
}

void DinosaurController::MoveTo(const Pose& target, uint32_t duration_ms) {
    // 本函数只由 dino_motion 任务调用，可以使用延时完成连续插值；主任务、
    // 网络回调和音频任务不会在这里等待。
    const Pose start = current_pose_;
    duration_ms = ComfortableDuration(start, target, duration_ms);
    const uint32_t steps = std::max<uint32_t>(1, duration_ms / kTickMs);
    for (uint32_t step = 1; step <= steps; ++step) {
        PollInteraction(esp_timer_get_time() / 1000);
        if (MotionInterrupted()) {
            return;
        }
        const float amount = SmoothStep(static_cast<float>(step) / steps);
        Pose pose = {
            .neck_tilt = start.neck_tilt + (target.neck_tilt - start.neck_tilt) * amount,
            .neck_lean = start.neck_lean + (target.neck_lean - start.neck_lean) * amount,
            .head_turn = start.head_turn + (target.head_turn - start.head_turn) * amount,
            .tail_ud = start.tail_ud + (target.tail_ud - start.tail_ud) * amount,
            .tail_lr = start.tail_lr + (target.tail_lr - start.tail_lr) * amount,
        };
        SetPose(pose);
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

DinosaurController::Pose DinosaurController::AttentionPose(DinoAction action, bool mirror) const {
    // 每个动作先短促地“看向用户/食物”建立互动，再进入编舞。
    Pose pose = {SERVO_NECK_TILT_DEFAULT, 90, SERVO_HEAD_TURN_DEFAULT, 90, SERVO_TAIL_LR_DEFAULT};
    switch (action) {
        case DinoAction::kDiscover:
        case DinoAction::kAffection:
        case DinoAction::kHappy:
            pose.neck_tilt = 100;  // 向正前方用户靠近一点
            pose.tail_ud = 118;
            break;
        case DinoAction::kEat:
            pose.neck_tilt = 112;  // 先看用户手中的食物
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
                                                             float amplitude,
                                                             bool authored_dynamics) const {
    Pose pose = prev;
    auto apply = [amplitude, mirror, authored_dynamics](float& dst, int value, bool lateral_axis,
                                                        int minimum = 0, int center = 90) {
        if (value != wzh_dino::kKeep) {
            // 0/180 是编舞明确指定的语义极值（仰天/贴地），不被随机缩幅拉回。
            const float angle =
                authored_dynamics
                    ? wzh::StoryAngle(value, center, minimum, 180, 1 + (amplitude - 1) * 1.3f)
                    : wzh::ActionAngle(value, center, minimum, 180, amplitude * 1.45f);
            int resolved =
                value == 0 || value == 180 ? value : static_cast<int>(std::lround(angle));
            if (mirror && lateral_axis) {
                resolved = 180 - resolved;
            }
            // 先限制实际目标再插值，避免随机放大后轨迹跑到机械范围之外，
            // 舵机已被输出限幅截住却还在空等；尾上下也使用其专属下限。
            dst = std::clamp(resolved, minimum, 180);
        }
    };
    apply(pose.neck_tilt, key.neck_tilt, false, 0, SERVO_NECK_TILT_DEFAULT);
    apply(pose.neck_lean, key.neck_lean, true);
    apply(pose.head_turn, key.head_turn, true);
    apply(pose.tail_ud, key.tail_ud, false, SERVO_TAIL_UD_MIN);
    apply(pose.tail_lr, key.tail_lr, true);
    return pose;
}

const wzh_dino::MotionClip* DinosaurController::PickClip(DinoAction action) {
    const uint8_t action_index = static_cast<uint8_t>(action);
    const ClipSet& set = kActionClips[action_index];
    const int index = action_clip_bags_[action_index].Draw(set.count, esp_random());
    return &set.clips[index];
}

const wzh_dino::MotionClip* DinosaurController::PickSoundClip(DinoAction action) {
    const uint8_t action_index = static_cast<uint8_t>(action);
    const ClipSet& set = kSoundActionClips[action_index];
    int index = irnd(set.count);
    if (set.count > 1 && index == last_sound_clip_indices_[action_index]) {
        index = (index + 1 + irnd(set.count - 1)) % set.count;
    }
    last_sound_clip_indices_[action_index] = index;
    return &set.clips[index];
}

const wzh_dino::MotionClip* DinosaurController::PickSkyRoarClip(uint32_t sound_duration_ms,
                                                                const wzh::SoundPlayback* sound) {
    // 音频越长，才开放更完整的高位扫视/二次回应；短吼声保持紧凑。
    // 实际录音各自不放回洗牌；旧动作入口保留全局不连重复。
    int candidates[4];
    int count = 0;
    if (sound_duration_ms <= 1700) {
        candidates[count++] = 0;  // roar-front
        candidates[count++] = 1;  // roar-quick-answer
        candidates[count++] = 5;  // roar-soft-answer
    } else if (sound_duration_ms <= 3200) {
        candidates[count++] = 0;
        candidates[count++] = 1;
        candidates[count++] = 2;  // roar-side-rise
        candidates[count++] = 5;
    } else if (sound_duration_ms <= 5200) {
        candidates[count++] = 2;
        candidates[count++] = 3;  // roar-high-sweep
        candidates[count++] = 0;
        candidates[count++] = 6;  // roar-crown-arc
    } else {
        candidates[count++] = 3;
        candidates[count++] = 4;  // roar-echo
        candidates[count++] = 2;
        candidates[count++] = 6;
    }
    int pick = sound ? roar_variants_.Draw(*sound, esp_random(), count) : irnd(count);
    if (!sound && count > 1 && candidates[pick] == last_sky_roar_clip_index_) {
        pick = (pick + 1 + irnd(count - 1)) % count;
    }
    last_sky_roar_clip_index_ = candidates[pick];
    return &kSkyRoarClips[last_sky_roar_clip_index_];
}

const wzh_dino::MotionClip* DinosaurController::PickPulse() {
    const int count = static_cast<int>(std::size(kPulseClips));
    int index = irnd(count);
    if (count > 1 && index == last_pulse_index_) {
        // 不连续复读同一种小表情，最小成本就能显著降低“程序循环感”。
        index = (index + 1 + irnd(count - 1)) % count;
    }
    last_pulse_index_ = index;
    return &kPulseClips[index];
}

bool DinosaurController::PickBalancedMirror() {
    bool mirror = mirror_balance_ > 0   ? false
                  : mirror_balance_ < 0 ? true
                                        : (esp_random() & 1U) != 0;
    mirror_balance_ += mirror ? 1 : -1;
    // 保留随机方向，但同一侧最多连续出现两次，避免用户长时间只看到
    // 脖子和尾巴朝左或朝右；动作结束回中，因此无需累积姿态补偿。
    if (mirror_run_length_ >= 2 && mirror == last_mirror_) {
        mirror = !last_mirror_;
    }
    if (mirror_run_length_ > 0 && mirror == last_mirror_) {
        ++mirror_run_length_;
    } else {
        last_mirror_ = mirror;
        mirror_run_length_ = 1;
    }
    return mirror;
}

void DinosaurController::ApplyLiving(Pose& pose, float living_phase, float strength) const {
    // 编舞过程中保留能看见的低频二级动作，让到达关键姿态后身体仍然有
    // 呼吸和重心传递；频率远低于逐音节抖动，不盖过主体动作。
    const float seconds = esp_timer_get_time() / 1000000.0f;
    pose.neck_tilt += std::sin(seconds * 0.58f + living_phase) * 4.2f * strength;
    pose.tail_ud += (std::sin(seconds * 0.43f + living_phase * 1.7f) * 6.8f * strength);
    if (!sky_roar_active_.load()) {
        pose.neck_lean +=
            (std::sin(seconds * 0.47f + living_phase * 1.3f + 0.8f) * 5.0f * strength);
        pose.head_turn += (std::sin(seconds * 0.68f + living_phase * 0.7f) * 6.0f * strength);
        pose.tail_lr += (std::sin(seconds * 0.39f + living_phase * 2.3f + 0.5f) * 9.0f * strength);
    }
}

void DinosaurController::PlaySegment(const Pose& from, const Pose& to, uint32_t duration_ms,
                                     wzh_dino::MotionEase ease, float living_phase,
                                     const Pose* previous, const Pose* next, uint32_t before_ms,
                                     uint32_t after_ms, bool duration_resolved) {
    if (!duration_resolved)
        duration_ms = ComfortableDuration(from, to, duration_ms);
    const bool moving = from.neck_tilt != to.neck_tilt || from.neck_lean != to.neck_lean ||
                        from.head_turn != to.head_turn || from.tail_ud != to.tail_ud ||
                        from.tail_lr != to.tail_lr;
    // 匀速标签只用于静止停留。发生位移时，kFastOut 保留温和的提前到位感，
    // 其他帧使用三次平滑；所有关节共用同一进度，同步开始、同步到达。
    const wzh_dino::MotionEase effective_ease =
        moving && ease == wzh_dino::MotionEase::kLinear ? wzh_dino::MotionEase::kSmooth : ease;
    // 连续同方向的 smooth 关键帧使用单调 Hermite 切线穿过中间姿态。切线遇到
    // 反向、定格或动作首尾会自动归零，所以既消除“每帧刹停”的机械感，也不会
    // 越过关键帧两端造成舵机角度过冲。
    const bool settling = std::abs(to.neck_tilt - 70) <= 2 && std::abs(to.neck_lean - 90) <= 2 &&
                          std::abs(to.head_turn - 90) <= 2 && std::abs(to.tail_ud - 90) <= 2 &&
                          std::abs(to.tail_lr - 90) <= 2;
    const bool flow_through = !settling && moving && ease == wzh_dino::MotionEase::kSmooth &&
                              previous != nullptr && next != nullptr;
    auto flow_axis = [before_ms, duration_ms, after_ms](float p0, float p1, float p2, float p3,
                                                        float t) {
        return wzh::MotionHermite(p0, p1, p2, p3, before_ms, duration_ms, after_ms, t);
    };
    const uint32_t steps = std::max<uint32_t>(1, duration_ms / kTickMs);
    TickType_t motion_tick = xTaskGetTickCount();
    for (uint32_t step = 1; step <= steps; ++step) {
        PollInteraction(esp_timer_get_time() / 1000);
        if (MotionInterrupted()) {
            return;
        }
        const float progress = static_cast<float>(step) / steps;
        const float amount =
            settling ? wzh::SettlingProgress(progress) : EaseValue(effective_ease, progress);
        Pose pose;
        if (flow_through) {
            pose = {
                .neck_tilt = flow_axis(previous->neck_tilt, from.neck_tilt, to.neck_tilt,
                                       next->neck_tilt, progress),
                .neck_lean = flow_axis(previous->neck_lean, from.neck_lean, to.neck_lean,
                                       next->neck_lean, progress),
                .head_turn = flow_axis(previous->head_turn, from.head_turn, to.head_turn,
                                       next->head_turn, progress),
                .tail_ud =
                    flow_axis(previous->tail_ud, from.tail_ud, to.tail_ud, next->tail_ud, progress),
                .tail_lr =
                    flow_axis(previous->tail_lr, from.tail_lr, to.tail_lr, next->tail_lr, progress),
            };
        } else {
            pose = {
                .neck_tilt = (from.neck_tilt + (to.neck_tilt - from.neck_tilt) * amount),
                .neck_lean = (from.neck_lean + (to.neck_lean - from.neck_lean) * amount),
                .head_turn = (from.head_turn + (to.head_turn - from.head_turn) * amount),
                .tail_ud = (from.tail_ud + (to.tail_ud - from.tail_ud) * amount),
                .tail_lr = (from.tail_lr + (to.tail_lr - from.tail_lr) * amount),
            };
        }
        // 二级生命感在段落中间展开、在关键帧落点归零。显式动作也保留一层
        // 较弱的低频微动，避免身体像按关键帧停顿的机械臂。
        if (!servo_test_active_ && !settling) {
            const float window = std::sin(progress * 3.14159265f);
            const float duration_gain = std::min(1.0f, duration_ms / 900.0f);
            const float explicit_gain =
                request_active_.load() && running_priority_.load() == wzh::PetPriority::kExplicit
                    ? 0.55f
                    : 1.0f;
            ApplyLiving(pose, living_phase, window * window * duration_gain * explicit_gain);
        }
        SetPose(pose);
        vTaskDelayUntil(&motion_tick, pdMS_TO_TICKS(kTickMs));
    }
}

DinosaurController::Pose DinosaurController::PlayClip(const wzh_dino::MotionClip& clip,
                                                      const Pose& start, bool mirror,
                                                      float amplitude, float duration_factor,
                                                      float living_phase) {
    // 一次性解析全部目标，让插值器看到前后关键帧并保持连续速度。
    // 使用固定小数组，长期自主运行不会每次动作都分配堆内存。
    if (clip.count > kMaxClipFrames) {
        ESP_LOGE(TAG, "Clip %s has too many frames: %u", clip.name,
                 static_cast<unsigned>(clip.count));
        return start;
    }
    std::array<Pose, kMaxClipFrames + 1> poses;
    std::array<uint32_t, kMaxClipFrames> durations;
    const uint32_t seed = esp_random();
    poses[0] = start;
    for (uint8_t i = 0; i < clip.count; ++i) {
        poses[i + 1] =
            ResolveKeyframe(clip.frames[i], poses[i], mirror, amplitude, clip.authored_dynamics);
        const float factor = sky_roar_active_.load() && i == 0 ? 1.0f : duration_factor;
        const uint32_t requested =
            static_cast<uint32_t>(std::lround(clip.frames[i].duration_ms * factor));
        const Pose& from = poses[i];
        const Pose& to = poses[i + 1];
        const float travel = std::max(
            {std::abs(to.neck_tilt - from.neck_tilt), std::abs(to.neck_lean - from.neck_lean),
             std::abs(to.head_turn - from.head_turn), std::abs(to.tail_ud - from.tail_ud),
             std::abs(to.tail_lr - from.tail_lr)});
        durations[i] = clip.authored_dynamics ? wzh::StoryFrameDuration(travel, requested, seed, i)
                                              : ComfortableDuration(from, to, requested);
        durations[i] = std::max(durations[i], ComfortableDuration(from, to, requested));
        if (clip.authored_dynamics && travel >= .01f) {
            // Keep broad travel and a clearly quicker active gesture budget.
            // Existing compact cues/body commands keep their original timing.
            durations[i] = std::max(
                {durations[i], uint32_t(300), static_cast<uint32_t>(std::ceil(travel * 7.5f))});
        }
    }
    for (uint8_t i = 0; i < clip.count; ++i) {
        if (MotionInterrupted())
            return current_pose_;
        const bool smooth_before =
            i > 0 && clip.frames[i - 1].ease == wzh_dino::MotionEase::kSmooth;
        const bool smooth_after =
            i + 1 < clip.count && clip.frames[i + 1].ease == wzh_dino::MotionEase::kSmooth;
        const Pose& previous = smooth_before ? poses[i - 1] : poses[i];
        const Pose& from = poses[i];
        const Pose& to = poses[i + 1];
        const Pose& next = smooth_after ? poses[i + 2] : poses[i + 1];
        const uint32_t before = i > 0 ? durations[i - 1] : durations[i];
        const uint32_t after = i + 1 < clip.count ? durations[i + 1] : durations[i];
        PlaySegment(from, to, durations[i], clip.frames[i].ease, living_phase, &previous, &next,
                    before, after, true);
        if (sky_roar_active_.load()) {
            ESP_LOGI(TAG, "Roar pose %s[%u]: IO17=%.1f IO16=%.1f IO15=%.1f IO8=%.1f IO18=%.1f",
                     clip.name, static_cast<unsigned>(i), last_output_pose_.neck_tilt,
                     last_output_pose_.neck_lean, last_output_pose_.head_turn,
                     last_output_pose_.tail_ud, last_output_pose_.tail_lr);
        }
    }
    return poses[clip.count];
}

uint32_t DinosaurController::ComfortableDuration(const Pose& from, const Pose& to,
                                                 uint32_t requested_ms) const {
    const float travel =
        std::max({std::abs(to.neck_tilt - from.neck_tilt), std::abs(to.neck_lean - from.neck_lean),
                  std::abs(to.head_turn - from.head_turn), std::abs(to.tail_ud - from.tail_ud),
                  std::abs(to.tail_lr - from.tail_lr)});
    if (travel < .01f)
        return std::clamp<uint32_t>(requested_ms, 100, 420);
    const bool settling = std::abs(to.neck_tilt - 70) <= 2 && std::abs(to.neck_lean - 90) <= 2 &&
                          std::abs(to.head_turn - 90) <= 2 && std::abs(to.tail_ud - 90) <= 2 &&
                          std::abs(to.tail_lr - 90) <= 2;
    if (settling)
        return std::max(requested_ms, wzh::SettlingDuration(travel, 100.f));
    return std::max({uint32_t(300), static_cast<uint32_t>(requested_ms * .76f),
                     static_cast<uint32_t>(std::ceil(travel * 7.5f))});
}

void DinosaurController::WaitForLocalSoundStart(int sound_index) const {
    if (audio_ == nullptr || sound_index < 0) {
        return;
    }
    // 这里只阻塞低优先级 dino_motion 任务，最长 300 ms；主循环与音频任务
    // 都不等待。看到 Flash 播放任务真正开始读这个文件后再起动作，避免
    // “身体先演、声音后到”。
    for (int attempt = 0; attempt < 30; ++attempt) {
        const DinoMotionData data = audio_->GetMotionData();
        if (data.playing && data.sound_index == sound_index) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

DinoAction DinosaurController::ClassifyLocalSound(int sound_index) const {
    if (audio_ == nullptr || sound_index < 0) {
        return DinoAction::kDiscover;
    }
    const char* name = audio_->SoundName(sound_index);
    const uint32_t duration_ms =
        static_cast<uint32_t>(std::max(0, audio_->SoundDurationMs(sound_index)));

    if (ContainsText(name, "吃") || ContainsText(name, "咀嚼") || ContainsText(name, "eat") ||
        ContainsText(name, "chew")) {
        return DinoAction::kEat;
    }
    if (ContainsText(name, "脚步") || ContainsText(name, "虫鸣") || ContainsText(name, "鸟叫") ||
        ContainsText(name, "下雨") || ContainsText(name, "流水") || ContainsText(name, "step") ||
        ContainsText(name, "foot") || ContainsText(name, "rain")) {
        return DinoAction::kListen;
    }
    if (ContainsText(name, "入睡") || ContainsText(name, "困") || ContainsText(name, "sleep")) {
        return DinoAction::kSleepy;
    }
    if (ContainsText(name, "警觉") || ContainsText(name, "受惊") || ContainsText(name, "startle") ||
        ContainsText(name, "alert")) {
        return DinoAction::kStartled;
    }
    if (ContainsText(name, "雀跃") || ContainsText(name, "开心") || ContainsText(name, "宝宝") ||
        ContainsText(name, "嘤嘤") || ContainsText(name, "happy") || ContainsText(name, "joy")) {
        return DinoAction::kHappy;
    }
    if (ContainsText(name, "亲近") || ContainsText(name, "撒娇") || ContainsText(name, "安慰") ||
        ContainsText(name, "comfort") || ContainsText(name, "nuzzle") ||
        ContainsText(name, "cat") || ContainsText(name, "Cat")) {
        return DinoAction::kAffection;
    }
    if (ContainsText(name, "叫声") || ContainsText(name, "咆哮") || ContainsText(name, "成年") ||
        ContainsText(name, "威风") || ContainsText(name, "call") || ContainsText(name, "roar") ||
        ContainsText(name, "adult")) {
        return DinoAction::kProud;
    }
    if (ContainsText(name, "发现") || ContainsText(name, "探索") || ContainsText(name, "好奇") ||
        duration_ms <= 1700) {
        return DinoAction::kDiscover;
    }
    if (duration_ms >= 6500) {
        return DinoAction::kProud;
    }

    // 旧音频包可能没有可识别文件名。使用偏友好、好奇的分布，而不是让
    // 每段未知声音都配同一个动作，增加 Flash 音频表演的变化。
    constexpr DinoAction kFallbackActions[] = {
        DinoAction::kHappy,  DinoAction::kAffection, DinoAction::kDiscover,
        DinoAction::kListen, DinoAction::kHappy,     DinoAction::kProud,
    };
    return kFallbackActions[esp_random() % std::size(kFallbackActions)];
}

bool DinosaurController::IsSkyRoarSound(int sound_index) const {
    if (audio_ == nullptr || sound_index < 0) {
        return false;
    }
    const char* name = audio_->SoundName(sound_index);
    // 实机 Flash 中“叫声音效合集 (N)”的所有条目都是怒吼，无论 N 和
    // 时长是多少都必须跳过 proud 随机池，避免再选到左右巡视编舞。
    return ContainsText(name, "叫声音效合集") || ContainsText(name, "怒吼") ||
           ContainsText(name, "仰天长啸") || ContainsText(name, "长啸") ||
           ContainsText(name, "咆哮") || ContainsText(name, "roar");
}

void DinosaurController::RunSoundAction(DinoAction action, const DinoSoundSelection& sound) {
    WaitForLocalSoundStart(sound.index);
    const bool sky_roar = IsSkyRoarSound(sound.index);
    const MotionClip* roar_clip = sky_roar ? PickSkyRoarClip(sound.duration_ms) : nullptr;
    ESP_LOGI(TAG, "Flash sound: %s (%" PRIu32 " ms) -> %s%s%s", audio_->SoundName(sound.index),
             sound.duration_ms, ActionName(action), sky_roar ? " / " : "",
             roar_clip != nullptr ? roar_clip->name : "");
    flash_performance_active_.store(true);
    sky_roar_active_.store(sky_roar);
    // 怒吼库已经包含有限且平衡的侧向细节，不再随机镜像，避免重新变成
    // 单侧扭头；非怒吼仍保留原动作库的左右变化。
    RunAction(action, sound.duration_ms, true, roar_clip, !sky_roar);
    sky_roar_active_.store(false);
    flash_performance_active_.store(false);
}

void DinosaurController::RunAction(DinoAction action, uint32_t sound_duration_ms, bool flash_sync,
                                   const wzh_dino::MotionClip* forced_clip, bool allow_mirror) {
    // 主动作采用更大的目标和紧凑节奏；连续插值与最终输出限速负责平滑起停。
    const bool mirror = allow_mirror && PickBalancedMirror();
    const float amplitude =
        forced_clip != nullptr ? (sky_roar_active_.load() ? 1.20f : 1.45f) : 1.45f + frnd() * 0.15f;
    const bool short_flash_cue = flash_sync && sound_duration_ms > 0 && sound_duration_ms <= 4200;
    // 短音频使用专属紧凑编舞。长音频也保持清楚的动作节奏，不按声音长度
    // 成倍拉慢；怒吼只允许少量延长，动作完成后释放舵机。
    const float tempo = forced_clip != nullptr ? 1.0f
                        : short_flash_cue      ? 1.0f + frnd() * 0.08f
                                               : 1.0f + frnd() * 0.12f;
    const float living_phase = frnd() * 6.2832f;
    const wzh_dino::MotionClip* clip =
        forced_clip != nullptr ? forced_clip
                               : (short_flash_cue ? PickSoundClip(action) : PickClip(action));

    const uint32_t attention_ms = short_flash_cue || forced_clip != nullptr ? 0 : 500;
    const uint32_t settle_ms = short_flash_cue || forced_clip != nullptr ? 760 : 820;
    float duration_scale = 1.0f;
    if (sky_roar_active_.load()) {
        // 按限速后的真实行程估计时长。声音预算用于仰头和主体编舞，收势
        // 放在尾音之后；第一段不随长声拉伸，始终尽早形成仰头剪影。
        Pose previous = current_pose_;
        uint32_t onset_ms = 0;
        uint32_t body_ms = 0;
        for (uint8_t i = 0; i < clip->count; ++i) {
            const Pose target = ResolveKeyframe(clip->frames[i], previous, false, 1.0f);
            const uint32_t duration =
                ComfortableDuration(previous, target, clip->frames[i].duration_ms);
            if (i == 0) {
                onset_ms = duration;
            } else {
                body_ms += duration;
            }
            previous = target;
        }
        const uint32_t remaining_ms =
            sound_duration_ms > onset_ms ? sound_duration_ms - onset_ms : 0;
        duration_scale = body_ms > 0
                             ? std::clamp(static_cast<float>(remaining_ms) / body_ms, 1.0f, 1.25f)
                             : 1.0f;
        ESP_LOGI(TAG, "Roar onset: %" PRIu32 "ms to IO17=0, IO16=90, IO15=90; audio=%" PRIu32 "ms",
                 onset_ms, sound_duration_ms);
    }
    ESP_LOGI(TAG, "Action: %s%s amp=%.2f tempo=%.2f%s duration=%" PRIu32 "ms scale=%.2f",
             clip->name, mirror ? " (mirror)" : "", static_cast<double>(amplitude),
             static_cast<double>(tempo), flash_sync ? " flash-sync" : "", sound_duration_ms,
             static_cast<double>(duration_scale));

    Pose pose = current_pose_;
    if (!short_flash_cue && forced_clip == nullptr) {
        const Pose attention = AttentionPose(action, mirror);
        PlaySegment(pose, attention,
                    static_cast<uint32_t>(std::lround(attention_ms * duration_scale)),
                    wzh_dino::MotionEase::kSmooth, living_phase);
        pose = attention;
    }

    pose = PlayClip(*clip, pose, mirror, amplitude, duration_scale / tempo, living_phase);

    // 收势：自然回到板级默认姿态，不再硬性定格。
    const Pose rest = {SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT, SERVO_HEAD_TURN_DEFAULT,
                       SERVO_TAIL_UD_DEFAULT, SERVO_TAIL_LR_DEFAULT};
    const float settle_scale = sky_roar_active_.load() ? 1.0f : duration_scale;
    PlaySegment(last_output_pose_, rest,
                static_cast<uint32_t>(std::lround(settle_ms * settle_scale)),
                wzh_dino::MotionEase::kSmooth, living_phase);
    PrepareIdleCenter();
    quiet_center_.Reset();
}

void DinosaurController::RunPulse(const wzh_dino::MotionClip& clip) {
    const bool mirror = PickBalancedMirror();
    // 生命脉冲需要看得见才能显得有生命力；幅度恢复到中等偏大，速度仍由
    // 统一时长和行程限速控制，因此不会变成快速甩动。
    const float amplitude = 1.05f + frnd() * 0.15f;
    const float tempo = 0.70f + frnd() * 0.12f;
    const float living_phase = frnd() * 6.2832f;
    ESP_LOGI(TAG, "Pulse: %s%s", clip.name, mirror ? " (mirror)" : "");
    PlayClip(clip, current_pose_, mirror, amplitude, 1.0f / tempo, living_phase);
    PrepareIdleCenter();
    quiet_center_.Reset();
}

void DinosaurController::RunIdlePose() {
    const uint32_t now_ms = esp_timer_get_time() / 1000;
    if (idle_breath_epoch_ms_ == 0) {
        idle_breath_epoch_ms_ = now_ms;
    }
    if (next_idle_pose_ms_ == 0) {
        next_idle_pose_ms_ = now_ms + static_cast<uint32_t>(irand(5000, 10000));
    } else if (static_cast<int32_t>(now_ms - next_idle_pose_ms_) >= 0) {
        if (idle_pose_index_ == 0) {
            // 中位停留后偶尔做一次轻微观察姿态。
            idle_pose_index_ = 1 + irnd(static_cast<int>(std::size(kIdlePoses)) - 1);
            next_idle_pose_ms_ = now_ms + static_cast<uint32_t>(irand(2500, 4500));
        } else {
            // 偏姿只短暂停留，随后回中更久；默认状态大部分时间在中位附近。
            idle_pose_index_ = 0;
            next_idle_pose_ms_ = now_ms + static_cast<uint32_t>(irand(6000, 11000));
        }
    }
    const IdlePose& target = kIdlePoses[idle_pose_index_];
    const float k = 0.028f;
    idle_drift_[0] += (static_cast<float>(target.neck_tilt) - idle_drift_[0]) * k;
    idle_drift_[1] += (static_cast<float>(target.neck_lean) - idle_drift_[1]) * k;
    idle_drift_[2] += (static_cast<float>(target.head_turn) - idle_drift_[2]) * k;
    idle_drift_[3] += (static_cast<float>(target.tail_ud) - idle_drift_[3]) * k;
    idle_drift_[4] += (static_cast<float>(target.tail_lr) - idle_drift_[4]) * k;
    // 呼吸从中位出发并回到中位，每轮最后留出自然停顿。侧向重心只在
    // 呼吸过程中轻微传递，停顿时四个中位轴回到 90 度附近。
    const float seconds = (now_ms - idle_breath_epoch_ms_) / 1000.0f;
    const float breath = BreathingWave(seconds);
    const float cycle_phase = std::fmod(seconds, 12.0f) / 12.0f;
    const float active_phase = std::clamp(cycle_phase / 0.64f, 0.0f, 1.0f);
    // 每次呼吸在左右两侧分配相同时间，避免开机后的前半轮总向同一边。
    const float sway = std::sin(active_phase * 6.2831853f) * breath;
    Pose pose = {
        idle_drift_[0] - 7.0f * breath,  idle_drift_[1] + 2.5f * sway, idle_drift_[2] + 3.5f * sway,
        idle_drift_[3] + 10.0f * breath, idle_drift_[4] - 5.0f * sway,
    };
    SetPose(pose);
}

void DinosaurController::PrepareIdleCenter() {
    // 从刚结束的真实姿态继续缓慢回中，避免动作结束后第一帧跳到旧的待机
    // 漂移值。回中后至少停留 9~15 秒才会选择下一次偏姿。
    idle_pose_index_ = 0;
    idle_drift_[0] = static_cast<float>(current_pose_.neck_tilt);
    idle_drift_[1] = static_cast<float>(current_pose_.neck_lean);
    idle_drift_[2] = static_cast<float>(current_pose_.head_turn);
    idle_drift_[3] = static_cast<float>(current_pose_.tail_ud);
    idle_drift_[4] = static_cast<float>(current_pose_.tail_lr);
    idle_breath_epoch_ms_ = esp_timer_get_time() / 1000;
    next_idle_pose_ms_ = esp_timer_get_time() / 1000 + static_cast<uint32_t>(irand(9000, 15000));
}

void DinosaurController::TaskEntry(void* arg) { static_cast<DinosaurController*>(arg)->Task(); }

void DinosaurController::ParkForShutdown() {
    request_active_.store(false);
    speaking_.store(false);
    speech_tail_.store(false);
    flash_performance_active_.store(false);
    sky_roar_active_.store(false);
    const auto origin = last_output_pose_;
    const Pose rest = {SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT, SERVO_HEAD_TURN_DEFAULT,
                       SERVO_TAIL_UD_DEFAULT, SERVO_TAIL_LR_DEFAULT};
    const float travel = std::max(
        {std::abs(origin.neck_tilt - rest.neck_tilt), std::abs(origin.neck_lean - rest.neck_lean),
         std::abs(origin.head_turn - rest.head_turn), std::abs(origin.tail_ud - rest.tail_ud),
         std::abs(origin.tail_lr - rest.tail_lr)});
    const uint32_t duration = std::min<uint32_t>(2200, wzh::SettlingDuration(travel, 90.f));
    const uint32_t started = esp_timer_get_time() / 1000;
    quiet_center_.Reset();
    quiet_center_.Sample(
        {origin.neck_tilt, origin.neck_lean, origin.head_turn, origin.tail_ud, origin.tail_lr},
        {rest.neck_tilt, rest.neck_lean, rest.head_turn, rest.tail_ud, rest.tail_lr}, started);
    while (servo_output_.powered() && !servo_output_.blocked()) {
        const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() / 1000 - started);
        const float mix = wzh::SettlingProgress(float(elapsed) / duration);
        SetPose(Pose{origin.neck_tilt + (rest.neck_tilt - origin.neck_tilt) * mix,
                     origin.neck_lean + (rest.neck_lean - origin.neck_lean) * mix,
                     origin.head_turn + (rest.head_turn - origin.head_turn) * mix,
                     origin.tail_ud + (rest.tail_ud - origin.tail_ud) * mix,
                     origin.tail_lr + (rest.tail_lr - origin.tail_lr) * mix});
        if (elapsed >= duration)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    servo_output_.SetFault(wzh::kShutdown, true);
}

cJSON* DinosaurController::RequestResult(bool accepted) const {
    return wzh::PetStatus(pet_mode(), servo_output_.faults(), servo_output_.powered(),
                          request_active_.load(), conversation_active_.load(), accepted);
}

void DinosaurController::CancelSoundMotion() {
    sound_motion_.Cancel(audio_ ? audio_->GetMotionData().sequence : 0);
}

bool DinosaurController::UpdateSoundMotion(uint32_t now) {
    if (audio_ == nullptr || conversation_active_.load()) {
        CancelSoundMotion();
        return false;
    }
    const auto sound = audio_->GetMotionData();
    const uint32_t random = sound_motion_.NeedsBegin(sound) ? esp_random() : 0;
    if (sound_motion_.NeedsBegin(sound)) {
        // These are the actual reference-example stories, not another set of
        // generic five-axis poses. Also reach paired playback, whose main path
        // remains the user's recording with a bounded reference contribution.
        DinoAction action = DinoAction::kDiscover;
        switch (sound.kind) {
            case wzh::SoundKind::kAffection:
                action = DinoAction::kAffection;
                break;
            case wzh::SoundKind::kLaugh:
            case wzh::SoundKind::kStretch:
                action = DinoAction::kHappy;
                break;
            case wzh::SoundKind::kChew:
                action = DinoAction::kEat;
                break;
            case wzh::SoundKind::kSearch:
            case wzh::SoundKind::kSteps:
                action = DinoAction::kListen;
                break;
            case wzh::SoundKind::kSleep:
                action = DinoAction::kSleepy;
                break;
            case wzh::SoundKind::kSneeze:
                action = DinoAction::kStartled;
                break;
            case wzh::SoundKind::kRoar:
                action = DinoAction::kProud;
                break;
            default:
                break;
        }
        // Do not answer every recording of the same category with the exact
        // same family. A little cross-family variation keeps the dinosaur from
        // looking like it has only one reaction per sound type.
        if (sound.kind != wzh::SoundKind::kRoar && irnd(100) < 35) {
            switch (action) {
                case DinoAction::kAffection:
                    action = DinoAction::kHappy;
                    break;
                case DinoAction::kHappy:
                    action = irnd(2) ? DinoAction::kProud : DinoAction::kAffection;
                    break;
                case DinoAction::kListen:
                    action = DinoAction::kDiscover;
                    break;
                case DinoAction::kSleepy:
                    action = DinoAction::kListen;
                    break;
                default:
                    break;
            }
        }
        const auto& set = kActionClips[static_cast<unsigned>(action)];
        const wzh_dino::MotionClip* reference[8]{};
        unsigned count = 0;
        for (unsigned i = 0; i < set.count && count < std::size(reference); ++i) {
            if (std::strncmp(set.clips[i].name, "ref-", 4) == 0)
                reference[count++] = &set.clips[i];
        }
        if (count) {
            const auto* clip = reference[sound_motion_.ReferenceVariant(sound, random, count)];
            sound_motion_.SetReferenceClip(*clip);
            ESP_LOGI(TAG, "Sound expression: reference=%s source=%s", clip->name,
                     wzh::UserPairAtIndex(sound.sound_index) ? "paired" : "flash");
        }
        if (sound.kind == wzh::SoundKind::kSneeze) {
            sound_motion_.SetReferenceClip(kSneezeClip);
        }
    }
    if (!wzh::UserPairAtIndex(sound.sound_index) && sound.kind == wzh::SoundKind::kRoar &&
        sound_motion_.NeedsBegin(sound)) {
        const auto* clip = PickSkyRoarClip(sound.duration_ms, &sound);
        sound_motion_.SetRoarClip(*clip, kCompactRoarTailAngles[last_sky_roar_clip_index_]);
    }
    wzh::SoundPose out{};
    bool powered = false;
    if (!sound_motion_.Update(
            sound, wzh::PetMechanism::kDinosaur,
            {last_output_pose_.neck_tilt, last_output_pose_.neck_lean, last_output_pose_.head_turn,
             last_output_pose_.tail_ud, last_output_pose_.tail_lr},
            now, out, powered, random))
        return false;

    flash_performance_active_.store(false);
    sky_roar_active_.store(false);
    if (powered) {
        quiet_center_.Reset();
        SetPose({out[0], out[1], out[2], out[3], out[4]});
    } else {
        servo_output_.Release();
    }
    return true;
}

void DinosaurController::Task() {
    // Audio-first periodic scores, with explicit interactions taking priority.
    while (true) {
        const uint32_t now_ms = esp_timer_get_time() / 1000;
        if (stop_audio_requested_.exchange(false) && audio_ != nullptr) {
            audio_->StopPlayback();
            CancelSoundMotion();
            autonomous_playback_ = false;
        }
        if (center_requested_.exchange(false)) {
            CancelSoundMotion();
            quiet_center_.Reset();
            autonomous_playback_ = false;
            request_active_.store(false);
            running_speech_bound_ = false;
        }
        PollInteraction(now_ms);
        const auto pet_state = pet_.Get(now_ms);
        if (shutdown_requested_.load()) {
            xQueueReset(action_queue_);
            ParkForShutdown();
        }
        if (servo_output_.blocked() || !pet_state.motion) {
            idle_breathing_ = false;
            CancelSoundMotion();
            autoplay_.Busy(now_ms, esp_random());
            autonomous_playback_ = false;
            request_active_.store(false);
            xQueueReset(action_queue_);
            if (audio_ != nullptr && audio_->GetMotionData().playing) {
                audio_->StopPlayback();
            }
            if (servo_output_.blocked()) {
                quiet_center_.Reset();
                servo_output_.Release();
            } else {
                // Quiet/rest/stop finish at neutral; protection still releases immediately.
                SettleToCenter(now_ms);
            }
            vTaskDelay(pdMS_TO_TICKS(kIdleUpdateMs));
            continue;
        }
        ActionRequest request;
        // 明确请求优先级最高。在线请求不播放本地音频，以免和 TTS 重叠。
        if (xQueueReceive(action_queue_, &request, 0) == pdTRUE &&
            now_ms - request.created_ms < 2500 && request.generation == intent_generation_.load() &&
            (!request.speech_bound || speaking_.load())) {
            if (autonomous_playback_ && audio_ != nullptr && audio_->Playing())
                audio_->StopPlayback();
            autonomous_playback_ = false;
            CancelSoundMotion();
            autoplay_.Busy(now_ms, esp_random());
            quiet_center_.Reset();
            bool sound_started = false;
            running_generation_ = request.generation;
            running_priority_.store(request.priority);
            running_speech_bound_ = request.speech_bound;
            request_gain_ =
                pet_state.gentle || pet_state.mode == wzh::PetMode::kQuiet ? .60f : 1.0f;
            request_active_.store(true);
            idle_breathing_ = false;
            // Motion can answer a new gesture without truncating an existing
            // Flash call. Explicit stop and conversation still cancel it.
            if (request.interaction != wzh::PetInteraction::kNone && !conversation_active_.load() &&
                audio_ != nullptr && pet_.CanPlayInteractionSound(now_ms)) {
                if (audio_->PlayInteractionSound(request.interaction)) {
                    pet_.InteractionSoundStarted(now_ms);
                    sound_started = true;
                }
            }
            if (sound_started) {
                // The exact accepted Flash file owns its score below.
            } else if (request.test_gpio >= 0) {
                RunServoTest(request.test_gpio);
            } else if (request.interaction != wzh::PetInteraction::kNone) {
                RunInteraction(request.interaction);
            } else if (request.is_body) {
                // 身体部位指令：直接播放对应剪辑，不随机镜像/幅度。
                RunBodyAction(static_cast<DinoBodyAction>(request.action));
            } else {
                DinoSoundSelection sound;
                if (request.with_local_sound && !conversation_active_.load() &&
                    pet_state.local_sound && audio_ != nullptr && pet_.CanPlayLocalSound(now_ms)) {
                    if (audio_->PlayForAction(request.action, &sound)) {
                        pet_.LocalSoundStarted(now_ms);
                        sound_started = true;
                    } else {
                        RunAction(request.action);
                    }
                } else {
                    RunAction(request.action, 0, false,
                              request.priority == wzh::PetPriority::kEmotion
                                  ? PickSoundClip(request.action)
                                  : nullptr);
                }
            }
            autoplay_.Busy(static_cast<uint32_t>(esp_timer_get_time() / 1000), esp_random());
            request_active_.store(false);
            running_speech_bound_ = false;
        } else {
            const bool local_motion = UpdateSoundMotion(now_ms);
            if (!local_motion && (audio_ == nullptr || !audio_->Playing()))
                autonomous_playback_ = false;
            const bool offline = !conversation_active_.load();
            if (autoplay_.Due(now_ms,
                              offline && pet_state.local_sound &&
                                  (!servo_output_.powered() || idle_breathing_) &&
                                  !local_motion && (audio_ == nullptr || !audio_->Playing()),
                              esp_random())) {
                if (audio_ != nullptr && audio_->PlayAutonomousSound(true, 15)) {
                    autonomous_playback_ = true;
                    running_generation_ = intent_generation_.load();
                    running_speech_bound_ = false;
                    request_gain_ = 1.0f;
                    idle_breathing_ = false;
                }
                autoplay_.Busy(now_ms, esp_random());
            }
            if (local_motion) {
                idle_breathing_ = false;
                quiet_center_.Reset();
                vTaskDelay(pdMS_TO_TICKS(kTickMs));
                continue;
            }
            const auto voice = audio_ != nullptr ? audio_->GetTtsMotionData() : DinoMotionData{};
            const bool audible = conversation_active_.load() &&
                                 (speaking_.load() || speech_tail_.load()) && voice.playing;
            const bool idle_eligible =
                !conversation_active_.load() && pet_state.motion && !request_active_.load() &&
                !local_motion && !audible;
            if (audible) {
                idle_breathing_ = false;
                quiet_center_.Reset();
                SetPose({70, 90, 90, 90, 90});
            } else if (idle_eligible) {
                // Between pulses keep the body breathing instead of freezing
                // at the neutral pose. A due autonomous sound may still
                // preempt this low-level motion.
                idle_breathing_ = true;
                if (!pet_state.local_sound && idle_life_.Due(now_ms, true, esp_random())) {
                    RunPulse(*PickPulse());
                    idle_life_.Busy(now_ms, esp_random());
                    continue;
                }
                RunIdlePose();
            } else {
                idle_breathing_ = false;
                if (!speaking_.load() && !voice.audible)
                    speech_tail_.store(false);
                SettleToCenter(now_ms);
            }
            vTaskDelay(pdMS_TO_TICKS(kTickMs));
        }
    }
}

bool DinosaurController::QueueRequest(ActionRequest request, wzh::PetPriority priority) {
    std::lock_guard<std::mutex> lock(request_mutex_);
    if (action_queue_ == nullptr || servo_output_.blocked() || shutdown_requested_.load() ||
        (request_active_.load() && priority != wzh::PetPriority::kExplicit &&
         priority <= running_priority_.load()) ||
        !pet_.Offer(priority, esp_timer_get_time() / 1000)) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    last_request_us_.store(now);
    if (!request.is_body && request.test_gpio < 0 && request.action == last_action_ &&
        last_action_time_us_ > 0 && last_action_generation_ == intent_generation_.load() &&
        now - last_action_time_us_ < 1200000) {
        return true;
    }
    request.created_ms = now / 1000;
    request.priority = priority;
    request.generation = intent_generation_.fetch_add(1) + 1;
    if (xQueueOverwrite(action_queue_, &request) != pdTRUE) {
        return false;
    }
    if (!request.is_body && request.test_gpio < 0) {
        last_action_ = request.action;
        last_action_time_us_ = now;
        last_action_generation_ = request.generation;
    }
    return true;
}

bool DinosaurController::TriggerAction(DinoAction action, bool with_local_sound,
                                       wzh::PetPriority priority, bool speech_bound) {
    ActionRequest request{action, with_local_sound, false};
    request.speech_bound = speech_bound;
    return QueueRequest(request, priority);
}

bool DinosaurController::HandleBodyCommand(const std::string& name) {
    DinoBodyAction action;
    if (!ParseBodyAction(name, action))
        return false;
    const bool accepted = TriggerBodyAction(action);
    if (accepted) {
        ESP_LOGI(TAG, "Local joint command=%s accepted", BodyActionName(action));
    } else {
        ESP_LOGW(TAG, "Local joint command=%s rejected", BodyActionName(action));
    }
    return true;
}

bool DinosaurController::TriggerBodyAction(DinoBodyAction action) {
    if (static_cast<unsigned>(action) >= static_cast<unsigned>(DinoBodyAction::kCount)) {
        return false;
    }
    return QueueRequest(ActionRequest{static_cast<DinoAction>(action), false, true},
                        wzh::PetPriority::kExplicit);
}

const wzh_dino::MotionClip* DinosaurController::PickBodyClip(DinoBodyAction action) {
    static_assert(std::size(kBodyClips) == static_cast<unsigned>(DinoBodyAction::kCount));
    const auto index = static_cast<unsigned>(action);
    return index < std::size(kBodyClips) ? &kBodyClips[index] : nullptr;
}

void DinosaurController::RunBodyAction(DinoBodyAction action) {
    const wzh_dino::MotionClip* clip = PickBodyClip(action);
    if (clip == nullptr)
        return;
    ESP_LOGI(TAG, "Body action: %s", clip->name);
    const float living_phase = frnd() * 6.2832f;
    // 固定放大身体指令，不随机镜像，保证方向和行程稳定。
    PlayClip(*clip, last_output_pose_, false, 1.40f, 1.0f, living_phase);
    const Pose rest = {SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT, SERVO_HEAD_TURN_DEFAULT,
                       SERVO_TAIL_UD_DEFAULT, SERVO_TAIL_LR_DEFAULT};
    PlaySegment(last_output_pose_, rest, 1000, wzh_dino::MotionEase::kSmooth, living_phase);
    PrepareIdleCenter();
    quiet_center_.Reset();
}

void DinosaurController::RunInteraction(wzh::PetInteraction interaction) {
    const int index = interaction_motion_.Select(interaction, esp_random());
    if (index < 0) {
        return;
    }
    const auto& clip = kInteractionClips[index];
    ESP_LOGI(TAG, "Interaction=%s motion=%s", wzh::InteractionName(interaction), clip.name);
    PlayClip(clip, current_pose_, false, 1.40f, 1.0f, frnd() * 6.2832f);
    PrepareIdleCenter();
    quiet_center_.Reset();
}

void DinosaurController::RunServoTest(int gpio) {
    // 用户显式调用的装配核对，仍在唯一动作任务执行；每次只测试一路，
    // 回到默认姿态后才改变选中关节。不用极限角验证方向。
    float Pose::* axis = nullptr;
    int low = 60;
    int high = 120;
    switch (gpio) {
        case 17:
            axis = &Pose::neck_tilt;
            low = 45;
            high = 95;
            break;
        case 16:
            axis = &Pose::neck_lean;
            break;
        case 15:
            axis = &Pose::head_turn;
            break;
        case 8:
            axis = &Pose::tail_ud;
            low = 70;
            break;
        case 18:
            axis = &Pose::tail_lr;
            break;
        default:
            return;
    }
    servo_test_active_ = true;
    if (audio_ != nullptr) {
        audio_->StopPlayback();
    }
    const Pose rest = {SERVO_NECK_TILT_DEFAULT, SERVO_NECK_LEAN_DEFAULT, SERVO_HEAD_TURN_DEFAULT,
                       SERVO_TAIL_UD_DEFAULT, SERVO_TAIL_LR_DEFAULT};
    PlaySegment(current_pose_, rest, 1200, MotionEase::kSmooth, 0);
    for (int angle : {low, high}) {
        Pose target = rest;
        target.*axis = angle;
        ESP_LOGI(TAG, "Servo test: IO%d -> %d deg (other axes at defaults)", gpio, angle);
        PlaySegment(current_pose_, target, 1200, MotionEase::kSmooth, 0);
        PlaySegment(target, target, 900, MotionEase::kLinear, 0);
        PlaySegment(target, rest, 1200, MotionEase::kSmooth, 0);
    }
    servo_test_active_ = false;
    PrepareIdleCenter();
    quiet_center_.Reset();
    ESP_LOGI(TAG, "Servo test: IO%d finished, default pose restored", gpio);
}

bool DinosaurController::ParseBodyAction(const std::string& name, DinoBodyAction& action) {
    const auto token = wzh::CanonicalBodyCommand(name, wzh::kBodyDinosaur);
    if (token.empty())
        return false;
    static const struct {
        const char* name;
        DinoBodyAction action;
    } actions[] = {
        {"shake_head", DinoBodyAction::kShakeHead},
        {"look_left", DinoBodyAction::kLookLeft},
        {"look_right", DinoBodyAction::kLookRight},
        {"look_up", DinoBodyAction::kLookUp},
        {"look_down", DinoBodyAction::kLookDown},
        {"wag_tail", DinoBodyAction::kWagTail},
        {"twist_neck", DinoBodyAction::kTwistNeck},
        {"head_center", DinoBodyAction::kHeadCenter},
        {"head_sway_left", DinoBodyAction::kHeadSwayLeft},
        {"head_sway_right", DinoBodyAction::kHeadSwayRight},
        {"head_scan", DinoBodyAction::kHeadScan},
        {"center", DinoBodyAction::kCenter},
        {"tail_up", DinoBodyAction::kTailUp},
        {"tail_down", DinoBodyAction::kTailDown},
        {"tail_left", DinoBodyAction::kTailLeft},
        {"tail_right", DinoBodyAction::kTailRight},
        {"tail_center", DinoBodyAction::kTailCenter},
        {"tail_level", DinoBodyAction::kTailLevel},
        {"tail_sway_left", DinoBodyAction::kTailSwayLeft},
        {"tail_sway_right", DinoBodyAction::kTailSwayRight},
        {"tail_circle", DinoBodyAction::kTailCircle},
        {"tail_eight", DinoBodyAction::kTailEight},
        {"tail_curl", DinoBodyAction::kTailCurl},
        {"wag_tail_slow", DinoBodyAction::kWagTailSlow},
        {"tail_fan", DinoBodyAction::kTailFan},
        {"tail_bounce", DinoBodyAction::kTailBounce},
        {"neck_left", DinoBodyAction::kNeckLeft},
        {"neck_right", DinoBodyAction::kNeckRight},
        {"neck_center", DinoBodyAction::kNeckCenter},
        {"neck_circle", DinoBodyAction::kNeckCircle},
        {"neck_reach", DinoBodyAction::kNeckReach},
        {"neck_stretch", DinoBodyAction::kNeckStretch},
        {"nod_head", DinoBodyAction::kNodHead},
    };
    for (const auto& item : actions) {
        if (token == item.name) {
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
        case DinoBodyAction::kHeadCenter:
            return "head_center";
        case DinoBodyAction::kHeadSwayLeft:
            return "head_sway_left";
        case DinoBodyAction::kHeadSwayRight:
            return "head_sway_right";
        case DinoBodyAction::kHeadScan:
            return "head_scan";
        case DinoBodyAction::kCenter:
            return "center";
        case DinoBodyAction::kTailUp:
            return "tail_up";
        case DinoBodyAction::kTailDown:
            return "tail_down";
        case DinoBodyAction::kTailLeft:
            return "tail_left";
        case DinoBodyAction::kTailRight:
            return "tail_right";
        case DinoBodyAction::kTailCenter:
            return "tail_center";
        case DinoBodyAction::kTailLevel:
            return "tail_level";
        case DinoBodyAction::kTailSwayLeft:
            return "tail_sway_left";
        case DinoBodyAction::kTailSwayRight:
            return "tail_sway_right";
        case DinoBodyAction::kTailCircle:
            return "tail_circle";
        case DinoBodyAction::kTailEight:
            return "tail_eight";
        case DinoBodyAction::kTailCurl:
            return "tail_curl";
        case DinoBodyAction::kWagTailSlow:
            return "wag_tail_slow";
        case DinoBodyAction::kTailFan:
            return "tail_fan";
        case DinoBodyAction::kTailBounce:
            return "tail_bounce";
        case DinoBodyAction::kNeckLeft:
            return "neck_left";
        case DinoBodyAction::kNeckRight:
            return "neck_right";
        case DinoBodyAction::kNeckCenter:
            return "neck_center";
        case DinoBodyAction::kNeckCircle:
            return "neck_circle";
        case DinoBodyAction::kNeckReach:
            return "neck_reach";
        case DinoBodyAction::kNeckStretch:
            return "neck_stretch";
        case DinoBodyAction::kNodHead:
            return "nod_head";
        default:
            return "unknown";
    }
}

bool DinosaurController::IsMaintenanceBusy() const {
    const int64_t now = esp_timer_get_time();
    const int64_t requested = last_request_us_.load();
    return maintenance_.load() || now < 30000000 || request_active_.load() ||
           servo_output_.powered() || (audio_ != nullptr && audio_->Playing()) ||
           (requested > 0 && now - requested < 30000000);
}

void DinosaurController::SetConversationActive(bool active) {
    const bool changed = conversation_active_.exchange(active) != active;
    if (!changed) {
        return;
    }
    ESP_LOGI(TAG, "Mode changed to %s",
             active ? "online/conversation" : "offline/interaction-wait");
    if (!active) {
        speaking_.store(false);
        speech_tail_.store(false);
        speech_intent_.store(wzh::SpeechIntent::kNeutral);
        pending_expression_.store(-1);
        // Closing a conversation consumes its pending/current expressions.
        // A reconnect or an idle transition must not resume stale choreography.
        intent_generation_.fetch_add(1);
    }
    if (active && audio_ != nullptr) {
        audio_->StopPlayback();
    }
    // Connecting/listening stay centered; only actual speech starts voice motion.
}

void DinosaurController::SetSpeaking(bool speaking) {
    const bool was_speaking = speaking_.exchange(speaking);
    if (speaking)
        speech_tail_.store(true);
    if (was_speaking && !speaking) {
        pending_expression_.store(-1);
    }
}

void DinosaurController::FeedOutputAudioLevel(float level, float attack) {
    output_audio_level_.store(std::clamp(level, 0.0f, 1.0f));
    output_audio_attack_.store(std::clamp(attack, 0.0f, 1.0f));
    output_audio_updated_us_.store(esp_timer_get_time());
}

bool DinosaurController::RequestCenter() {
    if (shutdown_requested_.load() || servo_output_.blocked())
        return false;
    intent_generation_.fetch_add(1);
    pending_expression_.store(-1);
    center_requested_.store(true);
    return true;
}

void DinosaurController::SettleToCenter(uint32_t now) {
    const auto sample = quiet_center_.Sample(
        {last_output_pose_.neck_tilt, last_output_pose_.neck_lean, last_output_pose_.head_turn,
         last_output_pose_.tail_ud, last_output_pose_.tail_lr},
        {70, 90, 90, 90, 90}, now, 60.f, 250);
    if (quiet_center_.powered(now)) {
        SetPose({sample[0], sample[1], sample[2], sample[3], sample[4]});
    } else {
        servo_output_.Release();
    }
}

void DinosaurController::SetDefaultPose() { shutdown_requested_.store(true); }

DinoAction DinosaurController::EmotionToAction(const std::string& emotion) const {
    // 同时兼容 XiaoZhi 常见的 llm.emotion 名称和 MCP 推荐的基础情绪名称。
    if (emotion == "sad" || emotion == "crying" || emotion == "depressed" || emotion == "lonely" ||
        emotion == "angry" || emotion == "frustrated" || emotion == "fearful" ||
        emotion == "scared" || emotion == "worried") {
        return DinoAction::kListen;
    }
    if (emotion == "happy" || emotion == "laughing" || emotion == "winking" || emotion == "funny" ||
        emotion == "excited" || emotion == "delighted" || emotion == "joyful" ||
        emotion == "proud") {
        return DinoAction::kHappy;
    }
    if (emotion == "loving" || emotion == "embarrassed" || emotion == "relaxed" ||
        emotion == "grateful" || emotion == "gratitude" || emotion == "love") {
        return DinoAction::kAffection;
    }
    if (emotion == "sad" || emotion == "crying" || emotion == "sleepy" || emotion == "tired" ||
        emotion == "bored" || emotion == "depressed" || emotion == "lonely") {
        return DinoAction::kSleepy;
    }
    if (emotion == "angry" || emotion == "confident" || emotion == "cool" || emotion == "annoyed" ||
        emotion == "frustrated") {
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
    if (wzh::ContainsAny(emotion, {"sad", "crying", "angry", "fear", "worried", "lonely"})) {
        pet_.Gentle(esp_timer_get_time() / 1000, 5000);
    }
    expression_since_.store(esp_timer_get_time() / 1000);
    pending_expression_.store(static_cast<int>(EmotionToAction(emotion)));
}

void DinosaurController::HandleUserSpeechEmotion(const char* text) {
    if (!conversation_active_.load() || text == nullptr || text[0] == '\0')
        return;
    if (HandlePetControl(wzh::ParsePetControl(text)))
        return;
    // Explicit cloud body intent is executed by MCP, once.
    if (!wzh::MatchBodyCommand(text, wzh::kBodyDinosaur).empty())
        return;
    React(wzh::ReactionFromText(text), wzh::PetPriority::kEmotion);
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
    mcp.AddTool(
        "self.dinosaur.get_pet_status",
        "查询宠物模式、运动保护、是否忙碌与对话状态。动作结果的accepted只代表请求接收，"
        "不代表实物执行完成；拒绝时解释reason，不要宣称做完。动作工具可传details=true返回详情。",
        PropertyList(), [this](const PropertyList&) -> ReturnValue {
            return wzh::PetStatus(pet_mode(), servo_output_.faults(), servo_output_.powered(),
                                  request_active_.load(), conversation_active_.load());
        });
    mcp.AddTool("self.dinosaur.pet_control",
                "宠物持续状态控制：stop立即停止，quiet安静陪伴，rest休息，wake恢复活动。"
                "普通动作和情绪不会自动解除stop/rest。",
                PropertyList({Property("command", kPropertyTypeString),
                              Property("details", kPropertyTypeBoolean, false)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    const bool accepted = HandlePetControl(
                        wzh::ParsePetControl(properties["command"].value<std::string>()));
                    if (properties["details"].value<bool>())
                        return RequestResult(accepted);
                    return accepted;
                });
    // react_to_user_emotion 面向模型语义判断；action 面向明确动作请求；
    // get_mode 让模型在需要时先确认设备是否在线。
    mcp.AddTool("self.dinosaur.react_to_user_emotion",
                "让恐龙根据用户此刻的情绪做出肢体动作。"
                "设备是一个恐龙机器人，通过摇头、伸脖子、摆尾巴来回应情绪。"
                "用户情绪明显且需要肢体回应时使用；每轮适量调用，避免重复表演。"
                "判断依据包括用户措辞、语义、上下文以及语音情绪线索；例如"
                "「我考了满分/太棒了/好开心」→ happy，「吓死我了/天啊」→ surprised，"
                "「好难过/想哭」→ sad（温和倾听），「气死我了」→ angry，「好喜欢你」→ loving，"
                "「好困/想睡觉」→ sleepy。"
                "emotion 取值：happy/loving/sad/angry/surprised/thinking/neutral。"
                "情绪不明显或不确定时传 neutral。",
                PropertyList({Property("emotion", kPropertyTypeString),
                              Property("details", kPropertyTypeBoolean, false)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    if (!conversation_active_.load()) {
                        return "offline mode: emotion control is paused";
                    }
                    const auto emotion = properties["emotion"].value<std::string>();
                    if (emotion == "neutral" || emotion == "normal") {
                        return false;
                    }
                    if (wzh::ContainsAny(emotion,
                                         {"sad", "crying", "angry", "fear", "worried", "lonely"})) {
                        pet_.Gentle(esp_timer_get_time() / 1000, 5000);
                    }
                    const bool accepted =
                        TriggerAction(EmotionToAction(emotion), false, wzh::PetPriority::kEmotion);
                    if (properties["details"].value<bool>())
                        return RequestResult(accepted);
                    return accepted;
                });
    mcp.AddTool("self.dinosaur.action",
                "执行一个明确的恐龙动作。仅在用户要求动作或当前语境强烈需要肢体表达时使用。action: "
                "discover, affection, happy, proud, eat, listen, startled, sleepy。",
                PropertyList({Property("action", kPropertyTypeString),
                              Property("details", kPropertyTypeBoolean, false)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    DinoAction action;
                    if (!ParseAction(properties["action"].value<std::string>(), action)) {
                        return "invalid action";
                    }
                    const bool accepted = TriggerAction(action, !conversation_active_.load());
                    if (properties["details"].value<bool>())
                        return RequestResult(accepted);
                    return accepted;
                });
    mcp.AddTool("self.dinosaur.body", wzh::BodyCommandHelp(wzh::kBodyDinosaur),
                PropertyList({Property("action", kPropertyTypeString),
                              Property("details", kPropertyTypeBoolean, false)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    DinoBodyAction action;
                    if (!ParseBodyAction(properties["action"].value<std::string>(), action)) {
                        return "invalid body action";
                    }
                    const bool accepted = TriggerBodyAction(action);
                    if (properties["details"].value<bool>())
                        return RequestResult(accepted);
                    return accepted;
                });
    mcp.AddUserOnlyTool(
        "self.dinosaur.test_servo",
        "仅在用户明确要求核对舵机接线或方向时使用。gpio 只能为17/16/15/8/18。"
        "全身先回默认姿态，再单独缓慢测试该GPIO的小范围角度，最后回中。"
        "测试期间关闭声音动作叠加。预期：17减小=仰、增大=俯；16/15减小=右、增大=左；"
        "8减小=下、增大=上；18减小=左、增大=右。需要用户观察实际运动部位。",
        PropertyList({Property("gpio", kPropertyTypeInteger, 8, 18)}),
        [this](const PropertyList& properties) -> ReturnValue {
            const int gpio = properties["gpio"].value<int>();
            if (gpio != 17 && gpio != 16 && gpio != 15 && gpio != 8 && gpio != 18) {
                return "invalid gpio: use 17, 16, 15, 8 or 18";
            }
            if (action_queue_ == nullptr || servo_output_.blocked() || shutdown_requested_.load()) {
                return false;
            }
            const ActionRequest request{DinoAction::kListen, false, false, gpio};
            return QueueRequest(request, wzh::PetPriority::kExplicit);
        });
    mcp.AddTool("self.dinosaur.body_capabilities", "查询实际支持的关节动作与动作名称；不执行动作。",
                PropertyList(), [](const PropertyList&) -> ReturnValue {
                    return wzh::BodyCommandHelp(wzh::kBodyDinosaur);
                });
    mcp.AddTool("self.dinosaur.get_mode", "返回恐龙当前 online 或 offline 模式。", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    return std::string(conversation_active_.load() ? "online" : "offline");
                });
}

wzh::PetMode DinosaurController::pet_mode() const {
    return pet_.Get(esp_timer_get_time() / 1000).mode;
}

bool DinosaurController::HandlePetControl(wzh::PetControl control, bool respond) {
    if (control == wzh::PetControl::kNone) {
        return false;
    }
    pet_.Control(control, esp_timer_get_time() / 1000);
    intent_generation_.fetch_add(1);
    pending_expression_.store(-1);
    stop_audio_requested_.store(control != wzh::PetControl::kWake || respond);
    const bool paused = control == wzh::PetControl::kStop || control == wzh::PetControl::kRest;
    servo_output_.SetFault(wzh::kPetPaused, paused);
    if (control == wzh::PetControl::kWake) {
        servo_output_.SetFault(wzh::kMotionTimeout, false);
        if (respond) {
            TriggerAction(DinoAction::kDiscover, !conversation_active_.load(),
                          wzh::PetPriority::kSensor);
        }
    } else {
        Application::GetInstance().StopInteraction();
    }
    ESP_LOGI(TAG, "Pet control=%d", static_cast<int>(control));
    return true;
}

void DinosaurController::SetMaintenance(bool blocked) {
    pet_.SetMaintenance(blocked);
    if (maintenance_.exchange(blocked) != blocked) {
        intent_generation_.fetch_add(1);
        stop_audio_requested_.store(true);
        pending_expression_.store(-1);
    }
    servo_output_.SetFault(wzh::kMaintenance, blocked);
}

bool DinosaurController::MotionInterrupted() const {
    return servo_output_.blocked() || shutdown_requested_.load() ||
           intent_generation_.load() != running_generation_ ||
           (running_speech_bound_ && !speaking_.load() &&
            (audio_ == nullptr || !audio_->GetTtsMotionData().playing));
}

bool DinosaurController::QueueInteraction(wzh::PetInteraction interaction) {
    DinoBodyAction body = DinoBodyAction::kShakeHead;
    switch (interaction) {
        case wzh::PetInteraction::kTouch1:
            body = DinoBodyAction::kShakeHead;
            break;
        case wzh::PetInteraction::kTouch2:
            body = DinoBodyAction::kLookLeft;
            break;
        case wzh::PetInteraction::kTouch3:
            body = DinoBodyAction::kLookRight;
            break;
        case wzh::PetInteraction::kTouch4:
        case wzh::PetInteraction::kPlayful:
            body = DinoBodyAction::kWagTail;
            break;
        case wzh::PetInteraction::kHeld:
        case wzh::PetInteraction::kPickedUp:
            body = DinoBodyAction::kLookLeft;
            break;
        case wzh::PetInteraction::kReleased:
        case wzh::PetInteraction::kPutDown:
            body = DinoBodyAction::kLookRight;
            break;
        case wzh::PetInteraction::kShaken:
        case wzh::PetInteraction::kFlipped:
            body = DinoBodyAction::kShakeHead;
            break;
        default:
            return false;
    }
    ActionRequest request{static_cast<DinoAction>(body), false, true};
    request.interaction = interaction;
    const bool accepted = QueueRequest(request, wzh::PetPriority::kExplicit);
    if (accepted) {
        ESP_LOGD(TAG, "Interaction=%s accepted", wzh::InteractionName(interaction));
    } else {
        ESP_LOGD(TAG, "Interaction=%s rejected faults=0x%lx mode=%d",
                 wzh::InteractionName(interaction),
                 static_cast<unsigned long>(servo_output_.faults()), int(pet_mode()));
    }
    return accepted;
}

void DinosaurController::HandleTouch(uint8_t code) {
    const uint32_t now = esp_timer_get_time() / 1000;
    pet_.Touch(code, now);
    PollInteraction(now);
}

bool DinosaurController::HandleSensorEvent(int event) {
    const auto kind = static_cast<wzh::Sensors::Event>(event);
    const uint32_t now = esp_timer_get_time() / 1000;
    if (kind == wzh::Sensors::Event::kHuman) {
        if (!pet_.SensorReady(now) || pet_.Get(now).mode != wzh::PetMode::kNormal)
            return false;
        const bool accepted =
            TriggerAction(DinoAction::kDiscover, false, wzh::PetPriority::kSensor);
        if (accepted)
            pet_.SensorAccepted(now);
        return accepted;
    }
    if (!pet_.Get(now).motion)
        return false;
    if (request_active_.load() || servo_output_.powered()) {
        return false;
    }
    const auto interaction = kind == wzh::Sensors::Event::kPickedUp ? wzh::PetInteraction::kPickedUp
                             : kind == wzh::Sensors::Event::kShaken ? wzh::PetInteraction::kShaken
                             : kind == wzh::Sensors::Event::kFlipped
                                 ? wzh::PetInteraction::kFlipped
                                 : wzh::PetInteraction::kPutDown;
    const bool accepted = QueueInteraction(interaction);
    if (accepted) {
        pet_.Gentle(now, kind == wzh::Sensors::Event::kPutDown ? 1800 : 3500);
    }
    return accepted;
}

void DinosaurController::HandleAssistantSpeech(const char* text) {
    if (text == nullptr || !conversation_active_.load()) {
        return;
    }
    speech_intent_.store(wzh::SpeechIntentFromText(text));
    const auto reaction = wzh::ReactionFromText(text);
    if (reaction == wzh::PetReaction::kNone) {
        return;
    }
    if (reaction == wzh::PetReaction::kComfort) {
        pet_.Gentle(esp_timer_get_time() / 1000, 5000);
    }
    const DinoAction action = reaction == wzh::PetReaction::kHappy       ? DinoAction::kHappy
                              : reaction == wzh::PetReaction::kAffection ? DinoAction::kAffection
                              : reaction == wzh::PetReaction::kComfort   ? DinoAction::kListen
                              : reaction == wzh::PetReaction::kProud     ? DinoAction::kProud
                              : reaction == wzh::PetReaction::kEat       ? DinoAction::kEat
                              : reaction == wzh::PetReaction::kStartled  ? DinoAction::kStartled
                              : reaction == wzh::PetReaction::kSleepy    ? DinoAction::kSleepy
                                                                         : DinoAction::kDiscover;
    expression_since_.store(esp_timer_get_time() / 1000);
    pending_expression_.store(static_cast<int>(action));
}

bool DinosaurController::React(wzh::PetReaction reaction, wzh::PetPriority priority,
                               bool speech_bound) {
    if (reaction == wzh::PetReaction::kNone) {
        return false;
    }
    if (reaction == wzh::PetReaction::kComfort) {
        pet_.Gentle(esp_timer_get_time() / 1000, 5000);
    }
    const DinoAction action = reaction == wzh::PetReaction::kHappy       ? DinoAction::kHappy
                              : reaction == wzh::PetReaction::kAffection ? DinoAction::kAffection
                              : reaction == wzh::PetReaction::kComfort   ? DinoAction::kListen
                              : reaction == wzh::PetReaction::kProud     ? DinoAction::kProud
                              : reaction == wzh::PetReaction::kEat       ? DinoAction::kEat
                              : reaction == wzh::PetReaction::kStartled  ? DinoAction::kStartled
                              : reaction == wzh::PetReaction::kSleepy    ? DinoAction::kSleepy
                                                                         : DinoAction::kDiscover;
    const auto state = pet_.Get(esp_timer_get_time() / 1000);
    return TriggerAction(action, !conversation_active_.load() && state.local_sound, priority,
                         speech_bound);
}

void DinosaurController::PollInteraction(uint32_t now) {
    const auto touch = pet_.PollTouchEvent(now);
    if (touch.response != wzh::TouchResponse::kNone) {
        QueueInteraction(touch.interaction());
    }
    int expression = pending_expression_.load();
    if (expression >= 0 && now - expression_since_.load() >= 2500) {
        pending_expression_.compare_exchange_strong(expression, -1);
    } else if (expression >= 0 && speaking_.load() && audio_ != nullptr &&
               audio_->GetTtsMotionData().playing) {
        int expected = expression;
        if (pending_expression_.compare_exchange_strong(expected, -1)) {
            TriggerAction(static_cast<DinoAction>(expression), false, wzh::PetPriority::kEmotion,
                          true);
        }
    }
}
