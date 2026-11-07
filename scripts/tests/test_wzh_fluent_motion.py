"""Numerical continuity and motor-vibration replay for the production helpers."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WzhFluentMotionTest(unittest.TestCase):
    def test_every_interaction_draws_all_variants_without_adjacent_repeats(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_interaction.h"
using namespace wzh;
int main(){
    InteractionMotionSelector picker;
    uint32_t random=0x82341abc;
    int last[InteractionMotionSelector::kEventCount];
    for(auto& value:last)value=-1;
    for(int cycle=0;cycle<500;++cycle){
        unsigned seen[InteractionMotionSelector::kEventCount]={};
        for(unsigned draw=0;draw<InteractionMotionSelector::kVariantCount;++draw){
            for(unsigned event=0;event<InteractionMotionSelector::kEventCount;++event){
                random^=random<<13;random^=random>>17;random^=random<<5;
                int selected=picker.Select(static_cast<PetInteraction>(event+1),random);
                assert(selected>=0&&unsigned(selected)<InteractionMotionSelector::kClipCount);
                assert(unsigned(selected)%InteractionMotionSelector::kEventCount==event);
                assert(selected!=last[event]);last[event]=selected;
                unsigned bit=1U<<(unsigned(selected)/InteractionMotionSelector::kEventCount);
                assert(!(seen[event]&bit));seen[event]|=bit;
            }
        }
        for(auto mask:seen)assert(mask==(1U<<InteractionMotionSelector::kVariantCount)-1U);
        assert(picker.Select(PetInteraction::kNone,random)==-1);
        assert(picker.Select(static_cast<PetInteraction>(255),random)==-1);
    }
}
''')

    def run_cpp(self, program):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)/'profile.cc'
            source.write_text(program)
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(ROOT/'main/boards/common/wzh'), str(source),
                                     '-o', str(source.with_suffix(''))], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(source.with_suffix(''))], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_adjacent_unequal_frames_share_velocity_and_do_not_overshoot(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include "wzh_motion_profile.h"
using namespace wzh;
int main(){
    // Physical velocities across an unequal-duration joint must agree and must
    // not stop at a waypoint that continues in the same direction.
    for(float direction:{-1.f,1.f}){
        auto left=[direction](float t){return MotionHermite(70*direction,90*direction,
            120*direction,145*direction,800,900,400,t);};
        auto right=[direction](float t){return MotionHermite(90*direction,120*direction,
            145*direction,165*direction,900,400,1500,t);};
        float incoming=(left(1)-left(1-1.f/900))*1000;
        float outgoing=(right(1.f/400)-right(0))*1000;
        assert(std::abs(incoming)>20&&std::abs(outgoing-incoming)<1);
    }
    for(float before:{-100.f,0.f,20.f,150.f})
        for(float after:{-100.f,0.f,20.f,150.f})
            for(unsigned dt:{220U,750U,2200U}){
                float previous=10;
                for(int sample=0;sample<=1000;++sample){
                    float value=MotionHermite(before,10,60,after,400,dt,1100,sample/1000.f);
                    assert(value>=10&&value<=60&&value>=previous-.0001f);
                    previous=value;
                }
            }
    auto reversal=[](float t){return MotionHermite(10,60,10,0,600,800,600,t);};
    assert(std::abs(reversal(.001f)-60)<.001f); // Turnaround begins from rest.
    assert(MotionFrameDuration(164,180)<=850); // A full head sweep must finish promptly.
    assert(MotionFrameDuration(0,420)<=120); // A landmark must not become a long stop.
    for(float center:{70.f,90.f,150.f}){
        assert(ActionAngle(center,center,0,180,1.85f)==center);
        assert(ActionAngle(center-2,center,0,180,1.85f)==center-2);
    }
    assert(ActionAngle(110,90,8,172,1.85f)>=135);
    assert(ActionAngle(70,90,8,172,1.85f)<=45);
    assert(ActionAngle(180,90,8,172,1.85f)==172);
    assert(ActionAngle(130,150,10,178,1.85f)<=105);
}
''')

    def test_broad_story_geometry_retains_waypoints_and_repeatable_phrasing(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include "wzh_motion_profile.h"
using namespace wzh;
int main(){
    for(float center:{70.f,90.f,150.f}){
        float previous=-1;
        for(int angle=0;angle<=180;++angle){
            float value=StoryAngle(angle,center,0,180,1.35f);
            assert(value>=0&&value<=180&&value>=previous);
            if(angle>0&&angle<180)assert(value>previous); // No flattened interior poses.
            if(angle!=center)assert(std::abs(value-center)>=std::abs(angle-center)-.0001f);
            previous=value;
        }
        assert(StoryAngle(center,center,0,180)==center);
        assert(StoryAngle(0,center,0,180)==0);
        assert(StoryAngle(180,center,0,180)==180);
    }
    assert(StoryAngle(85,150,10,178)<85); // Tail lift stays broad around the real sitting pose.
    assert(StoryAngle(0,90,55,180)==55);
    bool varies=false;
    for(uint32_t seed=0;seed<100;++seed){
        for(uint8_t beat=0;beat<16;++beat){
            uint32_t duration=StoryFrameDuration(120,900,seed,beat);
            assert(duration>=600&&duration<=1000); // Includes the common peak-speed budget.
            assert(duration==StoryFrameDuration(120,900,seed,beat));
            varies|=duration!=StoryFrameDuration(120,900,seed+1,beat);
            uint32_t hold=StoryFrameDuration(0,450,seed,beat);
            assert(hold>=420&&hold<=480); // Authored observation remains visible.
        }
    }
    assert(varies);
}
''')

    def test_full_action_shuffle_bag_exhausts_each_family_and_avoids_boundary_repeats(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_motion_profile.h"
using namespace wzh;
int main(){
    for(unsigned count=1;count<=16;++count){
        MotionShuffleBag<16> bag;int previous=-1;
        for(uint32_t cycle=0;cycle<100;++cycle){
            unsigned seen=0;
            for(unsigned draw=0;draw<count;++draw){
                int clip=bag.Draw(count,cycle*771+draw);
                assert(clip>=0&&unsigned(clip)<count);
                assert(!(seen&(1U<<clip)));seen|=1U<<clip;
                if(count>1)assert(clip!=previous);
                previous=clip;
            }
            assert(seen==((1U<<count)-1));
        }
        assert(bag.Draw(0,0)==-1&&bag.Draw(17,0)==-1);
        assert(bag.Draw(3,0)>=0); // Changing the family size resets the pool.
    }
}
''')

    def test_motor_vibration_never_queues_a_delayed_gesture_after_release(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_posture_input.h"
using namespace wzh;
int main(){
    PostureInput input;uint32_t now=UINT32_MAX-3000;
    for(int i=0;i<60;++i)assert(input.Update(-128,1010,20,false,false,now+=20)==PetInteraction::kNone);
    // Includes the user's approximately 1g samples plus deliberately stronger
    // shake/flip-like motor vibration. None may survive as a pending pickup.
    for(int i=0;i<400;++i){
        int x=i%2?1500:-1500;
        int y=i%3?870:-1000;
        assert(input.Update(x,y,52,true,false,now+=20)==PetInteraction::kNone);
    }
    for(int i=0;i<29;++i)
        assert(input.Update(i%2?900:-900,998,12,false,false,now+=20)==PetInteraction::kNone);
    for(int i=0;i<150;++i)
        assert(input.Update(-140,998,12,false,false,now+=20)==PetInteraction::kNone);
    // Real user shaking after the settling window is still immediately usable.
    input.Update(1500,0,1000,false,false,now+=20);
    assert(input.Update(-1500,0,1000,false,false,now+=20)==PetInteraction::kShaken);
    input.Accepted(now);
    for(int i=0;i<10;++i)
        assert(input.Update(0,0,0,true,true,now+=20)==PetInteraction::kNone);
}
''')
