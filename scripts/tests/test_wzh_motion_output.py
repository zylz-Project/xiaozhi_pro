"""Exercise real interpolation and output limits, not just authored target tables."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function as function

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / 'main/boards/common/wzh'


class WzhMotionOutputTest(unittest.TestCase):
    def run_cpp(self, program):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)/'motion.cc'
            source.write_text(program)
            result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                     '-Werror', '-I', str(COMMON), str(source), '-o', str(source.with_suffix(''))],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(source.with_suffix(''))], capture_output=True, text=True,
                                    errors='replace', timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)

    def panda_program(self, board):
        base = ROOT/'main/boards'/board
        source = (base/'panda_controller.cc').read_text()
        types = function((base/'panda_controller.h').read_text(), 'namespace wzh_panda')
        first = 'constexpr Keyframe kIdleListen' if board=='wzh_crawlPanda' else 'constexpr Keyframe kIdleGlance'
        tables = source[source.index(first):source.index('bool Contains(')]
        for name in ("panda_reference_motion.h", "panda_story_motion.h"):
            tables = tables.replace(f'#include "{name}"', f'#include "{base / name}"')
        axes = ['head','left_arm','right_arm'] if board=='wzh_crawlPanda' else ['head','tail_lr','tail_ud']
        neutral = [90,90,90] if board=='wzh_crawlPanda' else [90,90,150]
        easing = function(source, 'float ease_motion(' if board=='wzh_crawlPanda' else 'float smoothstep(')
        program = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>
#include "wzh_pet_behavior.h"
#include "wzh_body_commands.h"
#include "wzh_voice_motion.h"
#include "wzh_quiet_center.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
uint32_t clock_ms=10000;
int64_t esp_timer_get_time(){return int64_t(clock_ms)*1000;}
constexpr uint32_t kTickMs=20;
int irnd(int){return 0;}
unsigned esp_random(){return 0;}
int irand(int low,int){return low;}
float frand(float,float){return 1;}
struct PandaMotionData {bool playing=false;float level=0,attack=0;};
struct Output {
    bool locked=false;int releases=0;void Release(){++releases;}
    std::vector<std::array<int,3>> samples;
    unsigned faults(){return locked?1:0;}
    bool WriteDegrees(const float* values,size_t count){
        assert(count==3);if(locked)return false;
        samples.push_back({int(std::lround(values[0])),int(std::lround(values[1])),int(std::lround(values[2]))});return true;
    }
};
'''+types+'\nusing namespace wzh_panda;\n'+easing
        style = base/'panda_motion_style.h'
        if style.exists():
            program = program.replace('#include "wzh_pet_behavior.h"',
                                      '#include "wzh_pet_behavior.h"\n#include "'+str(style)+'"')
        program += '\nconstexpr size_t kBehaviourCount=static_cast<size_t>(Behaviour::kCount);\n'
        program += tables+'\n'+function(source,'Pose TargetPose(')
        center = ','.join(map(str,neutral))
        program += r'''
struct PandaController {
    wzh::PetBehavior pet_;Output servo_output_;wzh::VoiceMotion voice_motion_;
    std::atomic<wzh::SpeechIntent> speech_intent_{wzh::SpeechIntent::kNeutral};
    std::atomic<bool> speech_tail_{false};
    Pose pose_{CENTER},from_{CENTER},last_output_pose_{CENTER},clip_origin_{CENTER};
    float clip_gain_=1;
    const Clip* clip_=nullptr;
    uint8_t frame_=0;
    uint32_t frame_started_ms_=0,frame_duration_ms_=1,last_output_ms_=0,last_pose_log_ms_=0;
    uint32_t motion_seed_=0;
    float tempo_=1;int crawl_mode_=0;bool mirror_=false,clip_active_=false,running_speech_bound_=false;
    int mirror_balance_=2;
    std::atomic<bool> request_active_{false};
    std::atomic<wzh::PetPriority> running_priority_{wzh::PetPriority::kEmotion};
    std::atomic<unsigned> intent_generation_{0};unsigned running_generation_=0;
    uint32_t FrameDuration(uint8_t) const;
    void StartClip(int,uint32_t);void UpdatePlayer(uint32_t);void OutputPose(const Pose&);
    wzh::QuietCenter<3> quiet_center_;
    void SettleToCenter(uint32_t);
    Pose ApplyAudioMotion(const Pose&,const PandaMotionData&,uint32_t);
};
'''.replace('CENTER',center)
        if style.exists():
            program = program.replace('float clip_gain_=1;',
                                      'float clip_gain_=1;wzh_crawl::VoiceGesture voice_gesture_;')
        for marker in ['uint32_t PandaController::FrameDuration(', 'void PandaController::StartClip(',
                       'void PandaController::UpdatePlayer(', 'void PandaController::OutputPose(',
                       'wzh_panda::Pose PandaController::ApplyAudioMotion(', 'void PandaController::SettleToCenter(']:
            program += function(source,marker)+'\n'
        program += 'std::array<float,3> Values(const Pose& p){return {'+','.join('p.'+a for a in axes)+'};}\n'
        program += r'''
int main(){
    for(int mode=0;mode<3;++mode){
        for(int id=0;id<C_COUNT+BODY_COUNT+int(std::size(kInteractionClips));++id){
            clock_ms=10000;
            PandaController c;
            if(mode==1)c.pet_.Gentle(clock_ms,120000);
            if(mode==2)c.pet_.Control(wzh::PetControl::kQuiet,clock_ms);
            c.StartClip(id,clock_ms);
            const auto* clip=c.clip_;
            const float gain=mode==0?1.0f:.6f;
            auto center=Values(c.pose_);
            auto expected_min=center,expected_max=center;
            for(uint8_t frame=0;frame<clip->count;++frame){
                auto target=Values(TargetPose(*clip,frame,false));
                for(int axis=0;axis<3;++axis){
                    auto value=center[axis]+(target[axis]-center[axis])*gain;
                    expected_min[axis]=std::min(expected_min[axis],value);
                    expected_max[axis]=std::max(expected_max[axis],value);
                }
            }
            int steps=0;
            auto previous=Values(c.last_output_pose_);
            do{
                c.UpdatePlayer(clock_ms);c.OutputPose(c.pose_);
                auto actual=Values(c.last_output_pose_);
                for(int axis=0;axis<3;++axis)assert(std::abs(actual[axis]-previous[axis])<=wzh::kPetServoSpeed*.02f+.001f);
                previous=actual;clock_ms+=20;++steps;
                assert(steps<2900); // Must complete within the existing 60-second power budget.
            }while(c.clip_active_);
            for(int i=0;i<20;++i,clock_ms+=20)c.OutputPose(c.pose_);
            std::array<float,3> minimum=center,maximum=center;
            for(auto sample:c.servo_output_.samples){
                for(int axis=0;axis<3;++axis){
                    assert(sample[axis]>=LOW[axis]&&sample[axis]<=HIGH[axis]);
                    minimum[axis]=std::min(minimum[axis],float(sample[axis]));
                    maximum[axis]=std::max(maximum[axis],float(sample[axis]));
                }
            }
            for(int axis=0;axis<3;++axis){
                if(minimum[axis]>expected_min[axis]+3||maximum[axis]<expected_max[axis]-3){
                    std::fprintf(stderr,"%s mode=%d axis=%d expected=%.1f..%.1f actual=%.1f..%.1f\n",
                        clip->name,mode,axis,expected_min[axis],expected_max[axis],minimum[axis],maximum[axis]);
                    assert(false);
                }
            }
            if(id>=C_COUNT+BODY_COUNT){
                float onset=0;
                for(size_t i=0;i<std::min<size_t>(31,c.servo_output_.samples.size());++i)
                    for(int axis=0;axis<3;++axis)
                        onset=std::max(onset,std::abs(c.servo_output_.samples[i][axis]-center[axis]));
                assert(onset>=(mode==0?5:3)); // Visible command within 600 ms of dispatch.
                auto end=Values(c.last_output_pose_);
                for(int axis=0;axis<3;++axis)assert(std::abs(end[axis]-center[axis])<=3);
                assert(steps<600); // Every acknowledgement finishes within 12 seconds.
            }
            if(mode==0){
                if(id==C_COUNT+BODY_SHAKE_HEAD){
                    assert(minimum[0]>=30&&minimum[0]<=36&&maximum[0]>=144&&maximum[0]<=150);
                    assert(steps*20<=6000); // Full-range gesture includes its slower distance-based return.
                }
                if(id==C_COUNT+BODY_LOOK_LEFT)assert(maximum[0]>=144&&maximum[0]<=150);
                if(id==C_COUNT+BODY_LOOK_RIGHT)assert(minimum[0]>=30&&minimum[0]<=36);
            }
            // A denied PWM write must not advance the remembered physical command.
            c.servo_output_.locked=true;
            auto before=Values(c.last_output_pose_);c.OutputPose(Pose{8,8,172});
            assert(before==Values(c.last_output_pose_));
        }
    }
    // Exercise the actual player across a nonstationary shared waypoint.
    clock_ms=10000;
    PandaController flow;flow.StartClip(0,clock_ms);
    const Keyframe stream[]={{600,105,90,LAST_CENTER},{600,120,90,LAST_CENTER},
                             {600,130,90,LAST_CENTER},{600,90,90,LAST_CENTER}};
    const Clip stream_clip={"continuous",stream,4,false,true};
    flow.clip_=&stream_clip;flow.tempo_=1;flow.mirror_=false;
    flow.frame_duration_ms_=flow.FrameDuration(0);
    uint32_t boundary=10000+flow.frame_duration_ms_;
    flow.UpdatePlayer(boundary-20);float left=flow.pose_.head;
    flow.UpdatePlayer(boundary);float joint=flow.pose_.head;
    flow.UpdatePlayer(boundary+20);float right=flow.pose_.head;
    float incoming=(joint-left)/.02f,outgoing=(right-joint)/.02f;
    assert(incoming>10&&std::abs(incoming-outgoing)<incoming*.2f);
    PandaController settle;clock_ms=100000;
    settle.pet_.Control(wzh::PetControl::kQuiet,clock_ms);
    settle.last_output_pose_={30,172,12}; // interrupted voice output
    settle.pose_={90,90,LAST_CENTER};    // logical clip pose is deliberately different
    auto previous_center=Values(settle.last_output_pose_);
    for(unsigned time=0;time<6000;time+=20,clock_ms+=20){
        settle.SettleToCenter(clock_ms);
        auto actual=Values(settle.last_output_pose_);
        for(unsigned a=0;a<3;++a)assert(std::abs(actual[a]-previous_center[a])<=2.801f);
        previous_center=actual;
    }
    assert(std::abs(previous_center[0]-90)<.01f&&std::abs(previous_center[1]-90)<.01f);
    assert(std::abs(previous_center[2]-LAST_CENTER)<.01f&&settle.servo_output_.releases>0);
    auto count=settle.servo_output_.samples.size();
    settle.SettleToCenter(clock_ms+5000);assert(settle.servo_output_.samples.size()==count);
    PandaController audio;
    clock_ms=10000;
    for(int i=0;i<750;++i,clock_ms+=20){
        audio.OutputPose(audio.ApplyAudioMotion(audio.pose_,{true,.4f,.6f},clock_ms));
    }
    auto center=Values(audio.pose_);
    std::array<float,3> movement{};
    for(auto sample:audio.servo_output_.samples){
        for(int axis=0;axis<3;++axis){
            assert(sample[axis]>=LOW[axis]&&sample[axis]<=HIGH[axis]);
            movement[axis]=std::max(movement[axis],std::abs(sample[axis]-center[axis]));
        }
    }
    assert(movement[0]>=15&&movement[1]>=15&&movement[2]>=15);
    EXTRA
}
'''
        lows = [30,8,8] if board=='wzh_crawlPanda' else [30,6,0]
        highs = [150,172,172] if board=='wzh_crawlPanda' else [150,174,180]
        program=program.replace('LAST_CENTER',str(neutral[2]))
        program=program.replace('LOW[axis]','(std::array<int,3>{'+','.join(map(str,lows))+'})[axis]')
        program=program.replace('HIGH[axis]','(std::array<int,3>{'+','.join(map(str,highs))+'})[axis]')
        extra=''
        if board=='wzh_tailRedPanda':
            extra=r'''
    // An already raised emotion pose must leave room for the full voice
    // phrase instead of pinning the tail up for the whole utterance.
    PandaController high_tail;
    float min_ud=180,max_ud=0;
    for(unsigned t=0;t<20000;t+=20){
        auto p=high_tail.ApplyAudioMotion({160,160,20},{true,.4f,.6f},30000+t);
        assert(p.head>8&&p.head<172&&p.tail_lr>6&&p.tail_lr<174);
        assert(p.tail_ud>0&&p.tail_ud<180);
        min_ud=std::min(min_ud,p.tail_ud);max_ud=std::max(max_ud,p.tail_ud);
    }
    assert(min_ud<50&&max_ud>120);
    auto quiet=high_tail.ApplyAudioMotion({160,160,20},{false,0,0},50000);
    assert(quiet.head==160&&quiet.tail_lr==160&&quiet.tail_ud==20);
    high_tail.request_active_.store(true);
    high_tail.running_priority_.store(wzh::PetPriority::kExplicit);
    auto body=high_tail.ApplyAudioMotion({160,160,20},{true,0,0},50020);
    assert(body.head==160&&body.tail_lr==160&&body.tail_ud==20);
'''
        if board=='wzh_crawlPanda':
            extra=r'''
    for(bool both:{false,true}){
        PandaController c;clock_ms=10000;
        c.StartClip(C_COUNT+(both?BODY_CRAWL_BOTH:BODY_CRAWL),clock_ms);
        do{c.UpdatePlayer(clock_ms);c.OutputPose(c.pose_);clock_ms+=20;}while(c.clip_active_);
        bool left=false,right=false,together=false;
        int head_min=90,head_max=90;
        for(auto p:c.servo_output_.samples){
            left|=p[1]<=10&&p[2]<=92;right|=p[1]>=88&&p[2]>=170;
            together|=p[1]<=10&&p[2]>=170;
            head_min=std::min(head_min,p[0]);head_max=std::max(head_max,p[0]);
        }
        assert(both?together:left&&right);
        assert(head_min>=30&&head_min<=40&&head_max>=140&&head_max<=150);
        assert(std::abs(c.last_output_pose_.head-90)<2);
        const uint32_t settle_ms=wzh::SettlingDuration(both?0:kCrawlStrokeDeg,100.f);
        assert(!c.clip_active_&&clock_ms==15220+((settle_ms+19)/20)*20); // Preserve reference gait; slow only its final settling.
    }
'''
        return program.replace('EXTRA',extra)

    def test_crawl_all_clips_reach_visible_output_and_finite_gaits(self):
        self.run_cpp(self.panda_program('wzh_crawlPanda'))

    def test_tail_all_clips_reach_visible_output_with_default_tail_150(self):
        self.run_cpp(self.panda_program('wzh_tailRedPanda'))

    def test_final_crawl_output_matches_reference_period_and_waveform_even_with_voice(self):
        program = self.panda_program('wzh_crawlPanda')
        # Exercise the production player AND final limiter, including a request
        # that interrupts an opposite extreme and a 32-bit millisecond wrap.
        extra = r'''
    for(bool both:{false,true})for(bool interrupted:{false,true})for(bool wrapped:{false,true}){
        const uint32_t started=wrapped?UINT32_MAX-200:10000;
        PandaController c;clock_ms=started;
        if(interrupted){c.pose_={30,172,8};c.last_output_pose_=c.pose_;}
        c.StartClip(C_COUNT+(both?BODY_CRAWL_BOTH:BODY_CRAWL),clock_ms);
        const uint32_t entry=interrupted&&!both?780:400;
        const uint32_t period=both?1200:800;
        const uint32_t cycles=both?4:6;
        std::array<float,3> previous=Values(c.last_output_pose_);
        for(uint32_t elapsed=0;elapsed<=entry+cycles*period+((wzh::SettlingDuration(both?0:kCrawlStrokeDeg,100.f)+19)/20)*20;elapsed+=20){
            clock_ms=started+elapsed;
            c.UpdatePlayer(clock_ms);
            // Simulate a large concurrent voice accent. Walking must retain
            // the reference head center and opposite-direction arm strokes.
            c.OutputPose(c.clip_active_?Pose{8,172,8}:c.pose_);
            auto output=Values(c.last_output_pose_);
            for(unsigned axis=0;axis<3;++axis)
                assert(std::abs(output[axis]-previous[axis])<=8.001f);
            previous=output;
            if(elapsed>=entry&&elapsed<entry+cycles*period){
                const double phase=double((elapsed-entry)%period)/period;
                const double stroke=.5-.5*std::cos(6.283185307179586*phase);
                const std::array<double,3> expected={90,90-82*stroke,
                                                     90+82*(both?stroke:1-stroke)};
                for(unsigned axis=1;axis<3;++axis){
                    if(std::abs(output[axis]-expected[axis])>.02){
                        std::fprintf(stderr,"gait both=%d interrupted=%d t=%u axis=%u actual=%f reference=%f\n",
                                     both,interrupted,elapsed,axis,output[axis],expected[axis]);
                        assert(false);
                    }
                }
            }
        }
        assert(!c.clip_active_);
        for(float angle:Values(c.last_output_pose_))assert(std::abs(angle-90)<.02);
        // A completed clip cannot restart merely because time passes.
        c.UpdatePlayer(clock_ms+3600000);assert(!c.clip_active_);
    }
    // The interaction variant is the same fast gait, limited to two cycles.
    PandaController short_gait;clock_ms=10000;
    short_gait.StartClip(C_COUNT+BODY_COUNT+4,clock_ms);
    assert(short_gait.clip_->frames==kAck_playful);
    const auto short_end=10000+400+1600+wzh::SettlingDuration(kCrawlStrokeDeg,100.f);
    short_gait.UpdatePlayer(short_end-20);assert(short_gait.clip_active_);
    short_gait.UpdatePlayer(short_end);assert(!short_gait.clip_active_);
'''
        self.run_cpp(program[:program.rfind('}')] + extra + '\n}\n')

    def test_dinosaur_resolved_targets_and_output_for_all_clip_families(self):
        base=ROOT/'main/boards/wzh_dinosaur'
        source=(base/'dinosaur_controller.cc').read_text()
        header=(base/'dinosaur_controller.h').read_text()
        types=function(header,'namespace wzh_dino')
        tables=source[source.index('using wzh_dino::Keyframe;'):source.index('bool DinosaurController::Initialize(')]
        tables=tables.rsplit('}  // namespace',1)[0]
        for name in ("dinosaur_reference_motion.h", "dinosaur_story_motion.h"):
            tables = tables.replace(f'#include "{name}"', f'#include "{base / name}"')
        collections=re.findall(r'constexpr MotionClip (\w+)\[\]',tables)
        self.assertGreaterEqual(len(collections),10)
        program=r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>
#include "wzh_pet_behavior.h"
#include "wzh_body_commands.h"
#include "wzh_voice_motion.h"
#include "wzh_quiet_center.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
constexpr uint32_t kTickMs=20;
constexpr size_t kMaxClipFrames=16;
constexpr int SERVO_NECK_TILT_DEFAULT=70,SERVO_TAIL_UD_MIN=55,SERVO_TAIL_UD_MAX=180;
uint32_t clock_ms=10000;
int64_t esp_timer_get_time(){return int64_t(clock_ms)*1000;}
void vTaskDelay(uint32_t ms){clock_ms+=ms;}
using TickType_t=uint32_t;
TickType_t xTaskGetTickCount(){return clock_ms;}
void vTaskDelayUntil(TickType_t* next,uint32_t dt){*next+=dt;if(clock_ms<*next)clock_ms=*next;}
#define pdMS_TO_TICKS(ms) (ms)
unsigned esp_random(){return 0;}
float frnd(){return .5f;}
int irnd(int count){static uint32_t rng=98765;rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng%count;}
struct DinoMotionData{bool playing=false;float level=0,attack=0;};
struct Audio{DinoMotionData GetMotionData(){return {};}
    DinoMotionData GetTtsMotionData(){return {true,.4f,.3f};}};
struct Output{
    bool locked=false;int releases=0;std::vector<std::array<float,5>> samples;
    void Release(){++releases;}
    unsigned faults(){return locked?1:0;}
    bool WriteDegrees(const float* angles,size_t count){
        assert(count==5);if(locked)return false;
        samples.push_back({angles[0],angles[1],angles[2],angles[3],angles[4]});return true;
    }
};
'''+types+'\n'+re.search(r'constexpr uint32_t kMinAuthoredStoryFrameMs = \d+;',source).group(0)+'\n'+tables+'\n'
        program+=function(header,'enum class DinoAction')+';\n'
        program+=function(source,'float SmoothStep(')+'\n'+function(source,'float EaseValue(')+'\n'
        program+=r'''
struct DinosaurController{
    struct Pose{float neck_tilt,neck_lean,head_turn,tail_ud,tail_lr;};
    wzh::PetBehavior pet_;Output servo_output_;wzh::VoiceMotion voice_motion_;
    std::atomic<wzh::SpeechIntent> speech_intent_{wzh::SpeechIntent::kNeutral};
    std::atomic<bool> speech_tail_{false};Audio* audio_=nullptr;
    wzh::InteractionMotionSelector interaction_motion_;
    bool servo_test_active_=false;float request_gain_=1;
    wzh::QuietCenter<5> quiet_center_;void SettleToCenter(uint32_t);
    std::atomic<bool> request_active_{false};
    std::atomic<wzh::PetPriority> running_priority_{wzh::PetPriority::kEmotion};
    std::atomic<bool> flash_performance_active_{false},speaking_{false},sky_roar_active_{false};
    std::atomic<float> output_audio_level_{0},output_audio_attack_{0};
    std::atomic<int64_t> output_audio_updated_us_{0};
    float audio_motion_level_=0,audio_motion_emphasis_=0,audio_motion_phase_=0,audio_motion_direction_=1;
    bool audio_motion_active_=false;int64_t audio_motion_last_update_us_=0;
    uint32_t last_output_ms_=0,last_pose_log_ms_=0,interrupt_at=UINT32_MAX;
    float output_angles_[5]={70,90,90,90,90};
    Pose current_pose_{70,90,90,90,90},last_output_pose_{70,90,90,90,90};
    bool MotionInterrupted()const{return clock_ms>=interrupt_at;}
    void PollInteraction(uint32_t){}
    void ApplyLiving(Pose&,float,float){}
    void PrepareIdleCenter(){}
    void RunInteraction(wzh::PetInteraction);
    wzh::MotionShuffleBag<16> action_clip_bags_[8];
    int8_t last_sound_clip_indices_[8]={-1,-1,-1,-1,-1,-1,-1,-1};
    const wzh_dino::MotionClip* PickClip(DinoAction);
    const wzh_dino::MotionClip* PickSoundClip(DinoAction);
    void SetPose(const Pose&);
    Pose ResolveKeyframe(const wzh_dino::Keyframe&,const Pose&,bool,float,bool=false) const;
    uint32_t ComfortableDuration(const Pose&,const Pose&,uint32_t)const;
    void PlaySegment(const Pose&,const Pose&,uint32_t,wzh_dino::MotionEase,float,
                     const Pose* =nullptr,const Pose* =nullptr,uint32_t=0,uint32_t=0,bool=false);
    Pose PlayClip(const wzh_dino::MotionClip&,const Pose&,bool,float,float,float);
};
'''
        for marker in ['void DinosaurController::SettleToCenter(', 'void DinosaurController::SetPose(',
                       'DinosaurController::Pose DinosaurController::ResolveKeyframe(',
                       'uint32_t DinosaurController::ComfortableDuration(',
                       'void DinosaurController::PlaySegment(',
                       'DinosaurController::Pose DinosaurController::PlayClip(',
                       'void DinosaurController::RunInteraction(',
                       'const wzh_dino::MotionClip* DinosaurController::PickClip(',
                       'const wzh_dino::MotionClip* DinosaurController::PickSoundClip(']:
            program+=function(source,marker)+'\n'
        program+='\nstd::array<float,5> Values(DinosaurController::Pose p){return {p.neck_tilt,p.neck_lean,p.head_turn,p.tail_ud,p.tail_lr};}\n'
        program+='\nint main(){std::vector<MotionClip> clips;\n'
        for collection in collections:
            program+=f'clips.insert(clips.end(),std::begin({collection}),std::end({collection}));\n'
        program+=r'''
    DinosaurController chooser;
    for(unsigned action=0;action<std::size(kActionClips);++action){
        for(bool short_sound:{false,true}){
            const auto& set=short_sound?kSoundActionClips[action]:kActionClips[action];
            std::vector<bool> seen(set.count,false);const MotionClip* last=nullptr;
            for(int draw=0;draw<300;++draw){
                auto selected=short_sound?chooser.PickSoundClip(static_cast<DinoAction>(action))
                                          :chooser.PickClip(static_cast<DinoAction>(action));
                assert(selected>=set.clips&&selected<set.clips+set.count&&selected!=last);
                seen[selected-set.clips]=true;last=selected;
            }
            for(bool selected:seen)assert(selected);
        }
    }
    for(unsigned event=0;event<wzh::InteractionMotionSelector::kEventCount;++event){
        std::vector<std::vector<std::array<float,5>>> stories;
        for(unsigned variant=0;variant<wzh::InteractionMotionSelector::kVariantCount;++variant){
            const auto& clip=kInteractionClips[variant*wzh::InteractionMotionSelector::kEventCount+event];
            DinosaurController resolver;auto pose=resolver.current_pose_;
            std::vector<std::array<float,5>> poses;
            for(unsigned frame=0;frame<clip.count;++frame){
                pose=resolver.ResolveKeyframe(clip.frames[frame],pose,false,1.4f);
                auto values=Values(pose);if(poses.empty()||poses.back()!=values)poses.push_back(values);
            }
            for(const auto& previous:stories)assert(previous!=poses);
            stories.push_back(poses);
        }
    }
    for(auto& clip:clips){
        for(bool mirror:{false,true}){
            DinosaurController c;clock_ms=10000;
            auto minimum=Values(c.current_pose_),maximum=minimum;
            auto pose=c.current_pose_;
            for(uint8_t frame=0;frame<clip.count;++frame){
                pose=c.ResolveKeyframe(clip.frames[frame],pose,mirror,1.4f,clip.authored_dynamics);
                auto values=Values(pose);
                for(int axis=0;axis<5;++axis){
                    assert(values[axis]>=(axis==3?55:0)&&values[axis]<=180);
                    minimum[axis]=std::min(minimum[axis],values[axis]);
                    maximum[axis]=std::max(maximum[axis],values[axis]);
                }
            }
            c.PlayClip(clip,c.current_pose_,mirror,1.4f,1.0f,0);
            assert(clock_ms<68000); // Finite, below the 60-second servo power budget.
            auto actual_min=Values(DinosaurController::Pose{70,90,90,90,90}),actual_max=actual_min;
            auto previous=actual_min;
            for(auto values:c.servo_output_.samples){
                for(int axis=0;axis<5;++axis){
                    assert(values[axis]>=(axis==3?55:0)&&values[axis]<=180);
                    assert(std::abs(values[axis]-previous[axis])<=wzh::kPetServoSpeed*.02f+.001f); // Command speed bound, float output angles.
                    actual_min[axis]=std::min(actual_min[axis],values[axis]);
                    actual_max[axis]=std::max(actual_max[axis],values[axis]);
                }
                previous=values;
            }
            if(!mirror){
                if(std::strcmp(clip.name,"body-shake-head")==0){
                    assert(actual_min[2]<=5&&actual_max[2]>=175);
                    assert(clock_ms-10000<=12000);
                }
                if(std::strcmp(clip.name,"body-look-left")==0)assert(actual_max[2]>=175);
                if(std::strcmp(clip.name,"body-look-right")==0)assert(actual_min[2]<=5);
            }
            for(int axis=0;axis<5;++axis){
                if(actual_min[axis]>minimum[axis]+4||actual_max[axis]<maximum[axis]-4){
                    std::fprintf(stderr,"%s mirror=%d axis=%d expected=%.1f..%.1f actual=%.1f..%.1f\n",
                        clip.name,mirror,axis,minimum[axis],maximum[axis],actual_min[axis],actual_max[axis]);
                    assert(false);
                }
            }
        }
    }
    for(int mode=0;mode<3;++mode){
        for(unsigned index=0;index<wzh::InteractionMotionSelector::kClipCount;++index){
            DinosaurController pet;clock_ms=10000;
            if(mode==1)pet.pet_.Gentle(clock_ms,120000);
            if(mode==2)pet.pet_.Control(wzh::PetControl::kQuiet,clock_ms);
            const unsigned base=wzh::InteractionMotionSelector::kEventCount;
            auto event=static_cast<wzh::PetInteraction>(index%base+1);
            for(unsigned variant=0;variant<index/base;++variant)pet.interaction_motion_.Select(event);
            pet.RunInteraction(event);
            for(unsigned t=0;t<6000;t+=20,clock_ms+=20)pet.SettleToCenter(clock_ms);
            assert(pet.servo_output_.releases>0&&clock_ms<30000);
            assert(!pet.servo_output_.samples.empty());
            auto end=pet.servo_output_.samples.back();
            auto rest=Values(DinosaurController::Pose{70,90,90,90,90});
            float onset=0;
            for(size_t i=0;i<std::min<size_t>(31,pet.servo_output_.samples.size());++i)
                for(int axis=0;axis<5;++axis)
                    onset=std::max(onset,std::abs(pet.servo_output_.samples[i][axis]-rest[axis]));
            assert(onset>=(mode==0?5:3));
            for(int axis=0;axis<5;++axis)assert(std::abs(end[axis]-rest[axis])<=3);
        }
    }
    DinosaurController settle;clock_ms=100000;
    settle.pet_.Control(wzh::PetControl::kQuiet,clock_ms);
    settle.last_output_pose_={180,0,180,180,0};
    const auto initial=Values(settle.last_output_pose_);
    for(unsigned a=0;a<5;++a)settle.output_angles_[a]=initial[a];
    auto previous_center=initial;
    for(unsigned time=0;time<6000;time+=20,clock_ms+=20){
        settle.SettleToCenter(clock_ms);auto actual=Values(settle.last_output_pose_);
        for(unsigned a=0;a<5;++a)assert(std::abs(actual[a]-previous_center[a])<=2.001f);
        previous_center=actual;
    }
    assert((previous_center==std::array<float,5>{70,90,90,90,90}));
    assert(settle.servo_output_.releases>0);
    DinosaurController c;
    auto rest=c.ResolveKeyframe({70,90,90,90,90,800,MotionEase::kSmooth},c.current_pose_,false,1.5f);
    assert(rest.neck_tilt==70); // Amplification must preserve this board's real center.
    auto up=c.ResolveKeyframe({0,90,90,90,90,800,MotionEase::kSmooth},rest,false,1.5f);
    auto down=c.ResolveKeyframe({180,90,90,90,90,800,MotionEase::kSmooth},rest,false,1.5f);
    assert(up.neck_tilt==0&&down.neck_tilt==180);
    // A broad new long-neck beat must retain the board's graceful travel
    // budget even when the imported source asks for a very short frame.
    const Keyframe broad[]={{180,90,90,90,90,100,MotionEase::kSmooth}};
    const MotionClip long_neck={"broad-neck-budget",broad,1,true};
    DinosaurController graceful;clock_ms=10000;
    graceful.PlayClip(long_neck,graceful.current_pose_,false,1.4f,1,0);
    assert(clock_ms>=10820&&clock_ms<11360); // 133 deg/s active beat, still finite and synchronized.
    assert(graceful.last_output_pose_.neck_tilt>=179);
    clock_ms=10000;c.interrupt_at=10500;
    c.PlayClip(kBodyClips[0],c.current_pose_,false,1.4f,1,0);
    assert(clock_ms==10500);
    auto count=c.servo_output_.samples.size();c.servo_output_.locked=true;
    c.SetPose({0,0,0,55,0});assert(c.servo_output_.samples.size()==count);
    // Real production audio motion: all five axes must visibly respond, with
    // several direction changes rather than a single slow drift over 15 sec.
    DinosaurController talking;Audio audio;talking.audio_=&audio;talking.speaking_=true;
    std::array<float,5> movement{};int head_turns=0;float previous_delta=0,previous_head=90;
    for(unsigned sample=0;sample<750;++sample){
        clock_ms=10000+sample*20;talking.output_audio_updated_us_=int64_t(clock_ms)*1000;
        talking.output_audio_level_=.4f;talking.output_audio_attack_=.3f;
        talking.SetPose({70,90,90,90,90});
        auto actual=Values(talking.last_output_pose_);
        auto rest=Values(DinosaurController::Pose{70,90,90,90,90});
        for(unsigned axis=0;axis<5;++axis)movement[axis]=std::max(movement[axis],std::abs(actual[axis]-rest[axis]));
        float delta=actual[2]-previous_head;
        if(delta*previous_delta<0)++head_turns;
        if(std::abs(delta)>.001f)previous_delta=delta;
        previous_head=actual[2];
    }
    for(float range:movement)assert(range>=15);
    assert(head_turns>=6);
}
'''
        self.run_cpp(program)

    def test_crawl_commands_are_explicit_directional_and_negation_aware(self):
        base=ROOT/'main/boards/wzh_crawlPanda'
        source=(base/'panda_controller.cc').read_text()
        header=(base/'panda_controller.h').read_text()
        enums=function(header,'enum class PandaBodyAction')+';\n'
        program=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include "wzh_pet_behavior.h"
#include "wzh_body_commands.h"
#include "wzh_voice_motion.h"
#include "wzh_quiet_center.h"
#define ESP_LOGI(...) ((void)0)
'''+enums+r'''
struct PandaController{
    std::atomic<bool> conversation_active_{true};
    PandaBodyAction last=PandaBodyAction::kCount;
    bool stopped=false;
    bool HandlePetControl(wzh::PetControl control){
        stopped=control==wzh::PetControl::kStop;
        return control!=wzh::PetControl::kNone;
    }
    void TriggerBodyAction(PandaBodyAction body){last=body;}
    void React(wzh::PetReaction,wzh::PetPriority){}
    static bool ParseBodyAction(const std::string&,PandaBodyAction&);
    static const char* BodyActionName(PandaBodyAction);
    void HandleUserSpeechEmotion(const char*);
};
'''
        for marker in ['bool PandaController::ParseBodyAction(',
                       'const char* PandaController::BodyActionName(',
                       'void PandaController::HandleUserSpeechEmotion(']:
            program+=function(source,marker)+'\n'
        program+=r'''
int main(){
    PandaController c;
    for(auto text:{"爬一爬","向前爬","爬过来","开始爬行"}){
        c.HandleUserSpeechEmotion(text);assert(c.last==PandaBodyAction::kCount);
    }
    for(auto text:{"双手一起爬","同步爬行","双臂爬"}){
        c.HandleUserSpeechEmotion(text);assert(c.last==PandaBodyAction::kCount);
    }
    c.last=PandaBodyAction::kCount;c.HandleUserSpeechEmotion("不要爬行");
    assert(c.stopped&&c.last==PandaBodyAction::kCount);
    c.conversation_active_=false;c.HandleUserSpeechEmotion("爬一爬");
    assert(c.last==PandaBodyAction::kCount); // Offline recognition is dispatched through the board-local token route.
    PandaBodyAction body;
    assert(c.ParseBodyAction("crawl",body)&&body==PandaBodyAction::kCrawl);
    assert(c.ParseBodyAction("crawl_both",body)&&body==PandaBodyAction::kCrawlBoth);
    assert(std::string(c.BodyActionName(PandaBodyAction::kCrawl))=="crawl");
    assert(std::string(c.BodyActionName(PandaBodyAction::kCrawlBoth))=="crawl_both");
    assert(!c.ParseBodyAction("crawl_forever",body));
}
'''
        self.run_cpp(program)
