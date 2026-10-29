#ifndef WZH_DINOSAUR_CONTROLLER_H_
#define WZH_DINOSAUR_CONTROLLER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "wzh_pet_behavior.h"
#include "wzh_quiet_center.h"
#include "wzh_servo_output.h"
#include "wzh_sound_choreography.h"
#include "wzh_speech_intent.h"
#include "wzh_voice_motion.h"

struct cJSON;
class DinosaurAudio;
struct DinoSoundSelection;

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
    kNeckLeft,
    kNeckRight,
    kNeckCenter,
    kNeckCircle,
    kNeckReach,
    kNeckStretch,
    kNodHead,
    kCount,
};

namespace wzh_dino {

// 关键帧动作引擎。kKeep 表示该轴在本帧保持上一姿态；同一帧内所有发生
// 变化的轴始终共用进度、同步起停。
constexpr int kKeep = 0x7FFFFFFF;

enum class MotionEase : uint8_t {
    kSmooth,   // 三次平滑，起止速度为零
    kFastOut,  // 保留编舞语义；播放时仍受统一慢速和平滑约束
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
    bool authored_dynamics = false;
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
    wzh::ServoOutput& servo_output() { return servo_output_; }
    void SetConversationActive(bool active);
    void SetSpeaking(bool speaking);
    void SetMaintenance(bool blocked);
    void HandleTouch(uint8_t code);
    bool HandleSensorEvent(int event);
    void HandleAssistantSpeech(const char* text);
    bool HandlePetControl(wzh::PetControl control, bool respond = true);
    wzh::PetMode pet_mode() const;

    // AudioService 输出任务调用：输入扬声器实际播放的 PCM 响度和重音。
    // 本函数仅写原子状态，不能在音频任务中直接运行舵机动作。
    void FeedOutputAudioLevel(float level, float attack);
    bool IsMaintenanceBusy() const;
    bool IsConversationActive() const { return conversation_active_.load(); }
    void HandleLlmEmotion(const char* emotion);
    void HandleUserSpeechEmotion(const char* text);
    bool TriggerAction(DinoAction action, bool with_local_sound = false,
                       wzh::PetPriority priority = wzh::PetPriority::kExplicit,
                       bool speech_bound = false);
    // 身体部位级动作（摇头/看左/看右/抬头/低头/摇尾巴/扭脖子）。队列与
    // TriggerAction 共用，在线对话时也会正常执行。
    bool TriggerBodyAction(DinoBodyAction action);
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
    float output_angles_[5] = {70, 90, 90, 90, 90};
    void PollInteraction(uint32_t now);
    bool MotionInterrupted() const;

    wzh::ServoOutput servo_output_;
    std::atomic<bool> shutdown_requested_{false};
    // 五个字段都是最终舵机角度，不使用旧工程 tail_ud 的反向内部坐标：
    // tilt 0=仰/180=俯；lean/head 0=右/180=左；tail_ud 55=下/180=上；
    // tail_lr 0=左/180=右。首次烧录仍需确认具体装配的机械限位。
    struct Pose {
        float neck_tilt;
        float neck_lean;
        float head_turn;
        float tail_ud;
        float tail_lr;
    };

    struct ActionRequest {
        DinoAction action;
        bool with_local_sound;
        bool is_body;
        int test_gpio = -1;
        uint32_t created_ms = 0;
        uint32_t generation = 0;
        wzh::PetPriority priority = wzh::PetPriority::kExplicit;
        bool speech_bound = false;
        wzh::PetInteraction interaction = wzh::PetInteraction::kNone;
    };

    bool QueueRequest(ActionRequest request, wzh::PetPriority priority);
    bool QueueInteraction(wzh::PetInteraction interaction);
    bool React(wzh::PetReaction reaction, wzh::PetPriority priority, bool speech_bound = false);

    DinosaurAudio* audio_ = nullptr;
    // 队列有界，防止服务端连续情绪事件造成动作无限堆积。
    QueueHandle_t action_queue_ = nullptr;
    // 对话状态回调和动作任务都会读取该值，因此使用原子变量。这里的
    // “在线”专指 connecting/listening/speaking，不等同于 Wi-Fi 已连接。
    std::atomic<bool> conversation_active_{false};
    // speaking 用于让在线 TTS 比提示音拥有稍强的声音随动增益。
    std::atomic<bool> speaking_{false};
    std::atomic<bool> speech_tail_{false};
    std::atomic<bool> center_requested_{false};
    std::atomic<wzh::SpeechIntent> speech_intent_{wzh::SpeechIntent::kNeutral};
    wzh::QuietCenter<5> quiet_center_;
    void SettleToCenter(uint32_t now);
    Pose current_pose_{70, 90, 90, 90, 90};
    Pose last_output_pose_{70, 90, 90, 90, 90};
    // 仅动作任务设置；单轴核对时关闭声音/生命叠加。
    bool servo_test_active_ = false;
    float request_gain_ = 1.0f;
    DinoAction last_action_ = DinoAction::kListen;
    int64_t last_action_time_us_ = 0;
    uint32_t last_action_generation_ = 0;
    // 待机微动作（转头/甩尾/嗅闻/精神一振）的下一次触发时间（毫秒）。
    uint32_t next_pulse_ms_ = 0;
    int last_pulse_index_ = -1;
    // 音频动作使用低通后的响度，避免每个音节直接变成一次舵机抽动。
    float audio_motion_level_ = 0.0f;
    float audio_motion_emphasis_ = 0.0f;
    float audio_motion_phase_ = 0.0f;
    int64_t audio_motion_last_update_us_ = 0;
    bool audio_motion_active_ = false;
    float audio_motion_direction_ = 1.0f;
    std::atomic<float> output_audio_level_{0.0f};
    std::atomic<float> output_audio_attack_{0.0f};
    std::atomic<int64_t> output_audio_updated_us_{0};
    // Flash 主体编舞期间提高声音随动表现力；仍只读取低通包络并受最终限幅。
    std::atomic<bool> flash_performance_active_{false};
    // 怒吼的主语义必须是仰头；声音和生命叠加不得覆盖编舞本身的轻微侧仰。
    std::atomic<bool> sky_roar_active_{false};
    // 每个行为族分别记住完整/短声上一次剪辑，避免连续复读同一轨迹。
    wzh::MotionShuffleBag<16> action_clip_bags_[8];
    int last_sound_clip_indices_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int last_sky_roar_clip_index_ = -1;
    wzh::SoundVariantSelector<4> roar_variants_;
    bool last_mirror_ = false;
    int8_t mirror_balance_ = 0;
    uint8_t mirror_run_length_ = 0;
    // 待机个性姿态状态。
    int idle_pose_index_ = 0;
    uint32_t next_idle_pose_ms_ = 0;
    uint32_t idle_breath_epoch_ms_ = 0;
    float idle_drift_[5] = {70.0f, 90.0f, 90.0f, 90.0f, 90.0f};

    static void TaskEntry(void* arg);
    void Task();
    bool UpdateSoundMotion(uint32_t now);
    void CancelSoundMotion();
    wzh::SoundChoreography sound_motion_;
    wzh::VoiceMotion voice_motion_;
    wzh::AutoplayCadence autoplay_;
    wzh::IdleCadence idle_life_;
    bool idle_breathing_ = false;
    bool autonomous_playback_ = false;
    void ParkForShutdown();
    cJSON* RequestResult(bool accepted) const;
    void InitializeServos();
    void RunServoTest(int gpio);
    void SetPose(const Pose& pose);
    void MoveTo(const Pose& target, uint32_t duration_ms);
    void RunAction(DinoAction action, uint32_t sound_duration_ms = 0, bool flash_sync = false,
                   const wzh_dino::MotionClip* forced_clip = nullptr, bool allow_mirror = true);
    void RunSoundAction(DinoAction action, const DinoSoundSelection& sound);
    DinoAction ClassifyLocalSound(int sound_index) const;
    bool IsSkyRoarSound(int sound_index) const;
    void WaitForLocalSoundStart(int sound_index) const;
    // 待机"个性姿态"：缓慢漂移到随机的多轴坐姿（平静/警觉/困倦/好奇…），
    // 再叠加呼吸正弦，让静止的恐龙不再只是单摆。
    void RunIdlePose();
    void PrepareIdleCenter();
    // 身体部位动作专用：不随机镜像/幅度，直接按剪辑播放。
    void RunBodyAction(DinoBodyAction action);
    void RunInteraction(wzh::PetInteraction interaction);
    static const wzh_dino::MotionClip* PickBodyClip(DinoBodyAction action);
    static bool ParseBodyAction(const std::string& name, DinoBodyAction& action);
    static const char* BodyActionName(DinoBodyAction action);
    // 关键帧引擎辅助。
    Pose AttentionPose(DinoAction action, bool mirror) const;
    Pose ResolveKeyframe(const wzh_dino::Keyframe& key, const Pose& prev, bool mirror,
                         float amplitude, bool authored_dynamics = false) const;
    uint32_t ComfortableDuration(const Pose& from, const Pose& to, uint32_t requested_ms) const;
    const wzh_dino::MotionClip* PickClip(DinoAction action);
    const wzh_dino::MotionClip* PickSoundClip(DinoAction action);
    const wzh_dino::MotionClip* PickSkyRoarClip(uint32_t sound_duration_ms,
                                                const wzh::SoundPlayback* sound = nullptr);
    const wzh_dino::MotionClip* PickPulse();
    bool PickBalancedMirror();
    void PlaySegment(const Pose& from, const Pose& to, uint32_t duration_ms,
                     wzh_dino::MotionEase ease, float living_phase, const Pose* previous = nullptr,
                     const Pose* next = nullptr, uint32_t before_ms = 0, uint32_t after_ms = 0,
                     bool duration_resolved = false);
    Pose PlayClip(const wzh_dino::MotionClip& clip, const Pose& start, bool mirror, float amplitude,
                  float duration_factor, float living_phase);
    void ApplyLiving(Pose& pose, float living_phase, float strength) const;
    void RunPulse(const wzh_dino::MotionClip& clip);
    DinoAction EmotionToAction(const std::string& emotion) const;
    static bool ParseAction(const std::string& name, DinoAction& action);
    static const char* ActionName(DinoAction action);
};

#endif  // WZH_DINOSAUR_CONTROLLER_H_
