"""Regress audio/motion duration mismatches and abrupt completion commands."""
from pathlib import Path
import unittest

import test_wzh_fluent_motion as motion_tests

ROOT=Path(__file__).resolve().parents[2]


class WzhFluidCompletionTest(unittest.TestCase):
    def test_12_second_footsteps_use_the_authored_path_through_the_last_eight_seconds(self):
        motion_tests.WzhFluentMotionTest.run_cpp(self,
            '#include "'+str(ROOT/'main/boards/wzh_dinosaur/wzh_user_pairs_catalog.inc')+'"\n'+r'''
#include <cassert>
#include <cmath>
#include "wzh_user_pair_motion.h"
using namespace wzh;
int main(){
    RegisterUserPairs(wzh_user_assets::kPairs,std::size(wzh_user_assets::kPairs));
    const auto* pair=UserPairAtIndex(FindUserPair("脚步音效合集 (23)"));
    assert(pair&&pair->audio_ms==11999&&pair->motion_ms==4020);
    UserPairMotion player;const UserPairMotion::Pose rest{70,90,90,90,90};
    player.Begin(pair,rest,rest);
    // The alert walking posture now replaces the recorded ground-feeding tilt;
    // its shorter return is still distance-limited and separate from the body.
    assert(player.body_duration()==11999&&player.duration()>=12899);
    assert(player.Sample(11999)[0]<120);
    assert(player.Sample(0)==rest);
    for(unsigned start=4000;start<10000;start+=2000){
        auto low=player.Sample(start),high=low,previous=low;float travel=0;
        for(unsigned t=start+20;t<start+2000;t+=20){
            const auto pose=player.Sample(t);
            for(unsigned a=0;a<5;++a){
                low[a]=std::min(low[a],pose[a]);high[a]=std::max(high[a],pose[a]);
                travel+=std::abs(pose[a]-previous[a]);
            }
            previous=pose;
        }
        assert(travel>50); // Previously every sample after 4020ms was identical.
    }
    auto previous=player.Sample(player.body_duration());
    for(unsigned t=player.body_duration()+5;t<player.duration();t+=5){
        const auto pose=player.Sample(t);
        for(unsigned a=0;a<5;++a)assert(std::abs(pose[a]-previous[a])*200<100.1f);
        previous=pose;
    }
    assert(player.Sample(player.duration())==rest);
}
''')

    def test_generic_return_is_slow_and_a_missed_completion_tick_yields_from_actual_pose(self):
        motion_tests.WzhFluentMotionTest.run_cpp(self,r'''
#include <cassert>
#include <cmath>
#include "wzh_sound_choreography.h"
#include "wzh_quiet_center.h"
using namespace wzh;
int main(){
    for(auto mechanism:{PetMechanism::kCrawl,PetMechanism::kTail,PetMechanism::kDinosaur}){
        const SoundPose rest=mechanism==PetMechanism::kDinosaur?SoundPose{70,90,90,90,90}:
            mechanism==PetMechanism::kTail?SoundPose{90,90,150,0,0}:SoundPose{90,90,90,0,0};
        const float limit=mechanism==PetMechanism::kDinosaur?100:mechanism==PetMechanism::kTail?130:115;
        for(unsigned variant=1;variant<=16;++variant){
            SoundChoreography player;SoundPlayback sound;
            sound.duration_ms=6000;sound.kind=SoundKind::kSteps;sound.sequence=variant;
            sound.playing=sound.audible=true;
            SoundPose out=rest;bool powered=false;
            assert(player.Update(sound,mechanism,rest,0,out,powered,variant));
            const auto end=player.duration();
            auto previous=player.Sample(end-900,powered);
            for(unsigned t=end-895;t<=end;t+=5){
                const auto pose=player.Sample(t,powered);
                for(unsigned a=0;a<5;++a)assert(std::abs(pose[a]-previous[a])*200<=limit+.2f);
                previous=pose;
            }
            assert(previous==rest);
            SoundPose actual=rest;actual[0]+=40;actual[1]-=35;
            out=actual;sound.elapsed_ms=end+500;
            assert(!player.Update(sound,mechanism,actual,end+500,out,powered,123));
            assert(out==actual&&!powered); // Completion does not issue an old neutral waypoint.
            QuietCenter<5> center;
            assert(center.Sample(actual,rest,end+500,limit)==actual);
        }
    }
}
''')
