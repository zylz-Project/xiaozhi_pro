"""User-supplied paired assets, exact timeline routing and cold-start hold."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
COMMON=ROOT/'main/boards/common/wzh'


class WzhUserPairsTest(unittest.TestCase):
    def run_cpp(self, source):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as folder:
            cpp=Path(folder)/'test.cc';cpp.write_text(source);exe=cpp.with_suffix('')
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror','-I',str(COMMON),str(cpp),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_all_52_embedded_audio_bytes_match_validated_archives(self):
        rows=json.loads((ROOT/'docs/wzh_user_pairs_2026-10-20.json').read_text())
        self.assertEqual(len(rows),52)
        for board,count in [('wzh_crawlPanda',14),('wzh_tailRedPanda',14),('wzh_dinosaur',24)]:
            selected=[r for r in rows if r['board']==board]
            self.assertEqual(len(selected),count)
            text=(ROOT/'main/boards'/board/'wzh_user_pairs_catalog.inc').read_text()
            blocks=re.findall(r'kAudio(\d+)\[\] = \{(.*?)\};',text,re.S)
            self.assertEqual(len(blocks),count)
            for (index,block),metadata in zip(blocks,selected):
                audio=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',block))
                self.assertEqual(hashlib.sha256(audio).hexdigest(),metadata['audio_sha256'])
                self.assertEqual(len(audio),metadata['audio_size'])
                self.assertTrue(audio.startswith(b'OggS'))

    def test_all_user_timelines_override_generic_motion_and_finish_centered(self):
        for board in ['wzh_crawlPanda','wzh_tailRedPanda','wzh_dinosaur']:
            with self.subTest(board=board):
                self.run_cpp('#include "'+str(ROOT/'main/boards'/board/'wzh_user_pairs_catalog.inc')+'"\n'+r'''
#include <cassert>
#include <cstring>
#include "wzh_sound_choreography.h"
using namespace wzh;
int main(){
    RegisterUserPairs(wzh_user_assets::kPairs,std::size(wzh_user_assets::kPairs));
    const bool dino=user_pairs[0].axes==5;
    const bool tail=std::strstr("__BOARD__","tail")!=nullptr;
    const SoundPose rest=dino?SoundPose{70,90,90,90,90}:tail?SoundPose{90,90,150,0,0}:SoundPose{90,90,90,0,0};
    const auto mechanism=dino?PetMechanism::kDinosaur:tail?PetMechanism::kTail:PetMechanism::kCrawl;
    for(size_t i=0;i<user_pair_count;++i){
        const auto& pair=user_pairs[i];const int index=kUserPairIndexBase+i;
        assert(FindUserPair(pair.name)==index&&UserPairAtIndex(index)==&pair);
        assert(pair.audio_size>0&&std::memcmp(pair.audio,"OggS",4)==0);
        uint32_t duration=0;
        for(unsigned f=0;f<pair.frame_count;++f){duration+=pair.frames[f].duration_ms;}
        assert(duration==pair.motion_ms);
        SoundChoreography player;SoundPlayback sound;
        sound.playing=sound.audible=true;sound.sequence=i+1;
        sound.sound_index=index;sound.duration_ms=pair.audio_ms;
        bool powered=false;SoundPose out;
        assert(player.Update(sound,mechanism,rest,0,out,powered,1234)&&powered&&out==rest);
        assert(player.count()==0); // No generic score is built over an exact pair.
        const uint32_t end=player.duration();
        assert(end>=std::max(pair.audio_ms,pair.motion_ms)+650&&end<33000);
        for(uint32_t t=0;t<=end;t+=10){
            auto pose=player.Sample(t,powered);
            for(unsigned a=0;a<pair.axes;++a){
                const float lo=dino?(a==3?55:0):tail?(a==1?6:a==2?0:8):8;
                const float hi=dino?180:tail?(a==1?174:a==2?180:172):172;
                assert(std::isfinite(pose[a])&&pose[a]>=lo-.001&&pose[a]<=hi+.001);
            }
            if(t<end)assert(powered);
        }
        assert(player.Sample(end,powered)==rest&&!powered);
        player.Cancel(sound.sequence);
        assert(!player.Update(sound,mechanism,rest,end+20,out,powered,123));
    }
    assert(!UserPairAtIndex(-1)&&!UserPairAtIndex(1000)&&FindUserPair("not-present")==-1);
}
'''.replace('__BOARD__',board))

    def test_one_second_power_hold_release_invalid_samples_and_clock_wrap(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_power_startup.h"
using namespace wzh;using R=PowerStartupGate::Result;
int main(){
    for(uint32_t start:{0U,UINT32_MAX-500}){
        PowerStartupGate gate;
        for(uint32_t elapsed=0;elapsed<1000;elapsed+=20)assert(gate.Update(start+elapsed,true,true)==R::kWaiting);
        assert(gate.Update(start+1000,true,true)==R::kReady);
        assert(gate.Update(start+1020,true,false)==R::kReady);
    }
    PowerStartupGate tap;
    assert(tap.Update(0,true,true)==R::kWaiting);
    for(unsigned t=20;t<=980;t+=20)assert(tap.Update(t,true,true)==R::kWaiting);
    assert(tap.Update(1000,true,false)==R::kWaiting);
    assert(tap.Update(1060,true,false)==R::kRejected);
    PowerStartupGate invalid;
    for(unsigned t=0;t<=600;t+=20)assert(invalid.Update(t,true,true)==R::kWaiting);
    invalid.Update(620,false,true);
    for(unsigned t=640;t<1640;t+=20)assert(invalid.Update(t,true,true)==R::kWaiting);
    assert(invalid.Update(1640,true,true)==R::kReady);
    PowerStartupGate gap;gap.Update(0,true,true);gap.Update(800,true,true);
    assert(gap.Update(1600,true,true)==R::kWaiting);
    PowerStartupGate unknown;
    for(unsigned t=0;t<3000;t+=20)assert(unknown.Update(t,false,false)==R::kWaiting);
    assert(unknown.Update(3000,false,false)==R::kRejected);
}
'''.replace('#include <cassert>','#include <cassert>\n#include <initializer_list>'))

    def test_panda_gaze_ignores_syllable_attack_and_tail_has_bounded_speed(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include "wzh_voice_motion.h"
using namespace wzh;
int main(){
    for(auto mechanism:{VoiceMechanism::kCrawl,VoiceMechanism::kTail,VoiceMechanism::kDinosaur}){
        VoiceMotion quiet_attack,loud_attack;
        std::array<float,5> previous{};
        const unsigned head=mechanism==VoiceMechanism::kDinosaur?2:0;
        float low=1000,high=-1000;
        for(unsigned t=0;t<120000;t+=20){
            const auto a=quiet_attack.Update(mechanism,true,.3f,0,t);
            const auto b=loud_attack.Update(mechanism,true,.3f,t%200<60?1:0,t);
            assert(std::abs(a[head]-b[head])<.0001f);
            assert(std::abs(b[head]-previous[head])<=
                (mechanism==VoiceMechanism::kDinosaur?2.401f:3.201f));
            if(mechanism==VoiceMechanism::kTail){
                for(unsigned axis=1;axis<=2;++axis)
                    assert(std::abs(b[axis]-previous[axis])<=3.801f);
            }
            low=std::min(low,b[head]);high=std::max(high,b[head]);previous=b;
        }
        assert(high-low>100); // Softer timing still preserves a visible wide gaze.
        for(unsigned t=120000;t<123000;t+=20)
            previous=loud_attack.Update(mechanism,true,0,0,t);
        for(float value:previous)assert(std::abs(value)<.5f);
    }
}
''')

    def test_dinosaur_random_reference_directions_remain_balanced(self):
        from test_wzh_action_queue import queue_function
        method=queue_function((ROOT/'main/boards/wzh_dinosaur/dinosaur_controller.cc').read_text(),'bool DinosaurController::PickBalancedMirror(')
        self.run_cpp(r'''
#include <cassert>
#include <cstdint>
uint32_t state=1234;
uint32_t esp_random(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
struct DinosaurController{
    int8_t mirror_balance_=0;bool last_mirror_=false;uint8_t mirror_run_length_=0;
    bool PickBalancedMirror();
};
'''+method+r'''
int main(){
    DinosaurController controller;int balance=0,run=0;bool last=false;
    for(unsigned i=0;i<10000;++i){
        bool value=controller.PickBalancedMirror();balance+=value?1:-1;
        assert(balance>=-1&&balance<=1);
        run=i&&value==last?run+1:1;assert(run<=2);last=value;
    }
}
''')

    def test_three_of_four_autonomous_slots_prefer_pairs_and_rejections_consume_nothing(self):
        from test_wzh_feedback import WzhFeedbackTest
        from test_wzh_action_queue import queue_function
        class Capture(WzhFeedbackTest):
            def run_cpp(self,program):self.program=program
        case=Capture();case.test_external_sound_queue_preserves_file_and_rejects_busy_and_chat()
        program=case.program[:case.program.index('int main(){')]
        program='#include <cstdio>\n#include <cstring>\n#include "wzh_user_pairs.h"\n#include "wzh_sound_coverage.h"\n'+program
        program=program.replace('    auto Snapshot(){return files;}','    auto Snapshot(){return files;}\n    size_t file_count(){return files.size();}')
        program=program.replace('    bool playback_pending_=false;','    SoundCoverage paired_coverage_,sound_coverage_;uint32_t autonomous_turn_=0;\n    bool PlayAutonomousSound(bool,uint32_t=100);\n    bool playback_pending_=false;')
        method=queue_function((COMMON/'wzh_audio_player.cc').read_text(),'bool AudioPlayer::PlayAutonomousSound(')
        self.run_cpp(program+method+r'''
int main(){
    const uint8_t audio[]={24};const UserPairFrame frame{100,{90,90,90,0,0},2};
    const UserPair pairs[]={
        {"tuned-one","撒娇",audio,1,100,&frame,1,100,3},
        {"tuned-two","宝宝开心",audio,1,100,&frame,1,100,3}};
    RegisterUserPairs(pairs,2);AudioPlayer p;
    for(unsigned i=0;i<40;++i){
        queue_full=true;assert(!p.PlayAutonomousSound(false));
        assert(p.autonomous_turn_==i);queue_full=false;
        assert(p.PlayAutonomousSound(false));
        assert((queue.back().index>=32)==(i%4!=3));
        assert(p.autonomous_turn_==i+1);p.playback_pending_=false;
    }
    assert(p.PlayIndex(0)&&queue.back().index==0);p.playback_pending_=false;
    p.store.files.clear();
    for(unsigned i=0;i<12;++i){assert(p.PlayAutonomousSound(false));assert(queue.back().index>=32);p.playback_pending_=false;}
    p.store.files={{"nature",100,1000,0,48000,"ambient"}};
    for(unsigned i=0;i<12;++i){assert(p.PlayAutonomousSound(false));assert(queue.back().index>=32);p.playback_pending_=false;}
}
''')

    def test_paired_audio_uses_memory_source_and_never_reads_external_flash(self):
        from test_wzh_feedback import WzhFeedbackTest
        class Capture(WzhFeedbackTest):
            def run_cpp(self,program):self.program=program
        case=Capture();case.test_external_playback_reads_selected_flash_file_and_honors_stop()
        program=case.program[:case.program.index('int main(){')]
        self.run_cpp('#include <cstdio>\n#include <cstring>\n#include "wzh_user_pairs.h"\n'+program+r'''
int main(){
    std::array<uint8_t,5000> bytes;bytes.fill(24);
    const UserPairFrame frame{100,{90,90,90,0,0},2};
    const UserPair pair={"factory-pair","",bytes.data(),bytes.size(),6000,&frame,1,100,3};
    RegisterUserPairs(&pair,1);AudioPlayer player;
    generation_ptr=&player.playback_generation_;
    AudioPlayer::PlayRequest request{32,1,{}};
    assert(player.PlayFile(request));
    assert(reads.empty()&&Application::GetInstance().service.packets==2);
    // A captured metadata mismatch cannot read beyond the registered asset.
    request.file.size=6000;
    assert(!player.PlayFile(request));assert(reads.empty());
}
''')
