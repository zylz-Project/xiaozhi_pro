#!/usr/bin/env python3
"""Compare all three products' paired output trajectories against a saved source baseline."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/tests'))
from test_wzh_reference_expression import reference_adapter


def replay(board,baseline=None):
    common=ROOT/'main/boards/common/wzh'
    source='#include "'+str(ROOT/'main/boards'/board/'wzh_user_pairs_catalog.inc')+'"\n'
    if board=='wzh_dinosaur':source+=reference_adapter()
    source+=r'''
#include <iostream>
#include "wzh_sound_choreography.h"
using namespace wzh;
int main(){
 RegisterUserPairs(wzh_user_assets::kPairs,std::size(wzh_user_assets::kPairs));
 const bool dino=__DINO__,tail=__TAIL__;
 const SoundPose rest=dino?SoundPose{70,90,90,90,90}:tail?SoundPose{90,90,150,0,0}:SoundPose{90,90,90,0,0};
 const auto mechanism=dino?PetMechanism::kDinosaur:tail?PetMechanism::kTail:PetMechanism::kCrawl;
 for(unsigned f=0;f<user_pair_count;++f){
  const auto& pair=user_pairs[f];SoundChoreography player;
  double range=0,speed=0,active=0,total=0,duration=0,body=0;float peak=0;
  double axis_range[5]={};
  for(unsigned v=0;v<SoundChoreography::kVariantCount;++v){
   SoundPlayback s;s.sound_index=kUserPairIndexBase+f;s.score=pair.score;s.kind=pair.score->kind;s.duration_ms=pair.audio_ms;
   __REFERENCE__
   player.Begin(s,mechanism,rest,v*7919U+f*104729U);
   bool on=false;auto previous=player.Sample(0,on),lo=previous,hi=previous;
   for(unsigned t=20;t<player.body_duration();t+=20){
    const auto q=player.Sample(t,on);float largest=0;
    for(unsigned a=0;a<pair.axes;++a){
     lo[a]=std::min(lo[a],q[a]);hi[a]=std::max(hi[a],q[a]);
     const float delta=std::abs(q[a]-previous[a])*50;
     speed+=delta/pair.axes;largest=std::max(largest,delta);peak=std::max(peak,delta);
    }
    active+=largest>=25;total+=1;previous=q;
   }
   for(unsigned a=0;a<pair.axes;++a){range+=(hi[a]-lo[a])/pair.axes;axis_range[a]+=hi[a]-lo[a];}
   duration+=player.duration();body+=player.body_duration();
  }
  const auto n=SoundChoreography::kVariantCount;
  std::cout<<pair.name<<"\t"<<range/n<<"\t"<<speed/total<<"\t"<<active/total<<"\t"<<peak<<"\t"<<duration/n<<"\t"<<body/n;
  for(unsigned a=0;a<5;++a)std::cout<<"\t"<<axis_range[a]/n;
  std::cout<<"\n";
 }
}
'''.replace('__DINO__','true' if board=='wzh_dinosaur' else 'false').replace('__TAIL__','true' if board=='wzh_tailRedPanda' else 'false').replace('__REFERENCE__','MatchReference(player,s,v*7919U+f*104729U);' if board=='wzh_dinosaur' else '')
    with tempfile.TemporaryDirectory() as folder:
        cpp=Path(folder)/'audit.cc';cpp.write_text(source);exe=cpp.with_suffix('')
        includes=(['-I',str(baseline/'main/boards/common/wzh')] if baseline else [])+['-I',str(common)]
        subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',*includes,str(cpp),'-o',str(exe)],check=True)
        output=subprocess.check_output([str(exe)],text=True)
    rows=[]
    keys=['mean_axis_range_deg','mean_joint_speed_dps','moving_fraction_above_25dps','peak_20ms_target_speed_dps','motion_ms','body_ms']+[f'axis_{a}_range_deg' for a in range(5)]
    for line in output.splitlines():
        name,*values=line.split('\t');rows.append({'name':name,**dict(zip(keys,map(float,values)))})
    return rows


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();products=[]
    for board in ['wzh_crawlPanda','wzh_tailRedPanda','wzh_dinosaur']:
        before=replay(board,args.baseline);after=replay(board)
        assert [r['name'] for r in before]==[r['name'] for r in after]
        summary={}
        for key in ['mean_axis_range_deg','mean_joint_speed_dps','moving_fraction_above_25dps','motion_ms']+[f'axis_{a}_range_deg' for a in range(5 if board=='wzh_dinosaur' else 3)]:
            a=sum(r[key] for r in before)/len(before);b=sum(r[key] for r in after)/len(after)
            summary[key]={'before':a,'after':b,'change_percent':(b/a-1)*100}
        products.append({'board':board,'summary':summary,'files':[{'before':a,'after':b} for a,b in zip(before,after)]})
        print(board,json.dumps(summary,ensure_ascii=False))
    args.output.write_text(json.dumps({'source':'actual paired recordings and production C++ motion player','sample_ms':20,'variants_per_source':32,'includes_entry':True,'hardware_measurement':False,'products':products},ensure_ascii=False,indent=2)+'\n')


if __name__=='__main__':main()
