#ifndef WZH_TAILREDPANDA_CONTROLLER_H_
#define WZH_TAILREDPANDA_CONTROLLER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "wzh_audio_player.h"
#include "wzh_pet_behavior.h"
#include "wzh_quiet_center.h"
#include "wzh_servo_output.h"
#include "wzh_sound_choreography.h"
#include "wzh_speech_intent.h"
#include "wzh_voice_motion.h"

struct cJSON;
class PandaAudio;

using PandaMotionData = wzh::MotionData;

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

// 身体部位级动作（语音控制："摇摇头/向左看/摇尾巴/扭扭脖子"等）。
// 名称同时作为 MCP self.panda.body 的参数，增删时同步更新 BodyActionName。
enum class PandaBodyAction : uint8_t {
    kShakeHead,
    kLookLeft,
    kLookRight,
    kWagTail,
    kTwistNeck,
    kHeadCenter,
    kHeadSwayLeft,
    kHeadSwayRight,
    kHeadScan,
    kCenter,
    kTailUp,
    kTailDown,
    kTailLeft,
    kTailRight,
    kTailCenter,
    kTailLevel,
    kTailSwayLeft,
    kTailSwayRight,
    kTailCircle,
    kTailEight,
    kTailCurl,
    kWagTailSlow,
    kTailFan,
    kTailBounce,
    kCount,
};

namespace wzh_panda {

struct Pose {
    float head;
    float tail_lr;
    float tail_ud;
};

// 行为族与旧工程 tailRedPanda 保持一致。PandaAction 会先映射到行为族，
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
    kStartled,
    kCount,
};

constexpr uint8_t kFamilyWidth = 16;
struct ShuffleBag {
    int8_t items[kFamilyWidth] = {};
    uint8_t count = 0;
    uint8_t position = 0;
    int8_t previous = -1;
};

struct Keyframe {
    uint16_t duration_ms;
    int16_t head;
    int16_t tail_lr;
    int16_t tail_ud;
};

struct Clip {
    const char* name;
    const Keyframe* frames;
    uint8_t count;
    bool mirrorable;
    bool authored_dynamics = false;
};

}  // namespace wzh_panda

// 三路舵机“灵动”动作控制器。
//
// 移植自旧工程 tailRedPanda 的 living-motion 引擎：关键帧连续目标流 +
// 指数追踪 + 常驻微动层（呼吸/凝视漂移/尾巴轻摆/肌肉微颤）+ 声音包络
// 跟随。外部回调只负责向 action_queue_ 投递请求，全部舵机插值和耗时
// 动作都在 panda_motion 任务中串行执行，因此网络回调和主循环不会被阻塞。
class PandaController {
public:
    bool Initialize(PandaAudio* audio);
    void RegisterMcpTools();
    wzh::ServoOutput& servo_output() { return servo_output_; }
    void SetConversationActive(bool active);
    void SetSpeaking(bool speaking);
    void SetMaintenance(bool blocked);
    void HandleTouch(uint8_t code);
    bool HandleSensorEvent(int event);
    void HandleAssistantSpeech(const char* text);
    bool HandlePetControl(wzh::PetControl control, bool respond = true);
    wzh::PetMode pet_mode() const;

    bool IsMaintenanceBusy() const;
    bool IsConversationActive() const { return conversation_active_.load(); }
    void HandleLlmEmotion(const char* emotion);
    void HandleUserSpeechEmotion(const char* text);
    bool TriggerAction(PandaAction action, bool with_local_sound = false,
                       wzh::PetPriority priority = wzh::PetPriority::kExplicit,
                       bool speech_bound = false);
    // 身体部位级动作（摇头/看左/看右/摇尾巴/扭脖子）。队列与 TriggerAction
    // 共用，在线对话时也会正常执行。
    bool TriggerBodyAction(PandaBodyAction action);
    bool HandleBodyCommand(const std::string& name);
    // 关机序列用：请求动作任务回默认姿态并释放，调用方不直接写 PWM。
    void SetDefaultPose();
    bool RequestCenter();

private:
    std::mutex request_mutex_;
    wzh::PetBehavior pet_;
    wzh::InteractionMotionSelector interaction_motion_;
    std::atomic<bool> maintenance_{false};
    std::atomic<bool> stop_audio_requested_{false};
    std::atomic<uint32_t> intent_generation_{0};
    std::atomic<int64_t> last_request_us_{0};
    uint32_t running_generation_ = 0;
    std::atomic<bool> request_active_{false};
    std::atomic<wzh::PetPriority> running_priority_{wzh::PetPriority::kSensor};
    bool running_speech_bound_ = false;
    std::atomic<int> pending_expression_{-1};
    std::atomic<uint32_t> expression_since_{0};
    uint32_t last_output_ms_ = 0;
    uint32_t last_pose_log_ms_ = 0;
    void PollInteraction(uint32_t now);
    bool MotionInterrupted() const;

    wzh::ServoOutput servo_output_;
    std::atomic<bool> shutdown_requested_{false};
    struct ActionRequest {
        PandaAction action;
        bool with_local_sound;
        bool is_body;
        uint32_t created_ms = 0;
        uint32_t generation = 0;
        wzh::PetPriority priority = wzh::PetPriority::kExplicit;
        bool speech_bound = false;
        wzh::PetInteraction interaction = wzh::PetInteraction::kNone;
    };

    bool QueueRequest(ActionRequest request, wzh::PetPriority priority);
    bool QueueInteraction(wzh::PetInteraction interaction);
    bool React(wzh::PetReaction reaction, wzh::PetPriority priority, bool speech_bound = false);

    PandaAudio* audio_ = nullptr;
    QueueHandle_t action_queue_ = nullptr;
    // 对话状态回调和动作任务都会读取该值，因此使用原子变量。这里的
    // “在线”专指 connecting/listening/speaking，不等同于 Wi-Fi 已连接。
    std::atomic<bool> conversation_active_{false};
    // 仅在 speaking 状态为真：在线 TTS 播放期间用它合成轻量舞动数据。
    std::atomic<bool> speaking_{false};
    std::atomic<bool> speech_tail_{false};
    std::atomic<bool> center_requested_{false};
    std::atomic<wzh::SpeechIntent> speech_intent_{wzh::SpeechIntent::kNeutral};
    wzh::QuietCenter<3> quiet_center_;
    void SettleToCenter(uint32_t now);
    PandaAction last_action_ = PandaAction::kListen;
    int64_t last_action_time_us_ = 0;
    uint32_t last_action_generation_ = 0;

    wzh_panda::Pose last_output_pose_{90.0f, 90.0f, 150.0f};
    uint32_t FrameDuration(uint8_t frame) const;

    // ---- 动作引擎状态（连续目标流 + 指数追踪）----
    wzh_panda::Pose pose_{90.0f, 90.0f, 150.0f};
    wzh_panda::Pose from_{90.0f, 90.0f, 150.0f};
    const wzh_panda::Clip* clip_ = nullptr;
    uint8_t frame_ = 0;
    uint32_t frame_started_ms_ = 0;
    uint32_t frame_duration_ms_ = 1;
    wzh_panda::Pose clip_origin_{90, 90, 150};
    float clip_gain_ = 1.0f;
    wzh::VoiceMotion voice_motion_;
    uint32_t motion_seed_ = 0;
    float tempo_ = 1.0f;
    bool mirror_ = false;
    bool clip_active_ = false;
    uint32_t last_motion_ms_ = 0;
    int mirror_balance_ = 0;
    int last_clip_ = -1;
    wzh_panda::ShuffleBag bags_[static_cast<size_t>(wzh_panda::Behaviour::kCount)];

    // ---- 自主调度状态 ----
    uint32_t clip_finished_ms_ = 0;
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
    bool UpdateSoundMotion(uint32_t now);
    void CancelSoundMotion();
    wzh::SoundChoreography sound_motion_;
    wzh::AutoplayCadence autoplay_;
    wzh::IdleCadence idle_life_;
    bool autonomous_playback_ = false;
    void ParkForShutdown();
    cJSON* RequestResult(bool accepted) const;
    void InitializeServos();
    void SetServoAngles(int head, int tail_lr, int tail_ud);
    void OutputPose(const wzh_panda::Pose& pose);
    void StartClip(int clip_id, uint32_t now_ms);
    void UpdatePlayer(uint32_t now_ms);
    int ChooseClip(wzh_panda::Behaviour behaviour);
    bool QueueRandomSound(const char* category);
    wzh_panda::Behaviour ClassifySound(int index) const;
    void AddMicroMotion(wzh_panda::Pose& pose, uint32_t now_ms, float life);
    wzh_panda::Pose ApplyAudioMotion(const wzh_panda::Pose& pose, const PandaMotionData& audio,
                                     uint32_t now_ms);
    static wzh_panda::Behaviour ActionToBehaviour(PandaAction action);
    static int BodyClipId(PandaBodyAction action);
    static bool ParseBodyAction(const std::string& name, PandaBodyAction& action);
    static const char* BodyActionName(PandaBodyAction action);
    PandaAction EmotionToAction(const std::string& emotion) const;
    static bool ParseAction(const std::string& name, PandaAction& action);
    static const char* ActionName(PandaAction action);
};

#endif  // WZH_TAILREDPANDA_CONTROLLER_H_
