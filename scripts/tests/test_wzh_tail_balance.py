"""Measure actual tail-board output, not just maximums in a motion table."""
import unittest
from pathlib import Path
import test_wzh_motion_output as output

ROOT = Path(__file__).resolve().parents[2]


def replay_program(enforce=True):
    program = output.WzhMotionOutputTest().panda_program('wzh_tailRedPanda')
    program = program[:program.index('int main(){')]
    return program + '\n#include <iostream>\n#include "wzh_sound_choreography.h"\n#include "'+str(
        ROOT/'main/boards/wzh_tailRedPanda/wzh_user_pairs_catalog.inc')+'"\n'+r'''
int main(){
    using namespace wzh;
    constexpr bool enforce=__ENFORCE__;
    RegisterUserPairs(wzh_user_assets::kPairs,std::size(wzh_user_assets::kPairs));
    const SoundPose rest{90,90,150,0,0};
    for(unsigned source=0;source<user_pair_count+18;++source){
        const bool paired=source<user_pair_count;
        const auto& score=paired?*user_pairs[source].score:kSoundScores[source-user_pair_count+1];
        SoundChoreography player;
        int previous_side=0;
        for(unsigned v=0;v<32;++v){
            SoundPlayback sound;sound.kind=score.kind;sound.score=&score;sound.duration_ms=score.duration_ms;
            sound.sound_index=paired?kUserPairIndexBase+source:source-user_pair_count+1;
            player.Begin(sound,PetMechanism::kTail,rest,v*7919U);
            clock_ms=10000;PandaController c;
            float low[]={90,90,150},high[]={90,90,150};
            double mean=0;unsigned n=0,up=0,left=0,right=0;
            bool powered=false;
            int first_side=0;
            for(unsigned t=0;t<=player.duration();t+=20,clock_ms+=20){
                auto p=player.Sample(t,powered);
                c.OutputPose({p[0],p[1],p[2]});
                const auto q=Values(c.last_output_pose_);
                for(unsigned a=0;a<3;++a){low[a]=std::min(low[a],q[a]);high[a]=std::max(high[a],q[a]);}
                if(!first_side&&std::abs(q[1]-90)>20)first_side=q[1]>90?1:-1;
                if(t<player.body_duration()){
                    mean+=q[1]-90;++n;up+=q[2]<=30;left+=q[1]<45;right+=q[1]>135;
                }
            }
            const bool lively=score.kind!=SoundKind::kSleep;
            if(enforce&&lively){
                if(!(90-low[0]>=46&&high[0]-90>=46&&90-low[1]>=72&&high[1]-90>=72&&low[2]<=24)){
                    std::cerr<<score.name<<" v="<<v<<" head="<<low[0]<<".."<<high[0]<<" tail="<<low[1]<<".."<<high[1]<<" up="<<low[2]<<"\n";
                    return 1;
                }
                assert(std::abs((90-low[1])-(high[1]-90))<=10);
                assert(std::abs(mean/n)<=5.0);
                assert(up*20>=100);
                assert(std::abs(int(left)-int(right))<=int((left+right)*.10f+4));
                if(previous_side)assert(first_side==-previous_side);
                previous_side=first_side;
            }
            auto finish=player.Sample(player.duration(),powered);assert(finish==rest&&!powered);
            std::cout<<(paired?"paired":"flash")<<"\t"<<score.name<<"\t"<<v<<"\t"<<lively<<"\t"
                     <<90-low[0]<<"\t"<<high[0]-90<<"\t"<<90-low[1]<<"\t"<<high[1]-90<<"\t"
                     <<low[2]<<"\t"<<mean/n<<"\t"<<up*20<<"\t"<<left*20<<"\t"<<right*20<<"\n";
        }
    }
}
'''.replace('__ENFORCE__','true' if enforce else 'false')


class WzhTailBalanceTest(unittest.TestCase):
    def test_all_recordings_at_actual_output_have_both_sides_full_head_and_high_tail(self):
        output.WzhMotionOutputTest().run_cpp(replay_program())

    def test_new_motion_starts_at_actual_pose_has_no_corner_and_returns_at_bounded_speed(self):
        output.WzhMotionOutputTest().run_cpp(r'''
#include <cassert>
#include "wzh_tail_motion.h"
int main(){
    using namespace wzh;
    for(unsigned v=0;v<32;++v)for(TailMotion::Pose start:{TailMotion::Pose{90,90,150,0,0},TailMotion::Pose{30,174,178,0,0},TailMotion::Pose{150,6,10,0,0}}){
        TailMotion motion;motion.Begin(400,start,SoundKind::kCall,v,19);
        auto previous=motion.Sample(0);assert(previous==start);
        for(unsigned t=5;t<=motion.duration();t+=5){
            const auto q=motion.Sample(t);
            for(unsigned a=0;a<3;++a){
                assert(std::abs(q[a]-previous[a])*200<400);
                if(t>motion.body_duration()+5)assert(std::abs(q[a]-previous[a])*200<=130.1f);
            }
            previous=q;
        }
        assert((motion.Sample(motion.duration())==TailMotion::Pose{90,90,150,0,0}));
    }
}
''')


if __name__ == '__main__':
    unittest.main()
