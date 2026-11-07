"""Product-level motion requirements against the real three-axis player."""
import unittest
from pathlib import Path

import test_wzh_motion_output as output

ROOT = Path(__file__).resolve().parents[2]


class WzhCrawlPersonalityTest(unittest.TestCase):
    def run_player(self, body):
        case = output.WzhMotionOutputTest()
        program = case.panda_program('wzh_crawlPanda')
        case.run_cpp(program[:program.index('int main(){')] + '\nint main(){\n' + body + '\n}\n')

    def test_every_story_moves_head_and_both_paws_with_prompt_visible_feedback(self):
        self.run_player(r'''
    for(int id=0;id<C_COUNT+BODY_COUNT+int(std::size(kInteractionClips));++id){
        // Explicit single-joint commands have separate isolation/pose tests.
        if(id>=C_COUNT&&id<C_COUNT+BODY_COUNT)continue;
        const bool head_only=false;
        const bool interaction=id>=C_COUNT+BODY_COUNT;
        for(bool held:{false,true}){
            if(held&&!interaction)continue;
            clock_ms=10000;PandaController c;
            if(held)c.pet_.Gentle(clock_ms,120000);
            c.StartClip(id,clock_ms);
            const bool reference=std::strncmp(c.clip_->name,"ref_",4)==0;
            std::array<float,3> target_low={90,90,90},target_high=target_low;
            for(unsigned frame=0;frame<c.clip_->count;++frame){
                auto target=Values(TargetPose(*c.clip_,frame,false));
                for(unsigned axis=0;axis<3;++axis){
                    target_low[axis]=std::min(target_low[axis],target[axis]);
                    target_high[axis]=std::max(target_high[axis],target[axis]);
                }
            }
            std::array<float,3> low={90,90,90},high=low;
            float onset=0;unsigned elapsed=0;
            do{
                c.UpdatePlayer(clock_ms);c.OutputPose(c.pose_);
                auto actual=Values(c.last_output_pose_);
                for(unsigned axis=0;axis<3;++axis){
                    low[axis]=std::min(low[axis],actual[axis]);
                    high[axis]=std::max(high[axis],actual[axis]);
                    if(elapsed<=(reference?600:240))onset=std::max(onset,std::abs(actual[axis]-90));
                }
                elapsed+=20;clock_ms+=20;
                assert(elapsed<8000);
            }while(c.clip_active_);
            if(interaction)assert(elapsed<=5000);
            if(onset<(reference?2:(held?6:10))){
                std::fprintf(stderr,"%s held=%d onset=%.1f within 240ms\n",c.clip_->name,held,onset);
                assert(false);
            }
            for(unsigned axis=0;axis<3;++axis){
                // Imported listening/settling has intentionally small paw beats.
                // Require the real player to realize its authored geometry; new
                // expressive stories retain the product's broad travel floor.
                const float required=reference?(target_high[axis]-target_low[axis])*.9f:
                    (head_only?0:(axis==0?std::min(held?30.f:50.f,(target_high[axis]-target_low[axis])*.9f):(held?24.f:35.f)));
                if(high[axis]-low[axis]<required){
                    std::fprintf(stderr,"%s held=%d axis=%u visible_range=%.1f required=%.1f\n",
                                 c.clip_->name,held,axis,high[axis]-low[axis],required);
                    assert(false);
                }
                assert(std::abs(Values(c.last_output_pose_)[axis]-90)<3);
            }
        }
    }
    // Identical simultaneous targets must produce a head lead and paw follow,
    // while still arriving at the intended endpoint and returning to rest.
    clock_ms=10000;PandaController staged;staged.StartClip(0,clock_ms);
    const Keyframe frames[]={{1000,110,70,110},{1000,90,90,90}};
    const Clip clip={"lead_and_follow",frames,2,false};
    staged.clip_=&clip;staged.tempo_=1;staged.mirror_=false;
    staged.frame_duration_ms_=staged.FrameDuration(0);
    for(unsigned t=0;t<=160;t+=20){clock_ms=10000+t;staged.UpdatePlayer(clock_ms);staged.OutputPose(staged.pose_);}
    auto pose=staged.last_output_pose_;
    assert(pose.head-90>90-pose.left_arm+8);
    assert(pose.head-90>pose.right_arm-90+8);
''')

    def test_explicit_left_right_paw_commands_preserve_requested_side(self):
        self.run_player(r'''
    for(int body:{BODY_LOOK_LEFT,BODY_LOOK_RIGHT,BODY_WAVE_LEFT,BODY_WAVE_RIGHT}){
        clock_ms=10000;PandaController c;c.mirror_balance_=-2;
        c.StartClip(C_COUNT+body,clock_ms);
        assert(!c.clip_->mirrorable&&!c.mirror_);
        if(body==BODY_WAVE_LEFT)assert(TargetPose(*c.clip_,0,true).left_arm<=10);
        if(body==BODY_WAVE_RIGHT)assert(TargetPose(*c.clip_,0,true).right_arm>=170);
    }
''')

    def test_phase_continuity_and_instance_voice_follow_with_stale_state_reset(self):
        header = ROOT/'main/boards/wzh_crawlPanda/panda_motion_style.h'
        output.WzhMotionOutputTest().run_cpp('#include "'+str(header)+'"\n'+r'''
#include <cassert>
#include <array>
using namespace wzh_crawl;
int main(){
    for(unsigned beat=0;beat<8;++beat)for(unsigned axis=0;axis<3;++axis)for(bool mirror:{false,true}){
        float previous=0;
        for(unsigned sample=0;sample<=1000;++sample){
            float t=sample/1000.f;float value=GestureProgress(t,axis,beat,mirror);
            assert(value>=previous&&value>=0&&value<=1);previous=value;
            if(axis==0)assert(value>=t);
            if(axis!=0)assert(value<=t);
        }
        const float eps=.0001f;
        assert(std::abs(GestureProgress(eps,axis,beat,mirror)/eps-1)<.01);
        assert(std::abs((1-GestureProgress(1-eps,axis,beat,mirror))/eps-1)<.01);
        if(axis!=0)assert(GestureProgress(.4f,axis,beat,true)==GestureProgress(.4f,3-axis,beat,false));
    }
    VoiceGesture voice;
    for(unsigned t=0;t<1000;t+=20){auto p=voice.Update(true,0,0,t);assert(p.head==0&&p.left==0&&p.right==0);}
    voice.Update(false,0,0,1000);
    std::array<float,3> low{},high{};
    for(unsigned t=0;t<15000;t+=20){
        auto p=voice.Update(true,.4f,t%600<60?.6f:0,2000+t);
        const std::array<float,3> values={p.head,p.left,p.right};
        for(unsigned axis=0;axis<3;++axis){
            assert(std::isfinite(values[axis])&&std::abs(values[axis])<90);
            low[axis]=std::min(low[axis],values[axis]);high[axis]=std::max(high[axis],values[axis]);
        }
    }
    assert(high[0]-low[0]>60&&high[1]-low[1]>30&&high[2]-low[2]>30);
    // Maintenance/stop gaps may bypass the voice updater. No old accent survives.
    auto fresh=voice.Update(true,.4f,.6f,20000);
    assert(std::abs(fresh.head)<3&&std::abs(fresh.left)<3&&std::abs(fresh.right)<3);
    auto end=voice.Update(false,0,0,20020);assert(end.head==0&&end.left==0&&end.right==0);
    VoiceGesture independent;auto still=independent.Update(true,0,0,UINT32_MAX-100);
    assert(still.head==0&&still.left==0&&still.right==0);
    for(uint32_t t=UINT32_MAX-80;t!=119;t+=20){
        auto p=independent.Update(true,.4f,.2f,t);assert(std::isfinite(p.head));
    }
}
''')


if __name__ == '__main__':
    unittest.main()
