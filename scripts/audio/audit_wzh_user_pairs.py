#!/usr/bin/env python3
"""Sample all embedded user timelines through the production C++ player."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BOARDS = ['wzh_crawlPanda', 'wzh_tailRedPanda', 'wzh_dinosaur']


def audit():
    rows = []
    for board in BOARDS:
        source = '#include "' + str(ROOT/'main/boards'/board/'wzh_user_pairs_catalog.inc') + '"\n' + r'''
#include <cassert>
#include <iostream>
#include <iomanip>
#include "wzh_sound_choreography.h"
using namespace wzh;
int main(){
    const bool dino=std::string("__BOARD__")=="wzh_dinosaur";
    const bool tail=std::string("__BOARD__")=="wzh_tailRedPanda";
    const auto rest=dino?SoundPose{70,90,90,90,90}:tail?SoundPose{90,90,150,0,0}:SoundPose{90,90,90,0,0};
    RegisterUserPairs(wzh_user_assets::kPairs,std::size(wzh_user_assets::kPairs));
    for(size_t i=0;i<user_pair_count;++i){
        const auto& pair=user_pairs[i];
        SoundPlayback sound;sound.sound_index=kUserPairIndexBase+i;sound.duration_ms=pair.audio_ms;
        SoundChoreography player;player.Begin(sound,dino?PetMechanism::kDinosaur:tail?PetMechanism::kTail:PetMechanism::kCrawl,rest);
        assert(player.count()==0);
        SoundPose low=rest,high=rest,previous=rest,command=rest;float speed=0,command_speed=0,return_speed=0;bool powered=false;
        for(uint32_t t=0;t<=player.duration();t+=5){
            const auto pose=player.Sample(t,powered);
            for(unsigned a=0;a<pair.axes;++a){
                const float minimum=dino?(a==3?55:0):tail?(a==1?6:a==2?10:8):8;
                const float maximum=dino?180:tail?(a==1?174:a==2?180:172):172;
                assert(std::isfinite(pose[a])&&pose[a]>=minimum-.001&&pose[a]<=maximum+.001);
                low[a]=std::min(low[a],pose[a]);high[a]=std::max(high[a],pose[a]);
                if(t)speed=std::max(speed,std::abs(pose[a]-previous[a])*200);
                if(t>=player.body_duration()+5)
                    return_speed=std::max(return_speed,std::abs(pose[a]-previous[a])*200);
            }
            if(t%20==0){
                float travel=0;
                for(unsigned a=0;a<pair.axes;++a)travel=std::max(travel,std::abs(pose[a]-command[a]));
                const float mix=travel>8?8/travel:1;
                for(unsigned a=0;a<pair.axes;++a){
                    const float step=(pose[a]-command[a])*mix;
                    command[a]+=step;command_speed=std::max(command_speed,std::abs(step)*50);
                }
            }
            previous=pose;
        }
        float end_error=0;
        for(unsigned a=0;a<pair.axes;++a)end_error=std::max(end_error,std::abs(command[a]-rest[a]));
        assert(command_speed<=400.01&&end_error<2);
        assert(return_speed<=(dino?65:tail?75:100)+.1f);
        assert(player.Sample(player.duration(),powered)==rest&&!powered);
        std::cout<<std::fixed<<std::setprecision(2)<<"{\"index\":"<<kUserPairIndexBase+i
            <<",\"audio_ms\":"<<pair.audio_ms<<",\"authored_motion_ms\":"<<pair.motion_ms
            <<",\"motion_with_return_ms\":"<<player.duration()<<",\"frames\":"<<pair.frame_count
            <<",\"body_ms\":"<<player.body_duration()
            <<",\"peak_target_speed_dps\":"<<speed<<",\"peak_limited_command_speed_dps\":"<<command_speed
            <<",\"return_peak_speed_dps\":"<<return_speed<<",\"return_residual_deg\":"<<end_error<<",\"axis_range_deg\":[";
        for(unsigned a=0;a<pair.axes;++a){if(a)std::cout<<',';std::cout<<'['<<low[a]<<','<<high[a]<<']';}
        std::cout<<"]}\n";
    }
}
'''.replace('__BOARD__', board)
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder)/'audit.cc'
            binary = Path(folder)/'audit'
            cpp.write_text(source)
            subprocess.run(['g++', '-std=c++17', '-O2', '-I', str(ROOT/'main/boards/common/wzh'), str(cpp), '-o', str(binary)], check=True)
            output = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout
        for line in output.splitlines():
            row = json.loads(line)
            row['board'] = board
            rows.append(row)
    metadata = json.loads((ROOT/'docs/wzh_user_pairs_2026-10-20.json').read_text())
    assert len(rows) == len(metadata) == 52
    for row, meta in zip(rows, metadata):
        assert row['board'] == meta['board']
        row['name'] = meta['name']
        row['validation_scope'] = 'source-only base paired trajectory; reference combinations covered by test_wzh_reference_expression.py'
        row['authored_time_scale'] = round(row['body_ms']/row['authored_motion_ms'], 4)
        row['return_ms'] = row['motion_with_return_ms']-row['body_ms']
    target = ROOT/'docs/wzh_user_pair_motion_audit_2026-10-20.json'
    target.write_text(json.dumps(rows, ensure_ascii=False, indent=2)+'\n')
    print(f'{len(rows)} user-pair timelines passed bounds, finite output and neutral-return checks')
    for board in BOARDS:
        selected = [row for row in rows if row['board'] == board]
        print(board, 'pairs', len(selected), 'max return overhead', max(row['motion_with_return_ms']-max(row['audio_ms'],row['authored_motion_ms']) for row in selected), 'max target speed', max(row['peak_target_speed_dps'] for row in selected))
    return rows


if __name__ == '__main__':
    audit()
