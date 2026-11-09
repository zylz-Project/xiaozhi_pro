"""Boot greeting safety, product cues and real authored motion tests."""
import re
import unittest
from pathlib import Path

import test_wzh_user_pairs as pairs_test
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class StartupGreetingTest(unittest.TestCase):
    run_cpp = pairs_test.WzhUserPairsTest.run_cpp
    def test_bounded_deferred_once_and_interruption(self):
        self.run_cpp(r'''
#include "wzh_startup_greeting.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>
using wzh::StartupGreetingGate;
int main(){
 for(uint32_t origin:{0U,UINT32_MAX-1000}){
  StartupGreetingGate gate(origin);
  assert(!gate.Due(origin+1999,true,false));
  assert(!gate.Due(origin+18000,false,false)); // battery, audio, setup, or maintenance busy
  assert(gate.Due(origin+18020,true,false));
  assert(gate.Due(origin+18040,true,false)); // a rejected queue attempt can retry
  gate.Accepted();assert(!gate.Due(origin+18060,true,false));
  StartupGreetingGate expired(origin);
  assert(!expired.Due(origin+30000,true,false));
  assert(!expired.Due(origin+30020,true,false));
  StartupGreetingGate stopped(origin);
  assert(!stopped.Due(origin+10,false,true));
  assert(!stopped.Due(origin+18000,true,false));
 }
}
''')

    def test_three_product_cues_use_authored_bounded_motion(self):
        cases = [
            ('wzh_crawlPanda', 'panda_controller.cc', 'a可爱_熊猫宝宝_叫声哼唧', 'kCrawl'),
            ('wzh_tailRedPanda', 'panda_controller.cc', '熊猫撒娇声音', 'kTail'),
            ('wzh_dinosaur', 'dinosaur_controller.cc', '叫声音效合集 (79)', 'kDinosaur'),
        ]
        for board, controller, name, mechanism in cases:
            with self.subTest(board=board):
                source=(ROOT/'main/boards'/board/controller).read_text()
                self.assertIn('FindSoundIndex("'+name+'")',source)
                self.assertIn('wzh::StartupGreetingAudioReady()',source)
                self.assertIn('last_request_us_.load() > 0',source)
                self.assertLess(source.index('if (servo_output_.blocked() || !pet_state.motion)'),
                                source.index('if (startup.Due('))
                code = '#include "'+str(ROOT/'main/boards'/board/'wzh_user_pairs_catalog.inc')+'"\n'+r'''
#include "wzh_sound_choreography.h"
#include <cassert>
#include <cstring>
#include <cmath>
using namespace wzh;
int main(){
 RegisterUserPairs(wzh_user_assets::kPairs,std::size(wzh_user_assets::kPairs));
 int index=FindUserPair("__NAME__");const UserPair* pair=UserPairAtIndex(index);
 assert(pair&&pair->audio_size>0&&!std::memcmp(pair->audio,"OggS",4));
 constexpr PetMechanism mechanism=PetMechanism::__MECH__;
 const SoundPose rest=mechanism==PetMechanism::kDinosaur?SoundPose{70,90,90,90,90}:
   mechanism==PetMechanism::kTail?SoundPose{90,90,150,0,0}:SoundPose{90,90,90,0,0};
 SoundPlayback sound;sound.playing=sound.audible=true;sound.sequence=1;
 sound.sound_index=index;sound.duration_ms=pair->audio_ms;sound.kind=pair->score->kind;sound.score=pair->score;
 SoundChoreography player;bool powered=false;SoundPose out;
 assert(player.Update(sound,mechanism,rest,0,out,powered,0));
 float minaxis[5]={999,999,999,999,999},maxaxis[5]={-999,-999,-999,-999,-999};
 for(uint32_t t=0;t<player.duration();t+=20){auto pose=player.Sample(t,powered);
  for(int a=0;a<pair->axes;++a){minaxis[a]=std::min(minaxis[a],pose[a]);maxaxis[a]=std::max(maxaxis[a],pose[a]);}}
 float widest=0;for(int a=0;a<pair->axes;++a)widest=std::max(widest,maxaxis[a]-minaxis[a]);
 assert(widest>40); // Noticeable movement after the existing smoothing and limits.
 assert(player.Sample(player.duration(),powered)==rest&&!powered);
}
'''.replace('__NAME__', name).replace('__MECH__', mechanism)
                self.run_cpp(code)

    def test_network_and_prompt_readiness(self):
        source=(ROOT/'main/boards/common/wzh/wzh_system_prompts.cc').read_text()
        method=queue_function(source,'bool StartupGreetingAudioReady(')
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#define CONFIG_WZH_SYSTEM_PROMPTS 1
namespace wzh {
std::atomic<bool> worker_started{false}, startup_network_ready{false}, shutting_down{false};
std::atomic<bool> wifi_setup_requested{false}, wifi_setup_producing{false};
''' + method + r'''
}
int main(){using namespace wzh;
 assert(!StartupGreetingAudioReady());
 worker_started=true;assert(!StartupGreetingAudioReady());
 startup_network_ready=true;wifi_setup_requested=true;assert(!StartupGreetingAudioReady());
 wifi_setup_producing=true;wifi_setup_requested=false;assert(!StartupGreetingAudioReady());
 wifi_setup_producing=false;assert(StartupGreetingAudioReady());
 shutting_down=true;assert(!StartupGreetingAudioReady());
}
''')

    def test_setup_prompt_is_reserved_while_being_queued(self):
        source=(ROOT/'main/boards/common/wzh/wzh_system_prompts.cc').read_text()
        branch=source[source.index('wifi_setup_producing.store(true)'):]
        self.assertLess(branch.index('wifi_setup_producing.store(true)'),branch.index('wifi_setup_requested.store(false)'))
        self.assertLess(branch.index('audio.PlaySound(Lang::Sounds::OGG_WIFICONFIG'),branch.index('wifi_setup_producing.store(false)'))
        self.assertIn('!wifi_setup_requested.load() && !wifi_setup_producing.load()',source)
