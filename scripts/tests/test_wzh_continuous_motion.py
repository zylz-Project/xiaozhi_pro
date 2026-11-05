"""Continuity, physical PWM scale and delayed scheduling regressions."""
from pathlib import Path
import unittest

from test_wzh_action_queue import queue_function
import test_wzh_motion_output as motion_output

ROOT = Path(__file__).resolve().parents[2]


class WzhContinuousMotionTest(unittest.TestCase):
    run_cpp = motion_output.WzhMotionOutputTest.run_cpp

    def test_quintic_has_shared_velocity_and_zero_knot_acceleration(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include "wzh_motion_profile.h"
int main(){
 for(float direction:{-1.f,1.f})for(unsigned left_ms:{400U,900U,1700U})
  for(unsigned right_ms:{300U,700U,2000U}){
   auto left=[&](float t){return wzh::MotionHermite(-direction,0,direction,2*direction,
                                                 800,left_ms,right_ms,t);};
   auto right=[&](float t){return wzh::MotionHermite(0,direction,2*direction,3*direction,
                                                  left_ms,right_ms,900,t);};
   constexpr float h=.01f;
   float a=(left(1)-left(1-h))*1000/(h*left_ms);
   float b=(right(h)-right(0))*1000/(h*right_ms);
   assert(std::abs(a-b)<.006f&&std::abs(a)>.1f);
   const float end_acc=(left(1)-2*left(1-h)+left(1-2*h))/(h*h);
   const float start_acc=(right(2*h)-2*right(h)+right(0))/(h*h);
   assert(std::abs(end_acc)<.85f&&std::abs(start_acc)<.85f);
  }
 // A true turnaround has zero velocity and acceleration on both sides.
 constexpr float h=.01f;
 auto turn=[](float t){return wzh::MotionHermite(0,1,0,-1,800,600,1200,t);};
 assert(std::abs(turn(h)-1)<.00002f);
 assert(std::abs((turn(2*h)-2*turn(h)+turn(0))/(h*h))<.85f);
}
''')

    def test_extreme_unequal_durations_remain_monotone_and_inside_landmarks(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include "wzh_motion_profile.h"
int main(){
 for(float previous:{-100.f,-1.f,0.f,2.f,100.f})
  for(float next:{-100.f,0.f,1.f,2.f,100.f})
   for(unsigned before:{20U,200U,20000U})for(unsigned after:{20U,600U,20000U})
    for(unsigned duration:{20U,240U,2000U}){
     float last=0;
     for(unsigned i=0;i<=500;++i){
      float q=wzh::MotionHermite(previous,0,1,next,before,duration,after,i/500.f);
      assert(std::isfinite(q)&&q>=0&&q<=1&&q>=last-.000001f);last=q;
     }
     assert(last==1);
    }
}
''')

    def test_short_segments_have_a_jerk_budget_without_slowing_reference_gaits(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include "wzh_motion_profile.h"
int main(){
 for(float travel:{.5f,3.f,10.f,35.f,90.f,164.f}){
  const float seconds=wzh::MotionFrameDuration(travel,100)/1000.f,h=.001f;
  auto p=[&](float t){return wzh::MotionHermite(0,0,travel,travel,100,unsigned(seconds*1000),100,t);};
  const float jerk=(p(3*h)-3*p(2*h)+3*p(h)-p(0))/std::pow(h*seconds,3);
  assert(jerk<=60010&&jerk>=0);
 }
 assert(wzh::MotionFrameDuration(164,180)<=850);
 assert(wzh::MotionFrameDuration(0,420)<=120);
}
''')

    def test_broader_interior_preserves_distinct_waypoints_rest_and_bounds(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include "wzh_motion_profile.h"
int main(){
 for(float center:{70.f,90.f,150.f})for(float gain:{1.f,1.2f,1.38f,1.45f}){
  float last=-1;
  for(unsigned i=0;i<=1800;++i){
   const float angle=i/10.f,delta=angle-center,span=delta<0?center:180-center,t=delta/span;
   const float before=center+span*(t+(gain-1)*t*(1-t*t));
   const float now=wzh::StoryAngle(angle,center,0,180,gain);
   assert(now>=0&&now<=180&&now>last);
   assert(std::abs(now-center)+.0001f>=std::abs(before-center));last=now;
  }
  assert(wzh::StoryAngle(center,center,0,180,gain)==center);
  assert(wzh::StoryAngle(0,center,0,180,gain)==0);
  assert(wzh::StoryAngle(180,center,0,180,gain)==180);
 }
}
''')

    def test_optional_tail_remap_has_no_velocity_step_at_sitting_landmark(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include "wzh_motion_profile.h"
int main(){
 using namespace wzh;
 for(float angle:{0.f,10.f,90.f,150.f,178.f,180.f})assert(TailServoUd(angle)==angle);
 g_tail_ud_remap=true;
 assert(TailServoUd(10)==3&&TailServoUd(150)==90&&TailServoUd(178)==177);
 float last=3;
 for(int i=0;i<=18000;++i){float p=TailServoUd(i/100.f);
  assert(p>=3&&p<=177&&p>=last-.0001f);last=p;}
 constexpr float h=.5f;
 float incoming=(TailServoUd(150)-TailServoUd(150-h))/h;
 float outgoing=(TailServoUd(150+h)-TailServoUd(150))/h;
 assert(std::abs(incoming-outgoing)<.01f);
}
''')

    def test_timer_and_pwm_conversion_use_matching_14_bit_scale_on_all_products(self):
        for board,file in [('wzh_tailRedPanda','panda_controller.cc'),
                           ('wzh_crawlPanda','panda_controller.cc'),
                           ('wzh_dinosaur','dinosaur_controller.cc')]:
            with self.subTest(board=board):
                folder=ROOT/'main/boards'/board
                self.assertRegex((folder/'config.h').read_text(),r'SERVO_FREQ_HZ\s+50\b')
                self.assertRegex((folder/'config.h').read_text(),r'SERVO_DUTY_RES\s+LEDC_TIMER_14_BIT')
                self.assertRegex((folder/file).read_text(),r'servo_output_\.Initialize\([\s\S]*?\(1U << SERVO_DUTY_RES\) - 1\)')

    def test_micro_motion_yields_at_landmarks_and_cannot_move_other_explicit_joints(self):
        for board in ['wzh_tailRedPanda','wzh_crawlPanda']:
            source=(ROOT/'main/boards'/board/'panda_controller.cc').read_text()
            program=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
namespace wzh_panda{struct Pose{float head=90,tail_lr=90,tail_ud=150,left_arm=90,right_arm=90;};}
float frand(float a,float b){return (a+b)/2;}int irand(int a,int){return a;}
struct Clip{const char*name;};
struct PandaController{
 bool clip_active_=true;Clip performance{"curious"};Clip*clip_=&performance;
 uint32_t frame_started_ms_=10000,frame_duration_ms_=800,breath_period_=3200,gaze_next_ms_=0;
 float breath_phase_=0,gaze_target_=0,gaze_pos_=0;
 void AddMicroMotion(wzh_panda::Pose&,uint32_t,float);
};
std::array<float,5>Values(wzh_panda::Pose p){return {p.head,p.tail_lr,p.tail_ud,p.left_arm,p.right_arm};}
'''+queue_function(source,'void PandaController::AddMicroMotion(')+r'''
int main(){
 PandaController c;const auto rest=Values(wzh_panda::Pose{});float motion=0;
 for(unsigned t=0;t<=800;t+=20){
  wzh_panda::Pose p;c.AddMicroMotion(p,10000+t,1);auto q=Values(p);
  for(unsigned a=0;a<q.size();++a){
   if(t==0||t==800)assert(std::abs(q[a]-rest[a])<.0001f);
   motion=std::max(motion,std::abs(q[a]-rest[a]));
  }
 }
 assert(motion>1);
 c.performance.name="body_wave_left";
 for(unsigned t=0;t<=800;t+=20){wzh_panda::Pose p;c.AddMicroMotion(p,10000+t,1);assert(Values(p)==rest);}
}
'''
            with self.subTest(board=board):
                self.run_cpp(program)

    def test_dinosaur_mixed_legacy_eases_keep_velocity_through_real_landmarks(self):
        tester=motion_output.WzhMotionOutputTest()
        captured=[]
        tester.run_cpp=captured.append
        tester.test_dinosaur_resolved_targets_and_output_for_all_clip_families()
        program=captured[0].split('int main(){')[0]
        program=program.replace('std::vector<std::array<float,5>> samples;',
            'std::vector<std::array<float,5>> samples;std::vector<uint32_t>times;')
        program=program.replace('samples.push_back({angles[0]',
            'times.push_back(clock_ms);samples.push_back({angles[0]')
        program+=r'''
int main(){
 const Keyframe frames[]={
  {70,105,90,90,90,733,MotionEase::kSmooth},
  {70,120,90,90,90,411,MotionEase::kFastOut},
  {70,135,90,90,90,577,MotionEase::kLinear},
  {70,150,90,90,90,693,MotionEase::kSmooth},
  {70,90,90,90,90,833,MotionEase::kSmooth}
 };
 const MotionClip clip={"mixed-eases",frames,5,true};
 DinosaurController controller;clock_ms=10000;
 controller.PlayClip(clip,controller.current_pose_,false,1.35f,1,0);
 const auto&samples=controller.servo_output_.samples;const auto&times=controller.servo_output_.times;
 std::vector<size_t>unique;
 for(size_t i=0;i<times.size();++i)if(unique.empty()||times[i]!=times[unique.back()])unique.push_back(i);
 auto from=DinosaurController::Pose{70,90,90,90,90};unsigned checked=0;
 for(unsigned knot=0;knot<3;++knot){
  from=controller.ResolveKeyframe(frames[knot],from,false,1.35f,true);
  bool found=false;
  for(size_t i=1;i+1<unique.size();++i){
   size_t p=unique[i-1],at=unique[i],n=unique[i+1];
   if(std::abs(samples[at][1]-from.neck_lean)>.0001f)continue;
   const float before=(samples[at][1]-samples[p][1])*1000/(times[at]-times[p]);
   const float after=(samples[n][1]-samples[at][1])*1000/(times[n]-times[at]);
   assert(before>5&&after>5);
   assert(std::abs(before-after)<.15f*std::max(before,after));
   found=true;++checked;break;
  }
  assert(found);
 }
 assert(checked==3);
 assert(controller.last_output_pose_.neck_lean==90);
}
'''
        self.run_cpp(program)

    def test_dinosaur_delayed_task_skips_stale_steps_and_preserves_shared_clock(self):
        source=(ROOT/'main/boards/wzh_dinosaur/dinosaur_controller.cc').read_text()
        program=r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>
#include "wzh_motion_profile.h"
namespace wzh_dino{enum class MotionEase{kLinear,kSmooth,kFastOut};}
namespace wzh{enum class PetPriority{kExplicit,kEmotion};}
constexpr uint32_t kTickMs=20;
uint32_t clock_ms=10000;int64_t esp_timer_get_time(){return int64_t(clock_ms)*1000;}
using TickType_t=uint32_t;TickType_t xTaskGetTickCount(){return clock_ms;}
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelayUntil(TickType_t*next,uint32_t dt){*next+=dt;if(clock_ms<*next)clock_ms=*next;}
struct DinosaurController{
 struct Pose{float neck_tilt,neck_lean,head_turn,tail_ud,tail_lr;};
 std::atomic<bool>request_active_{false};std::atomic<wzh::PetPriority>running_priority_{wzh::PetPriority::kEmotion};
 bool servo_test_active_=true,stalled=false;unsigned polls=0;
 uint32_t interrupt_at=UINT32_MAX;std::vector<uint32_t>times;std::vector<Pose>poses;
 void PollInteraction(uint32_t){if(stalled&&++polls==7)clock_ms+=95;}
 bool MotionInterrupted(){return clock_ms>=interrupt_at;}
 void ApplyLiving(Pose&,float,float){}
 void SetPose(Pose pose){times.push_back(clock_ms);poses.push_back(pose);}
 uint32_t ComfortableDuration(const Pose&,const Pose&,uint32_t ms){return ms;}
 void PlaySegment(const Pose&,const Pose&,uint32_t,wzh_dino::MotionEase,float,
  const Pose*previous=nullptr,const Pose*next=nullptr,uint32_t before_ms=0,uint32_t after_ms=0,bool duration_resolved=false);
};
'''+queue_function(source,'float SmoothStep(')+queue_function(source,'float EaseValue(')+queue_function(source,'void DinosaurController::PlaySegment(')+r'''
int main(){
 for(bool late:{false,true}){
  clock_ms=10000;DinosaurController c;c.stalled=late;
  DinosaurController::Pose from{70,90,90,90,90},to{30,140,50,150,30};
  c.PlaySegment(from,to,1000,wzh_dino::MotionEase::kSmooth,0);
  assert(c.times.front()==10000&&c.times.back()>=11000&&c.times.back()<=11020);
  bool saw_gap=false;
  for(size_t i=0;i<c.times.size();++i){
   if(i){assert(c.times[i]-c.times[i-1]>=20);saw_gap|=c.times[i]-c.times[i-1]>20;}
   const float t=std::min(1.f,float(c.times[i]-10000)/1000),q=wzh::SettlingProgress(t);
   assert(std::abs(c.poses[i].neck_tilt-(70-40*q))<.0001f);
   assert(std::abs(c.poses[i].head_turn-(90-40*q))<.0001f);
   assert(std::abs(c.poses[i].tail_ud-(90+60*q))<.0001f);
  }
  assert(saw_gap==late);
 }
 clock_ms=10000;DinosaurController cancelled;cancelled.interrupt_at=10400;
 cancelled.PlaySegment({70,90,90,90,90},{30,140,50,150,30},1000,wzh_dino::MotionEase::kSmooth,0);
 assert(cancelled.times.back()<10400);
}
'''
        self.run_cpp(program)
