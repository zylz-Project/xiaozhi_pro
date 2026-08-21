#ifndef WZH_CRAWLPANDA_CONTROLLER_H_
#define WZH_CRAWLPANDA_CONTROLLER_H_

#include <atomic>
#include <cstdint>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class PandaAudio;
struct PandaMotionData;

// 大模型和离线逻辑统一使用的动作集合。动作名称同时作为 MCP 对外接口，
// 增删或改名时需要同步检查 ParseAction、ActionName 和服务端提示词。
enum class PandaAction : uint8_t {
    kDiscover,
    kAffection,
    kHappy,
    kProud,
    kEat,
    kListen,
    kStartled,
    kSleepy,
};

namespace wzh_panda {

struct Pose {
    float head;
    float left_arm;
    float right_arm;
};

struct Keyframe {
    uint16_t duration_ms;
    int16_t head;
    int16_t left_arm;
    int16_t right_arm;
};

struct Clip {
    const char* name;
    const Keyframe* frames;
    uint8_t count;
    bool mirrorable;
};

// 行为族与旧工程 crawlPanda 保持一致。PandaAction 会先映射到行为族，
// 行为族再随机挑选具体动作剪辑。
enum class Behaviour : uint8_t {
    kIdle,
    kCurious,
    kEat,
    kBaby,
    kProud,
    kAffection,
    kPlayful,
    kAmbient,
    kSettle,
    kCount,
};

struct ShuffleBag {
    int8_t items[4] = {-1, -1, -1, -1};
    uint8_t count = 0;
    uint8_t position = 0;
    int8_t previous = -1;
};

}  // namespace wzh_panda

// 三路舵机“爬爬熊猫”动作控制器。
//
// 移植自旧工程 crawlPanda 的 living-motion 引擎：关键帧连续插值 + 声音
// 包络跟随（手臂交替扒动 + 呼吸起伏）+ 常驻微动层（呼吸/凝视漂移/手臂微摆/
// 肌肉微颤）。外部回调只负责向 action_queue_ 投递请求，全部舵机插值和耗时
// 动作都在 panda_motion 任务中串行执行，因此网络回调和主循环不会被阻塞。
class PandaController {
public:
    bool Initialize(PandaAudio* audio);
    void RegisterMcpTools();
    void SetConversationActive(bool active);
    void SetSpeaking(bool speaking);
    bool IsConversationActive() const { return conversation_active_.load(); }
    void HandleLlmEmotion(const char* emotion);
    void HandleUserSpeechEmotion(const char* text);
    bool TriggerAction(PandaAction action, bool with_local_sound = false);

private:
    struct ActionRequest {
        PandaAction action;
        bool with_local_sound;
    };

    PandaAudio* audio_ = nullptr;
    QueueHandle_t action_queue_ = nullptr;
    // 对话状态回调和动作任务都会读取该值，因此使用原子变量。这里的
    // “在线”专指 connecting/listening/speaking，不等同于 Wi-Fi 已连接。
    std::atomic<bool> conversation_active_{false};
    // 仅在 speaking 状态为真：在线 TTS 播放期间用它合成轻量舞动数据。
    std::atomic<bool> speaking_{false};
    PandaAction last_action_ = PandaAction::kListen;
    int64_t last_action_time_us_ = 0;

    // ---- 动作引擎状态（关键帧插值 + 指数追踪）----
    wzh_panda::Pose pose_{90.0f, 90.0f, 90.0f};
    wzh_panda::Pose from_{90.0f, 90.0f, 90.0f};
    const wzh_panda::Clip* clip_ = nullptr;
    uint8_t frame_ = 0;
    uint32_t frame_started_ms_ = 0;
    uint32_t frame_duration_ms_ = 1;
    float tempo_ = 1.0f;
    bool mirror_ = false;
    bool clip_active_ = false;
    int mirror_balance_ = 0;
    wzh_panda::ShuffleBag bags_[static_cast<size_t>(wzh_panda::Behaviour::kCount)];

    // ---- 自主调度状态 ----
    uint32_t next_idle_ms_ = 0;
    uint32_t next_sound_ms_ = 0;
    uint32_t next_ambient_ms_ = 0;
    bool was_playing_ = false;
    wzh_panda::Behaviour sound_behaviour_ = wzh_panda::Behaviour::kCurious;

    // ---- 常驻微动层状态 ----
    float breath_phase_ = 0.0f;
    uint32_t breath_period_ = 3400;
    float gaze_pos_ = 0.0f;
    float gaze_target_ = 0.0f;
    uint32_t gaze_next_ms_ = 0;
    float tremor_phase_ = 0.0f;

    static void TaskEntry(void* arg);
    void Task();
    void InitializeServos();
    void SetServoAngles(int head, int left_arm, int right_arm);
    void OutputPose(const wzh_panda::Pose& pose);
    void StartClip(int clip_id, uint32_t now_ms);
    void UpdatePlayer(uint32_t now_ms);
    int ChooseClip(wzh_panda::Behaviour behaviour);
    bool QueueRandomSound(const char* category);
    wzh_panda::Behaviour ClassifySound(int index) const;
    void AddMicroMotion(wzh_panda::Pose& pose, uint32_t now_ms, float life);
    wzh_panda::Pose ApplyAudioMotion(const wzh_panda::Pose& pose, const PandaMotionData& audio,
                                     uint32_t now_ms);
    void FeedSpeakingMotion(PandaMotionData& motion);
    static wzh_panda::Behaviour ActionToBehaviour(PandaAction action);
    PandaAction EmotionToAction(const std::string& emotion) const;
    static bool ParseAction(const std::string& name, PandaAction& action);
    static const char* ActionName(PandaAction action);
};

#endif  // WZH_CRAWLPANDA_CONTROLLER_H_