"""Run the production motion-task loops with a clock, queue and hardware substitute."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function_body(text, marker):
    start = text.index(marker)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


HARNESS = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include "wzh_pet_behavior.h"
#include "wzh_sound_choreography.h"
#include "wzh_quiet_center.h"
#include "wzh_speech_intent.h"
uint32_t clock_ms = 30000;
unsigned esp_random(){return 0;}
int irnd(int){return 0;}
// Run production loops with a deterministic clock and real choreography/cadence.
constexpr uint32_t kIdleTestDurationMs = 60000U;
int scenario = 0, auto_sounds = 0, stops = 0;
int animations = 0, sounds = 0, writes = 0, releases = 0, invites = 0;
bool inject = false, injected = false, inject_interaction = false;
struct Finished {};
uint32_t esp_log_timestamp() { return clock_ms; }
int64_t esp_timer_get_time() { return int64_t(clock_ms) * 1000; }
void vTaskDelay(uint32_t ticks) {
    clock_ms += ticks;
    if (clock_ms >= 30000U + kIdleTestDurationMs) throw Finished{};
}
using TickType_t=uint32_t;
TickType_t xTaskGetTickCount(){return clock_ms;}
void vTaskDelayUntil(TickType_t* next,uint32_t dt){*next+=dt;vTaskDelay(*next>clock_ms?*next-clock_ms:0);}
#define pdMS_TO_TICKS(ms) (ms)
constexpr int pdTRUE = 1;
constexpr int kTickMs = 20, kIdleUpdateMs = 20;
constexpr int BODY_COUNT = 6, C_PLAYFUL_CRAWL = 19, C_PLAYFUL_CRAWL_BOTH = 20;
constexpr int SERVO_HEAD_DEFAULT = 90, SERVO_LEFT_ARM_DEFAULT = 90, SERVO_RIGHT_ARM_DEFAULT = 90;
constexpr int SERVO_TAIL_LR_DEFAULT = 90, SERVO_TAIL_UD_DEFAULT = 90;
constexpr int SERVO_NECK_TILT_DEFAULT = 70, SERVO_NECK_LEAN_DEFAULT = 90, SERVO_HEAD_TURN_DEFAULT = 90;
const char* TAG = "test";
template<class... T> void fake_log(const T&...) {}
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
#define ESP_LOGD(...) fake_log(__VA_ARGS__)
namespace wzh { using MotionData = SoundPlayback; }
using PandaMotionData = wzh::MotionData;
using DinoMotionData = wzh::MotionData;
struct Pose {
    float head = 90, left_arm = 90, right_arm = 90, tail_lr = 90, tail_ud = 90;
    float neck_tilt = 70, neck_lean = 90, head_turn = 90;
};
namespace wzh_panda { using Pose = ::Pose; }
enum class Behaviour { kIdle, kCurious, kAmbient, kPlayful };
enum class Action { kDiscover, kAffection, kHappy, kProud, kEat, kListen, kStartled, kSleepy };
using DinoAction = Action;
using PandaAction = Action;
using DinoBodyAction = Action;
using PandaBodyAction = Action;
struct DinoSoundSelection {};
struct RoarFrame{int neck_tilt=0,neck_lean=90,head_turn=90,tail_ud=150,tail_lr=90;};
struct RoarClip{unsigned count=1;RoarFrame frames[1];};
namespace wzh_dino {
enum class MotionEase {kSmooth,kFastOut,kLinear};
constexpr int kKeep=0x7fffffff;
struct Keyframe{int neck_tilt,neck_lean,head_turn,tail_ud,tail_lr;uint16_t duration_ms;MotionEase ease;};
struct MotionClip{const char* name;const Keyframe* frames;uint8_t count;bool authored_dynamics=false;};
}
#include "__REFERENCE_HEADER__"
using wzh_dino::MotionClip;
const MotionClip reference_clips[]={
    {"ref-discover",reference_motion::kDiscoverFrames,std::size(reference_motion::kDiscoverFrames)},
    {"ref-affection",reference_motion::kAffectionFrames,std::size(reference_motion::kAffectionFrames)},
    {"ref-happy",reference_motion::kHappyFrames,std::size(reference_motion::kHappyFrames)},
    {"ref-proud",reference_motion::kProudCallFrames,std::size(reference_motion::kProudCallFrames)},
    {"ref-eat",reference_motion::kEatFrames,std::size(reference_motion::kEatFrames)},
    {"ref-listen",reference_motion::kListenFrames,std::size(reference_motion::kListenFrames)},
    {"ref-startled",reference_motion::kStartledFrames,std::size(reference_motion::kStartledFrames)},
    {"ref-sleepy",reference_motion::kSleepyFrames,std::size(reference_motion::kSleepyFrames)},
};
const MotionClip kSneezeClip=reference_clips[0];
struct ClipSet{const MotionClip* clips;unsigned count;};
const ClipSet kActionClips[]={{&reference_clips[0],1},{&reference_clips[1],1},{&reference_clips[2],1},{&reference_clips[3],1},
    {&reference_clips[4],1},{&reference_clips[5],1},{&reference_clips[6],1},{&reference_clips[7],1}};
constexpr float kCompactRoarTailAngles[]={166,112,130,154,120,148,140};
struct ActionRequest {
    Action action = Action::kDiscover;
    bool with_local_sound = true, is_body = false, speech_bound = false;
    int test_gpio = -1;
    uint32_t created_ms = 0, generation = 0;
    wzh::PetPriority priority = wzh::PetPriority::kExplicit;
    wzh::PetInteraction interaction = wzh::PetInteraction::kNone;
};
ActionRequest pending;
bool queued = false;
void xQueueReset(void*) { queued = false; }
int xQueueReceive(void*, ActionRequest* out, int) {
    if (!queued) return 0;
    *out = pending; queued = false; return pdTRUE;
}
struct Audio {
    uint32_t start=0, end=0, sequence=0, duration=2000;
    bool Start() {
        if(Playing()) return false;
        start=clock_ms;duration=scenario==5?26514:2000;end=start+duration;++sequence;++sounds;
        return true;
    }
    bool PlayAutonomousSound(bool include_ambient=true,uint32_t ambient_percent=100) {
        (void)include_ambient;(void)ambient_percent;
        if(!Start()) return false;
        ++auto_sounds;return true;
    }
    int SoundCountByCategory(const char*) { return 12; }
    bool Playing() { return end>clock_ms; }
    void StopPlayback() { end=0; ++stops; }
    wzh::MotionData GetMotionData() {
        wzh::MotionData data;
        data.playing=Playing();data.audible=data.playing;
        data.sequence=sequence;data.duration_ms=duration;data.elapsed_ms=clock_ms-start;
        data.score=wzh::FindSoundScore("Cute_baby_panda_voca");return data;
    }
    wzh::MotionData GetTtsMotionData() { return {}; }
    bool PlayForAction(Action, DinoSoundSelection* = nullptr) { return Start(); }
    bool PlayInteractionSound(wzh::PetInteraction) { return Start(); }
} audio;
struct Output {
    bool powered() const { return rail; }
    bool rail=false;
    bool blocked() { return scenario==7&&clock_ms<60000; }
    void Release() { ++releases; rail=false; }
    void SetFault(int, bool) {}
};
namespace wzh { constexpr int kShutdown = 8; }
class Controller {
public:
    wzh::PetBehavior pet_;
    wzh::QuietCenter<__AXES__> quiet_center_;
    std::atomic<bool> speech_tail_{false}, center_requested_{false};
    std::atomic<wzh::SpeechIntent> speech_intent_{wzh::SpeechIntent::kNeutral};
    void SettleToCenter(uint32_t);
    wzh::InteractionMotionSelector interaction_motion_;
    Audio* audio_ = &audio;
    Output servo_output_;
    std::atomic<bool> shutdown_requested_{false}, stop_audio_requested_{false};
    std::atomic<bool> conversation_active_{false}, speaking_{false}, request_active_{false};
    std::atomic<bool> flash_performance_active_{false}, sky_roar_active_{false};
    int last_sky_roar_clip_index_=0;
    std::atomic<uint32_t> intent_generation_{0};
    std::atomic<wzh::PetPriority> running_priority_{wzh::PetPriority::kSensor};
    std::atomic<float> output_audio_level_{0}, output_audio_attack_{0};
    std::atomic<int64_t> output_audio_updated_us_{0};
    float audio_motion_level_ = 0, audio_motion_emphasis_ = 0, request_gain_ = 1;
    bool audio_motion_active_ = false;
    void* action_queue_ = this;
    bool clip_active_ = false, running_speech_bound_ = false;
    uint32_t running_generation_ = 0, last_motion_ms_ = 0;
    Pose pose_, last_output_pose_, current_pose_;
    std::atomic<int> pending_expression_{-1};
    void ParkForShutdown() { servo_output_.Release(); }
    void Task();
    wzh::SoundChoreography sound_motion_;
    wzh::AutoplayCadence autoplay_;
    wzh::IdleCadence idle_life_;
    bool idle_breathing_ = false;
    bool autonomous_playback_=false;
    bool UpdateSoundMotion(uint32_t);
    void CancelSoundMotion();
    void SetConversationActive(bool);
    void TriggerAction(Action, bool, wzh::PetPriority) { ++invites; }
    void PollInteraction(uint32_t now) {
        if (((scenario==3&&now>=39500) || scenario==5) && !injected) {
            injected=queued=true;pending.created_ms=now;
            pending.interaction=wzh::PetInteraction::kTouch1;pending.is_body=true;
            pending.generation=intent_generation_.load();
        }
        if (scenario==4&&now>=42000) {
            pet_.Control(wzh::PetControl::kStop,now);
            stop_audio_requested_.store(true);
        }
    }
    bool MotionInterrupted() { return false; }
    void SetServoAngles(int, int, int) { ++writes; }
    void SetPose(const std::array<float,5>&) { ++writes; servo_output_.rail=true; }
    void OutputPose(const Pose&) { ++writes; servo_output_.rail=true; }
    int clip_ticks_ = 0;
    void StartClip(int, uint32_t) { ++animations; clip_active_ = true; clip_ticks_ = 0; }
    void UpdatePlayer(uint32_t) {
        if (!clip_active_) return;
        ++writes;
        OutputPose(pose_);
        if (++clip_ticks_ >= 250) clip_active_ = false;
    }
    int BodyClipId(Action) { return 0; }
    int ActionToBehaviour(Action) { return 0; }
    int ChooseClip(int) { return 0; }
    int ChooseClip(Behaviour) { return 0; }
    Behaviour ClassifySound(int) { return Behaviour::kIdle; }
    int ChooseOfflineSoundClip(int,wzh::SoundKind,uint32_t);
    int C_COUNT = 28;
    Pose ApplyAudioMotion(const Pose& p, const wzh::MotionData&, uint32_t) { return p; }
    void AddMicroMotion(Pose&, uint32_t, float) {}
    void RunServoTest(int) { ++animations; }
    void RunBodyAction(Action) { ++animations; }
    void RunInteraction(wzh::PetInteraction) { ++animations; }
    void RunSoundAction(Action, const DinoSoundSelection&) { ++animations; }
    void RunAction(Action, int = 0, bool = false, const int* = nullptr) { ++animations; }
    const int* PickSoundClip(Action) { return nullptr; }
    const MotionClip* PickPulse() { return &reference_clips[0]; }
    void RunPulse(const MotionClip&) { ++animations; }
    const RoarClip* PickSkyRoarClip(uint32_t,const wzh::SoundPlayback* = nullptr){static RoarClip clip;return &clip;}
    void RunIdlePose() { ++animations; }
};
__SOUND_METHODS__
__TASK__
__CONVERSATION__
int main(int argc, char** argv) {
    scenario=argc>1?std::atoi(argv[1]):0;
    Controller controller;
    if(scenario==1)controller.pet_.Control(wzh::PetControl::kQuiet,clock_ms);
    if(scenario==2)controller.SetConversationActive(true);
    if(scenario==6)controller.pet_.SetMaintenance(true);
    try { controller.Task(); } catch (const Finished&) {}
    if(scenario==0){assert(auto_sounds>=3&&auto_sounds<=20&&writes>100);}
    if(scenario==1||scenario==2||scenario==6){assert(auto_sounds==0&&sounds==0);}
    if(scenario==3){assert(auto_sounds>=1&&sounds>auto_sounds);}
    if(scenario==4){assert(auto_sounds>=1&&sounds==auto_sounds&&stops>=1);}
    if(scenario==5){assert(sounds>=1&&auto_sounds<=1&&writes>100);}
    if(scenario==7){assert(auto_sounds>=1);}
}
'''

class WzhEventDrivenTest(unittest.TestCase):
    def test_production_tasks_coverage_preemption_modes_and_long_recordings(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required')
        for board, stem, cls in [('wzh_crawlPanda','panda','PandaController'),
                                 ('wzh_tailRedPanda','panda','PandaController'),
                                 ('wzh_dinosaur','dinosaur','DinosaurController')]:
            source=(ROOT/'main/boards'/board/(stem+'_controller.cc')).read_text()
            methods='\n'.join(function_body(source, marker).replace(cls+'::','Controller::')
                for marker in ['void '+cls+'::CancelSoundMotion()', 'bool '+cls+'::UpdateSoundMotion(', 'void '+cls+'::SettleToCenter('])
            if board=='wzh_crawlPanda':
                methods += '\n'+function_body(source,'int PandaController::ChooseOfflineSoundClip(').replace(cls+'::','Controller::')
            task=function_body(source,'void '+cls+'::Task()').replace(cls+'::','Controller::')
            conversation=function_body(source,'void '+cls+'::SetConversationActive(').replace(cls+'::','Controller::')
            for scenario in range(8):
                with self.subTest(board=board,scenario=scenario), tempfile.TemporaryDirectory() as directory:
                    harness=HARNESS.replace('__REFERENCE_HEADER__',str(ROOT/'main/boards/wzh_dinosaur/dinosaur_reference_motion.h')).replace('__AXES__', '5' if stem=='dinosaur' else '3').replace('__TASK__',task).replace('__CONVERSATION__',conversation).replace('__SOUND_METHODS__',methods)
                    if scenario==5:
                        harness=harness.replace('60000U','5000U')
                    cpp=Path(directory)/'task.cc';cpp.write_text(harness)
                    exe=cpp.with_suffix('')
                    result=subprocess.run([compiler,'-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT/'main/boards/common/wzh'),str(cpp),'-o',str(exe)],capture_output=True,text=True)
                    self.assertEqual(result.returncode,0,result.stderr)
                    result=subprocess.run([str(exe),str(scenario)],capture_output=True,text=True,timeout=15)
                    self.assertEqual(result.returncode,0,result.stderr)
