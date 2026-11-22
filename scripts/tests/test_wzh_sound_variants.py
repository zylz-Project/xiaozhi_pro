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

    def test_new_dinosaur_roars_turn_after_the_centered_skyward_onset(self):
        source=(ROOT/'main/boards/wzh_dinosaur/dinosaur_controller.cc').read_text()
        start=source.index('constexpr Keyframe kRoarFrontFrames[]')
        end=source.index('constexpr Keyframe kSoundEatNibbleFrames[]',start)
        self.run_cpp(r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
constexpr unsigned kMaxClipFrames=16;
constexpr int SERVO_TAIL_UD_MIN=55,SERVO_TAIL_UD_MAX=180;
enum class MotionEase{kSmooth,kLinear,kFastOut};
struct Keyframe{
 int neck_tilt,neck_lean,head_turn,tail_ud,tail_lr;
 uint16_t duration_ms;
 MotionEase ease;
};
struct MotionClip{const char* name;const Keyframe* frames;unsigned count;bool authored_dynamics=false;};
'''+source[start:end]+r'''
int main(){
    assert(std::size(kSkyRoarClips)==21);
    for(unsigned i=7;i<12;++i){
        const auto& clip=kSkyRoarClips[i];
        const auto& first=clip.frames[0];
        assert(first.neck_tilt==0&&first.neck_lean==90&&first.head_turn==90&&first.tail_lr==90);
        std::array<int,5> previous{70,90,90,90,90};
        bool right=false,left=false;
        unsigned calls=0,duration=0;
        for(unsigned frame=0;frame<clip.count;++frame){
            const auto& key=clip.frames[frame];
            const std::array<int,5> pose{key.neck_tilt,key.neck_lean,key.head_turn,
                                         key.tail_ud,key.tail_lr};
            duration+=key.duration_ms;
            assert(key.duration_ms>=420&&key.neck_tilt<=28);
            for(unsigned axis=0;axis<pose.size();++axis){
                const auto travel=std::max(pose[axis]-previous[axis],previous[axis]-pose[axis]);
                assert(travel*1000<=key.duration_ms*80);
            }
            previous=pose;
            right|=key.head_turn<=65;
            left|=key.head_turn>=115;
            calls+=key.neck_tilt==0&&key.head_turn==90;
        }
        if(i==7||i==8){
            assert(duration<1500&&clip.count==2);
            assert(right==(i==7)&&left==(i==8));
        }else{
            assert(duration>=2400&&right&&left);
        }
        assert(calls==(i==11?2:1));
    }
    for(unsigned i=12;i<std::size(kSkyRoarClips);++i){
        const auto& clip=kSkyRoarClips[i];
        assert(clip.authored_dynamics&&std::abs(clip.frames[0].head_turn-90)>=20);
        std::array<int,5> previous{70,90,90,90,90};
        for(unsigned frame=0;frame<clip.count;++frame){
            const auto& key=clip.frames[frame];
            const std::array<int,5> pose{key.neck_tilt,key.neck_lean,key.head_turn,
                                         key.tail_ud,key.tail_lr};
            assert(key.duration_ms>=420);
            for(unsigned axis=0;axis<pose.size();++axis)
                assert(std::abs(pose[axis]-previous[axis])*1000<=key.duration_ms*80);
            previous=pose;
        }
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
const wzh_dino::MotionClip kSkyRoarClips[]={{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},
                                         {12},{13},{14},{15},{16},{17},{18},{19},{20}};
uint32_t state=12345;
uint32_t esp_random(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
int irnd(int n){return esp_random()%n;}
struct DinosaurController{
    int last_sky_roar_clip_index_=-1;
    wzh::SoundVariantSelector<21> roar_variants_;
    const wzh_dino::MotionClip* PickSkyRoarClip(uint32_t,const wzh::SoundPlayback* = nullptr);
};
'''+method+r'''
int main(){
    DinosaurController controller;
    const unsigned durations[]={0,1700,1800,2400,3200,3201,3480,3860,4200,4201,5200,6500,9000};
    const std::set<int> expected[]={
        {1,7,8,12,13,14,20},{1,7,8,12,13,14,20},{0,1,5,7,8,12,13,14,20},
        {0,1,5,7,8,9,12,13,14,15,20},{0,1,5,7,8,9,12,13,14,15,18,20},{0,5,9,14,15,18,20},
        {0,5,9,10,14,15,18,20},{0,5,9,10,11,14,15,16,18},{0,2,5,9,10,11,14,15,16,18,19},
        {2,9,10,11,15,16,18,19},{2,3,9,10,11,15,16,17,18,19},
        {2,3,4,6,9,10,11,16,17,19},{2,3,4,6,9,10,11,17,19}
    };
    for(unsigned length=0;length<std::size(durations);++length){
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
    for(unsigned length=0;length<std::size(durations);++length){
        int previous=-1;
        for(unsigned i=0;i<50;++i){
            int n=controller.PickSkyRoarClip(durations[length])->index;
            assert(n!=previous&&expected[length].count(n));previous=n;
        }
    }
}
''')
