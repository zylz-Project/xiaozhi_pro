"""Replay tail-led offline patterns and actual library/recording dispatch."""
from pathlib import Path
import unittest

import test_wzh_fluent_motion as fluent
import test_wzh_motion_output as output
from test_wzh_action_queue import queue_function as function

ROOT = Path(__file__).resolve().parents[2]


class WzhOfflinePersonalityTest(unittest.TestCase):
    def test_tail_real_dispatch_keeps_long_flash_sounds_on_the_continuous_score(self):
        source = (ROOT/'main/boards/wzh_tailRedPanda/panda_controller.cc').read_text()
        program = r'''
#include <atomic>
#include <cassert>
#include "wzh_sound_choreography.h"
unsigned esp_random(){return 123;}
struct Pose {float head=90,tail_lr=90,tail_ud=150;};
struct Audio {wzh::SoundPlayback sound;auto GetMotionData(){return sound;}};
struct Motion {
    int updates=0;void Cancel(uint32_t){}bool NeedsBegin(const wzh::SoundPlayback&){return false;}
    bool Update(const wzh::SoundPlayback&,wzh::PetMechanism,const wzh::SoundPose&,uint32_t,
                wzh::SoundPose& out,bool& powered,uint32_t){++updates;out={90,30,20,0,0};powered=true;return true;}
};
struct Center {void Reset(){}};struct Output {void Release(){}};
struct PandaController {
    Audio* audio_;std::atomic<bool> conversation_active_{false},request_active_{false};
    bool clip_active_=false;Motion sound_motion_;Center quiet_center_;Output servo_output_;
    Pose last_output_pose_,pose_;int starts=0,cancels=0;
    void CancelSoundMotion(){++cancels;}int ClassifySound(int){return 0;}int ChooseClip(int){return 0;}
    void StartClip(int,uint32_t){++starts;clip_active_=true;}
    void OutputPose(Pose p){last_output_pose_=p;}
    bool UpdateSoundMotion(uint32_t);
};
'''+function(source, 'bool PandaController::UpdateSoundMotion(')+r'''
int main(){
    Audio audio;audio.sound.playing=true;audio.sound.audible=true;audio.sound.sound_index=0;
    audio.sound.kind=wzh::SoundKind::kCall;audio.sound.duration_ms=1600;
    PandaController c;c.audio_=&audio;
    assert(c.UpdateSoundMotion(1000)&&c.starts==0&&c.sound_motion_.updates==1);
    audio.sound.duration_ms=24000;
    assert(c.UpdateSoundMotion(1020)&&!c.clip_active_&&c.starts==0&&c.sound_motion_.updates==2);
    assert(c.last_output_pose_.tail_lr==30&&c.last_output_pose_.tail_ud==20);
    const wzh::UserPair pair{};wzh::RegisterUserPairs(&pair,1);
    audio.sound.duration_ms=1600;audio.sound.sound_index=wzh::kUserPairIndexBase;
    assert(c.UpdateSoundMotion(1040)&&c.starts==0&&c.sound_motion_.updates==3);
    c.conversation_active_=true;
    assert(!c.UpdateSoundMotion(1060)&&c.cancels==1&&c.sound_motion_.updates==3);
}
'''
        fluent.WzhFluentMotionTest.run_cpp(self, program)

    def test_crawl_gaits_are_in_random_play_and_footsteps_choose_actual_gaits(self):
        base = ROOT/'main/boards/wzh_crawlPanda'
        source = (base/'panda_controller.cc').read_text()
        program = output.WzhMotionOutputTest().panda_program('wzh_crawlPanda')
        program = program[:program.index('int main(){')]
        program = '#include "wzh_sound_score.h"\n'+program
        program = program.replace('int irnd(int){return 0;}',
                                  'int irnd(int n){static unsigned i=0;return i++%n;}')
        program = program.replace('void StartClip(int,uint32_t);',r'''
    Behaviour ClassifySound(int)const{return Behaviour::kCurious;}
    int ChooseClip(Behaviour){return C_CURIOUS_PEEK;}
    int ChooseOfflineSoundClip(int,wzh::SoundKind,uint32_t);
    void StartClip(int,uint32_t);
''')
        program += function(source, 'int PandaController::ChooseOfflineSoundClip(')+r'''
int main(){
    PandaController c;bool alternate=false,together=false;
    for(int id:kFamily[static_cast<unsigned>(Behaviour::kPlayful)]){
        alternate|=id==C_PLAYFUL_CRAWL;together|=id==C_PLAYFUL_CRAWL_BOTH;
    }
    assert(alternate&&together);alternate=together=false;
    for(unsigned draw=0;draw<20;++draw){
        const int id=c.ChooseOfflineSoundClip(0,wzh::SoundKind::kSteps,6000);
        assert(id==C_PLAYFUL_CRAWL||id==C_PLAYFUL_CRAWL_BOTH);
        alternate|=id==C_PLAYFUL_CRAWL;together|=id==C_PLAYFUL_CRAWL_BOTH;
        clock_ms=10000;c.StartClip(id,clock_ms);assert(IsCrawlClip(*c.clip_));
    }
    assert(alternate&&together);
    assert(c.ChooseOfflineSoundClip(0,wzh::SoundKind::kSleep,20000)==C_CURIOUS_PEEK);
}
'''
        output.WzhMotionOutputTest().run_cpp(program)

    def test_tail_patterns_are_continuous_broad_mirrored_and_keep_wagging_on_long_sounds(self):
        fluent.WzhFluentMotionTest.run_cpp(self, r'''
#include <cassert>
#include <set>
#include <string>
#include "wzh_tail_phrase.h"
int main(){
    for(unsigned duration:{2550,6000,12000,24000}){
        std::set<std::string> paths;
        for(unsigned variant=0;variant<32;++variant){
            auto previous=wzh::TailPhrase(0,variant,1,duration);
            auto low=previous,high=previous;std::string path;int crossings=0;
            for(unsigned t=5;t<=duration;t+=5){
                auto q=wzh::TailPhrase(float(t)/duration,variant,1,duration);
                auto mirror=wzh::TailPhrase(float(t)/duration,variant,-1,duration);
                for(unsigned a=0;a<3;++a){
                    const float lo=a==0?30:a==1?6:0,hi=a==0?150:a==1?174:180;
                    assert(std::isfinite(q[a])&&q[a]>=lo&&q[a]<=hi);
                    assert(std::abs(q[a]-previous[a])*200<400);
                    assert(std::abs(mirror[a]-(a==2?q[a]:180-q[a]))<.001f);
                    low[a]=std::min(low[a],q[a]);high[a]=std::max(high[a],q[a]);
                    if(t%100==0)path+=std::to_string(std::lround(q[a]*10))+",";
                }
                if((previous[1]<90&&q[1]>=90)||(previous[1]>90&&q[1]<=90))++crossings;
                previous=q;
            }
            assert(high[1]-low[1]>125);
            if(duration>=6000){
                // Multiple full sweeps, not one gesture slowed over the sound.
                assert(crossings>=int(duration/3000)*2-1);
                assert(high[1]-low[1]>high[0]-low[0]);
                assert(high[2]-low[2]>35);
            }
            assert(paths.insert(path).second);
        }
        assert(paths.size()==32);
    }
}
''')

    def test_new_tail_stories_are_reachable_and_real_pwm_preserves_their_travel(self):
        program = output.WzhMotionOutputTest().panda_program('wzh_tailRedPanda')
        body = r'''
int main(){
    const char* stories[]={"story_head_first_greeting","story_peek_then_flag",
        "story_tail_compass","story_ribbon_bow","story_curled_listening",
        "story_left_right_invitation","story_food_peek","story_question_answer"};
    for(const char* name:stories){
        int id=-1;
        for(int i=0;i<C_COUNT;++i)if(std::strcmp(kClips[i].name,name)==0)id=i;
        assert(id>=0);bool reachable=false;
        for(const auto& family:kFamily){
            bool ended=false;
            for(int i:family){if(i<0){ended=true;break;}assert(i<C_COUNT);reachable|=i==id;}
            assert(ended);
        }
        assert(reachable);
        clock_ms=10000;PandaController c;c.StartClip(id,clock_ms);
        auto low=Values(c.last_output_pose_),high=low;unsigned elapsed=0;
        do{
            c.UpdatePlayer(clock_ms);c.OutputPose(c.pose_);
            auto p=Values(c.last_output_pose_);
            for(unsigned a=0;a<3;++a){low[a]=std::min(low[a],p[a]);high[a]=std::max(high[a],p[a]);}
            clock_ms+=20;elapsed+=20;assert(elapsed<10000);
        }while(c.clip_active_);
        assert(high[1]-low[1]>110&&high[2]-low[2]>80);
        assert(std::abs(c.last_output_pose_.tail_ud-150)<3);
    }
}
'''
        output.WzhMotionOutputTest().run_cpp(program[:program.index('int main(){')]+body)

    def test_both_pandas_dispatch_sleep_food_and_affection_by_content_before_duration(self):
        for board in ['wzh_crawlPanda', 'wzh_tailRedPanda']:
            with self.subTest(board=board):
                base = ROOT/'main/boards'/board
                source = (base/'panda_controller.cc').read_text()
                types = function((base/'panda_controller.h').read_text(), 'namespace wzh_panda')
                program = r'''
#include <cassert>
#include <cstring>
#include "wzh_sound_choreography.h"
int irnd(int){return 0;}
struct Audio {const char* name="";int duration=20000;
    const char* SoundName(int)const{return name;}int SoundDurationMs(int)const{return duration;}};
'''+types+r'''
using namespace wzh_panda;
struct PandaController {Audio* audio_;Behaviour ClassifySound(int)const;};
'''+function(source, 'bool Contains(')+'\n'+function(source, 'wzh_panda::Behaviour PandaController::ClassifySound(')+r'''
int main(){
    Audio audio;PandaController c{&audio};
    audio.name="睡觉呼吸";assert(c.ClassifySound(0)==Behaviour::kSettle);
    audio.name="熊猫_咀嚼";assert(c.ClassifySound(0)==Behaviour::kEat);
    audio.name="熊猫吃竹子";assert(c.ClassifySound(0)==Behaviour::kEat);
    audio.name="熊猫撒娇声音";assert(c.ClassifySound(0)==Behaviour::kAffection);
    audio.name="挠痒痒笑声";assert(c.ClassifySound(0)==Behaviour::kPlayful);
    audio.name="唤醒_类似伸懒腰状态";assert(c.ClassifySound(0)==Behaviour::kPlayful);
    c.audio_=nullptr;assert(c.ClassifySound(0)==Behaviour::kCurious);
}
'''
                fluent.WzhFluentMotionTest.run_cpp(self, program)


if __name__ == '__main__':
    unittest.main()
