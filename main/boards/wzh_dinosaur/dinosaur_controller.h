#ifndef WZH_DINOSAUR_CONTROLLER_H_
#define WZH_DINOSAUR_CONTROLLER_H_

#include <atomic>
#include <cstdint>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class DinosaurAudio;

// 大模型和离线逻辑统一使用的动作集合。动作名称同时作为 MCP 对外接口，
// 增删或改名时需要同步检查 ParseAction、ActionName 和服务端提示词。
enum class DinoAction : uint8_t {
    kDiscover,
    kAffection,
    kHappy,
    kProud,
    kEat,
    kListen,
    kStartled,
    kSleepy,
};

// 身体部位级动作（语音控制："摇摇头/向左看/抬头/低头/摇尾巴/扭脖子"等）。
// 名称同时作为 MCP self.dinosaur.body 的参数，增删时同步更新 BodyActionName。
enum class DinoBodyAction : uint8_t {
    kShakeHead,
    kLookLeft,
    kLookRight,
    kLookUp,
    kLookDown,
    kWagTail,
    kTwistNeck,
    kCount,
};

namespace wzh_dino {

// 关键帧动作引擎。kKeep 表示该轴沿用上一帧，这是让头部先动、脖子随后、
// 尾巴最后响应的动物时序核心。
constexpr int kKeep = 0x7FFFFFFF;

enum class MotionEase : uint8_t {
    kSmooth,   // 三次平滑，起止速度为零
    kFastOut,  // 快速冲出后减速，适合突然探头/甩尾
    kLinear,   // 匀速，适合保持姿态
};

struct Keyframe {
    int neck_tilt;
    int neck_lean;
    int head_turn;
    int tail_ud;
    int tail_lr;
    uint16_t duration_ms;
    MotionEase ease;
};

struct MotionClip {
    const char* name;
    const Keyframe* frames;
    uint8_t count;
};

}  // namespace wzh_dino

// 五路舵机动作控制器。
//
// 外部回调只负责向 action_queue_ 投递请求；全部舵机插值和耗时动作都在
// dino_motion 任务中串行执行，因此网络回调和主循环不会被舵机动作阻塞。
class DinosaurController {
public:
    bool Initialize(DinosaurAudio* audio);
    void RegisterMcpTools();
    void SetConversationActive(bool active);
    void SetSpeaking(bool speaking);
    bool IsConversationActive() const { return conversation_active_.load(); }
    void HandleLlmEmotion(const char* emotion);
    void HandleUserSpeechEmotion(const char* text);
    bool TriggerAction(DinoAction action, bool with_local_sound = false);
    // 身体部位级动作（摇头/看左/看右/抬头/低头/摇尾巴/扭脖子）。队列与
    // TriggerAction 共用，在线对话时也会正常执行。
    bool TriggerBodyAction(DinoBodyAction action);
    // 关机序列用：直接写回默认安全姿态（不经过动作队列）。
    void SetDefaultPose();

private:
    // 五个字段均为舵机角度（0～180 度），并不是物理空间中的绝对角。
    // 实机装配方向可能不同，首次烧录必须先校准默认值和机械限位。
    struct Pose {
        int neck_tilt;
        int neck_lean;
        int head_turn;
        int tail_ud;
        int tail_lr;
    };

    struct ActionRequest {
        DinoAction action;
        bool with_local_sound;
        bool is_body;
    };

    DinosaurAudio* audio_ = nullptr;
    // 队列有界，防止服务端连续情绪事件造成动作无限堆积。
    QueueHandle_t action_queue_ = nullptr;
    // 对话状态回调和动作任务都会读取该值，因此使用原子变量。这里的
    // “在线”专指 connecting/listening/speaking，不等同于 Wi-Fi 已连接。
    std::atomic<bool> conversation_active_{false};
    // 仅在 speaking 状态为真：TTS 播放期间叠加随音频响度摆动的微层。
    std::atomic<bool> speaking_{false};
    Pose current_pose_{70, 90, 90, 90, 90};
    DinoAction last_action_ = DinoAction::kListen;
    int64_t last_action_time_us_ = 0;
    // 待机微动作（转头/甩尾/嗅闻/精神一振）的下一次触发时间（毫秒）。
    uint32_t next_pulse_ms_ = 3000;
    // TTS 摆动相位（每次 SetPose 递增，用于让摆动连续不抽动）。
    float speak_phase_ = 0.0f;
    // 待机个性姿态状态。
    int idle_pose_index_ = 0;
    uint32_t next_idle_pose_tick_ = 0;
    float idle_drift_[5] = {70.0f, 90.0f, 90.0f, 90.0f, 90.0f};

    static void TaskEntry(void* arg);
    void Task();
    void InitializeServos();
    void SetPose(const Pose& pose);
    void MoveTo(const Pose& target, uint32_t duration_ms);
    void RunAction(DinoAction action);
    // 待机"个性姿态"：缓慢漂移到随机的多轴坐姿（平静/警觉/困倦/好奇…），
    // 再叠加呼吸正弦，让静止的恐龙不再只是单摆。
    void RunIdlePose(uint32_t tick);
    // 身体部位动作专用：不随机镜像/幅度，直接按剪辑播放。
    void RunBodyAction(DinoBodyAction action);
    static const wzh_dino::MotionClip* PickBodyClip(DinoBodyAction action);
    static bool ParseBodyAction(const std::string& name, DinoBodyAction& action);
    static const char* BodyActionName(DinoBodyAction action);
    // 关键帧引擎辅助。
    Pose AttentionPose(DinoAction action, bool mirror) const;
    Pose ResolveKeyframe(const wzh_dino::Keyframe& key, const Pose& prev, bool mirror,
                         float amplitude) const;
    const wzh_dino::MotionClip* PickClip(DinoAction action) const;
    const wzh_dino::MotionClip* PickPulse() const;
    void PlaySegment(const Pose& from, const Pose& to, uint32_t duration_ms,
                     wzh_dino::MotionEase ease, float living_phase);
    void ApplyLiving(Pose& pose, float living_phase) const;
    void RunPulse(const wzh_dino::MotionClip& clip);
    DinoAction EmotionToAction(const std::string& emotion) const;
    static bool ParseAction(const std::string& name, DinoAction& action);
    static const char* ActionName(DinoAction action);
};

#endif  // WZH_DINOSAUR_CONTROLLER_H_
