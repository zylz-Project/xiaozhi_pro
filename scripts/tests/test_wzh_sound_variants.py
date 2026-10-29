"""Replay per-recording motion selection, audible onset and dinosaur roar pools."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WzhSoundVariantsTest(unittest.TestCase):
    def run_cpp(self, source):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory)/'test.cc'
            cpp.write_text(source)
            exe = cpp.with_suffix('')
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT/'main/boards/common/wzh'), str(cpp), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_32_interleaved_files_cover_every_variant_without_boundary_repeats(self):
        self.run_cpp(r'''
#include <array>
#include <cassert>
#include <set>
#include "wzh_sound_variants.h"
using namespace wzh;
int main(){
    SoundVariantSelector<8> selector;
    std::array<SoundScore,16> scores{};
    std::array<int,32> previous;previous.fill(-1);
    for(unsigned round=0;round<5;++round){
        std::array<std::set<int>,32> covered;
        for(unsigned turn=0;turn<8;++turn){
            for(unsigned file=0;file<32;++file){
                SoundPlayback sound;
                // Known files keep their score identity when the directory is
                // reordered. Unknown files use separate captured Flash indexes.
                sound.score=file<16?&scores[file]:nullptr;
                sound.sound_index=file<16?(file+turn)%16:file;
                sound.sequence=1+round*256+turn*32+file;
                int chosen=selector.Draw(sound,sound.sequence*7919U);
                assert(chosen>=0&&chosen<8&&chosen!=previous[file]);
                assert(covered[file].insert(chosen).second);
                previous[file]=chosen;
            }
        }
        for(const auto& choices:covered)assert(choices.size()==8);
    }
    // More than 32 identities are evicted without unbounded memory or indexes.
    for(int file=32;file<132;++file){SoundPlayback s;s.sound_index=file;
        int n=selector.Draw(s,file);assert(n>=0&&n<8);}
}
''')

    def test_entropy_changes_order_replay_is_stable_and_invalid_counts_consume_nothing(self):
        self.run_cpp(r'''
#include <cassert>
#include <set>
#include <string>
#include "wzh_sound_variants.h"
using namespace wzh;
int main(){
    std::set<std::string> orders;
    SoundPlayback sound;sound.sound_index=7;
    for(unsigned seed=0;seed<128;++seed){
        SoundVariantSelector<8> a,b;
        std::string order;int previous=-1;
        for(unsigned round=0;round<4;++round){
            std::set<int> covered;
            for(unsigned i=0;i<8;++i){
                assert(a.Draw(sound,123,0)==-1&&a.Draw(sound,123,9)==-1);
                uint32_t entropy=seed*7919U+round*1234567U+i;
                int n=a.Draw(sound,entropy);
                assert(n==b.Draw(sound,entropy)&&n!=previous);
                assert(covered.insert(n).second);previous=n;
                order+=char('0'+n);
            }
        }
        orders.insert(order);
    }
    assert(orders.size()>64);
    // Duration-compatible candidate sets may shrink or grow safely.
    SoundVariantSelector<4> a;
    assert(a.Draw(sound,0,1)==0);
    for(unsigned count:{3U,4U,3U}){
        std::set<int> seen;
        for(unsigned i=0;i<count;++i)assert(seen.insert(a.Draw(sound,i,count)).second);
    }
}
''')

    def test_waiting_pcm_updates_cancel_and_completed_packets_do_not_redraw(self):
        self.run_cpp(r'''
#include <cassert>
#include <set>
#include "wzh_sound_choreography.h"
using namespace wzh;
int main(){
    for(auto mechanism:{PetMechanism::kCrawl,PetMechanism::kTail,PetMechanism::kDinosaur}){
        const SoundPose rest=mechanism==PetMechanism::kDinosaur?SoundPose{70,90,90,90,90}:
            mechanism==PetMechanism::kTail?SoundPose{90,90,150,0,0}:SoundPose{90,90,90,0,0};
        SoundChoreography player,reference;SoundPose out;bool powered=false;
        SoundPlayback sound;sound.playing=true;sound.duration_ms=6000;
        sound.score=FindSoundScore("Cute_baby_panda_voca");
        for(unsigned round=0;round<3;++round){
            std::set<unsigned> covered;
            for(unsigned turn=0;turn<8;++turn){
                sound.sequence=1+(round*8+turn)*2;sound.audible=false;
                sound.elapsed_ms=0;
                for(unsigned t=0;t<200;t+=20){
                    assert(!player.NeedsBegin(sound));
                    assert(player.Update(sound,mechanism,rest,t,out,powered,t)&&!powered);
                }
                sound.audible=true;const uint32_t entropy=1234+sound.sequence*7919;
                assert(player.NeedsBegin(sound));
                player.Update(sound,mechanism,rest,200,out,powered,entropy);
                reference.Begin(sound,mechanism,rest,entropy);
                assert(player.variant()==reference.variant());
                assert(covered.insert(player.variant()).second);
                const unsigned chosen=player.variant();
                for(unsigned t=220;t<500;t+=20){
                    sound.elapsed_ms=t-200;assert(!player.NeedsBegin(sound));
                    player.Update(sound,mechanism,rest,t,out,powered,t*12345);
                    assert(player.variant()==chosen);
                }
                player.Cancel(sound.sequence);
                assert(!player.Update(sound,mechanism,rest,520,out,powered,456));
                // A cancelled packet which never became audible consumes nothing.
                ++sound.sequence;player.Cancel(sound.sequence);
                assert(!player.Update(sound,mechanism,rest,540,out,powered,789));
            }
        }
        ++sound.sequence;sound.elapsed_ms=0;
        player.Update(sound,mechanism,rest,600,out,powered,123);
        unsigned chosen=player.variant();sound.elapsed_ms=player.duration();
        player.Update(sound,mechanism,rest,700,out,powered,456);
        assert(out==rest&&!player.NeedsBegin(sound));
        assert(!player.Update(sound,mechanism,rest,720,out,powered,789));
        assert(player.variant()==chosen);
    }
}
''')

    def test_dinosaur_each_recording_shuffles_only_duration_compatible_roars(self):
        from test_wzh_action_queue import queue_function
        source=(ROOT/'main/boards/wzh_dinosaur/dinosaur_controller.cc').read_text()
        method=queue_function(source, 'const wzh_dino::MotionClip* DinosaurController::PickSkyRoarClip(')
        self.run_cpp(r'''
#include <array>
#include <cassert>
#include <set>
#include "wzh_sound_variants.h"
namespace wzh_dino{struct MotionClip{int index;};}
const wzh_dino::MotionClip kSkyRoarClips[]={{0},{1},{2},{3},{4},{5},{6}};
uint32_t state=12345;
uint32_t esp_random(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
int irnd(int n){return esp_random()%n;}
struct DinosaurController{
    int last_sky_roar_clip_index_=-1;
    wzh::SoundVariantSelector<4> roar_variants_;
    const wzh_dino::MotionClip* PickSkyRoarClip(uint32_t,const wzh::SoundPlayback* = nullptr);
};
'''+method+r'''
int main(){
    DinosaurController controller;
    const unsigned durations[]={1700,3200,5200,9000};
    const std::set<int> expected[]={{0,1,5},{0,1,2,5},{2,3,0,6},{3,4,2,6}};
    for(unsigned length=0;length<4;++length){
        std::array<int,8> previous;previous.fill(-1);
        for(unsigned round=0;round<6;++round){
            std::array<std::set<int>,8> seen;
            for(unsigned turn=0;turn<expected[length].size();++turn){
                for(unsigned file=0;file<8;++file){
                    wzh::SoundPlayback sound;sound.sound_index=length*8+file;
                    int n=controller.PickSkyRoarClip(durations[length],&sound)->index;
                    assert(expected[length].count(n)&&n!=previous[file]);
                    assert(seen[file].insert(n).second);previous[file]=n;
                }
            }
            for(const auto& choices:seen)assert(choices==expected[length]);
        }
    }
    for(unsigned length=0;length<4;++length){
        int previous=-1;
        for(unsigned i=0;i<50;++i){
            int n=controller.PickSkyRoarClip(durations[length])->index;
            assert(n!=previous&&expected[length].count(n));previous=n;
        }
    }
}
''')
