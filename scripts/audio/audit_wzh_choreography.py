#!/usr/bin/env python3
"""Replay the production C++ scores at 5 ms; write timing/travel and preview data."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def audit():
    manifest = json.loads((ROOT / 'docs/wzh_sound_pack_2026-10-19.json').read_text())
    cases = []
    for row in manifest:
        product = row['path'].split('/')[0]
        mechanism = {'crawling_panda': 'kCrawl', 'tail_wagging_panda': 'kTail', 'dinosaur': 'kDinosaur'}[product]
        cases.append('{' + json.dumps(row['name'], ensure_ascii=False) + ',PetMechanism::' + mechanism + '},')
    dino_source=(ROOT/'main/boards/wzh_dinosaur/dinosaur_controller.cc').read_text()
    roar_data=dino_source[dino_source.index('constexpr Keyframe kRoarFrontFrames[]'):dino_source.index('constexpr bool ValidateRoarClips()')]
    source = r'''
#include <cassert>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <set>
#include "wzh_sound_choreography.h"
using namespace wzh;
enum class MotionEase {kSmooth,kFastOut,kLinear};
struct Keyframe {int neck_tilt,neck_lean,head_turn,tail_ud,tail_lr;uint16_t duration_ms;MotionEase ease;};
struct MotionClip {const char* name;const Keyframe* frames;uint8_t count;};
__ROAR_DATA__
int main(){
    struct Case{const char* name;PetMechanism mechanism;};
    const Case cases[]={__CASES__};
    unsigned id=0;
    for(const auto& item:cases){
        const auto* score=FindSoundScore(item.name);assert(score);
        const bool dino=item.mechanism==PetMechanism::kDinosaur;
        const bool tail=item.mechanism==PetMechanism::kTail;
        SoundPose rest=dino?SoundPose{70,90,90,90,90}:tail?SoundPose{90,90,150,0,0}:SoundPose{90,90,90,0,0};
        SoundChoreography player;std::set<unsigned> phrases;
        for(unsigned variant=1;variant<=SoundChoreography::kVariantCount;++variant){
            SoundPlayback sound;sound.score=score;sound.kind=score->kind;
            sound.duration_ms=score->duration_ms;sound.sequence=variant;
            if(dino&&sound.kind==SoundKind::kRoar){
                const unsigned short_set[]={0,1,5},medium_set[]={0,1,2,5},long_set[]={2,3,0,6},extended_set[]={3,4,2,6};
                const unsigned* set=sound.duration_ms<=1700?short_set:sound.duration_ms<=3200?medium_set:sound.duration_ms<=5200?long_set:extended_set;
                const unsigned count=sound.duration_ms<=1700?3:4;
                const unsigned clip_index=set[(score->seed+variant)%count];
                player.SetRoarClip(kSkyRoarClips[clip_index],kCompactRoarTailAngles[clip_index]);
                // All seven borrowed paths must still fit physical constraints,
                // including paths not selected for the eight exported previews.
                std::set<int> compact_tails;
                for(unsigned clip_id=0;clip_id<std::size(kSkyRoarClips);++clip_id){
                    SoundChoreography candidate;
                    candidate.SetRoarClip(kSkyRoarClips[clip_id],kCompactRoarTailAngles[clip_id]);
                    candidate.Begin(sound,item.mechanism,rest);
                    assert(candidate.duration()<=sound.duration_ms+6500);
                    SoundPose before=rest;bool on=false;
                    if(sound.duration_ms<2100){
                        SoundPose peak{};float best=999;
                        for(unsigned probe=0;probe<=candidate.duration();probe+=5){
                            auto q=candidate.Sample(probe,on);
                            if(q[0]<best){best=q[0];peak=q;}
                        }
                        assert(peak[0]<=.01);
                        assert(compact_tails.insert(std::lround(peak[3])).second);
                    }
                    for(unsigned time=0;time<=candidate.duration();time+=5){
                        auto q=candidate.Sample(time,on);
                        for(unsigned axis=0;axis<5;++axis){
                            assert(q[axis]>= (axis==3?55:0)&&q[axis]<=180);
                            assert(std::abs(q[axis]-before[axis])*200<=210);
                        }
                        before=q;
                    }
                }
            }
            player.Begin(sound,item.mechanism,rest,score->seed ^ variant*0x9e3779b9U);
            assert(phrases.insert(player.variant()).second);
            SoundPose low=rest,high=rest,previous=rest;
            float max_speed=0;bool powered=false;unsigned longest=0,run=0;
            for(unsigned t=0;t<=player.duration()+5;t+=5){
                auto out=player.Sample(t,powered);
                if(powered){run+=5;longest=std::max(longest,run);}else{run=0;}
                for(unsigned axis=0;axis<(dino?5U:3U);++axis){
                    assert(std::isfinite(out[axis]));
                    const float lower=dino?(axis==3?55:0):axis==0?30:tail?(axis==1?6:0):8;
                    const float upper=dino?180:axis==0?150:tail?(axis==1?174:180):172;
                    assert(out[axis]>=lower-.01&&out[axis]<=upper+.01);
                    low[axis]=std::min(low[axis],out[axis]);high[axis]=std::max(high[axis],out[axis]);
                    max_speed=std::max(max_speed,std::abs(out[axis]-previous[axis])*200);
                }
                previous=out;
            }
            assert(max_speed<=(dino?210:401));
            assert(longest<60000); // Long ambience deliberately releases torque.
            if(player.duration()<sound.duration_ms){std::cerr<<"SHORT "<<item.name<<" audio="<<sound.duration_ms<<" motion="<<player.duration()<<" frames="<<player.count()<<"\n";}
            assert(player.duration()>=sound.duration_ms);
            const uint32_t duration_limit=sound.kind==SoundKind::kAmbient?sound.duration_ms+8000:sound.duration_ms+(dino?6500:5000);
            if(player.duration()>duration_limit){std::cerr<<"LONG "<<item.name<<" audio="<<sound.duration_ms<<" motion="<<player.duration()<<" frames="<<player.count()<<"\n";}
            assert(player.duration()<=duration_limit);
            std::cout<<"{\"file\":"<<id<<",\"variant\":"<<variant<<",\"phrase_variant\":"<<player.variant()+1<<",\"audio_ms\":"<<sound.duration_ms
                <<",\"motion_ms\":"<<player.duration()<<",\"frames\":"<<player.count()
                <<",\"max_speed_dps\":"<<max_speed<<",\"max_powered_ms\":"<<longest<<",\"range\":[";
            for(unsigned a=0;a<(dino?5U:3U);++a){if(a)std::cout<<',';std::cout<<'['<<low[a]<<','<<high[a]<<']';}
            std::cout<<"],\"preview\":[";
            // 100 ms preview, computed by the same production player.
            for(unsigned t=0;t<=player.duration();t+=100){
                if(t)std::cout<<',';
                auto out=player.Sample(t,powered);
                std::cout<<'['<<t<<','<<powered;
                for(unsigned a=0;a<(dino?5U:3U);++a)std::cout<<','<<std::round(out[a]*10)/10;
                std::cout<<']';
            }
            std::cout<<"]}\n";
        }
        ++id;
    }
}
'''.replace('__CASES__', '\n'.join(cases)).replace('__ROAR_DATA__',roar_data)
    with tempfile.TemporaryDirectory() as directory:
        cpp = Path(directory) / 'audit.cc'
        cpp.write_text(source)
        exe = cpp.with_suffix('')
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I',
                        str(ROOT/'main/boards/common/wzh'), str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f'audit failed ({result.returncode}):\n{result.stderr}')
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    for file_index, item in enumerate(manifest):
        samples = [row for row in rows if row['file'] == file_index]
        expected = 32
        # IDs alone cannot prove visible diversity: short scores can collapse
        # multiple phrases to an identical lift/return. Compare actual output.
        unique = {json.dumps(row['preview']) for row in samples}
        assert len(unique) == expected, (item['path'], len(unique), expected)
    return manifest, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'docs/wzh_sound_motion_audit_2026-10-19.json')
    args = parser.parse_args()
    manifest, rows = audit()
    args.output.write_text(json.dumps({'files':manifest, 'trajectories':rows}, ensure_ascii=False, separators=(',', ':'))+'\n')
    print(f'{len(rows)} trajectories: timing, limits, peak speed, centered ending and torque breaks passed')
    for row in rows:
        if row['motion_ms']-row['audio_ms']>300:
            print(manifest[row['file']]['path'], row['variant'], row['audio_ms'], row['motion_ms'])


if __name__ == '__main__':
    main()
