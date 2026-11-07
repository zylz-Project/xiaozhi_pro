"""Replay actual paired recordings and reference paths, without an ESP-IDF build."""
from pathlib import Path
import re
import unittest

import test_wzh_fluent_motion as motion_tests

ROOT = Path(__file__).resolve().parents[2]
BOARDS = ['wzh_crawlPanda', 'wzh_tailRedPanda', 'wzh_dinosaur']


def reference_adapter():
    folder=ROOT/'main/boards/wzh_dinosaur'
    header=(folder/'dinosaur_controller.h').read_text()
    types=header[header.index('namespace wzh_dino {'):header.index('}  // namespace wzh_dino')+len('}  // namespace wzh_dino')]
    names=re.findall(r'constexpr Keyframe (k\w+)\[\]',(folder/'dinosaur_reference_motion.h').read_text())
    arrays=[]
    for group in ['Discover','Affection','Happy','Proud','Eat','Listen','Startled','Sleepy']:
        entries=['{"'+name+'",reference_motion::'+name+',std::size(reference_motion::'+name+')}'
                 for name in names if name.startswith('k'+group)]
        arrays.append('const wzh_dino::MotionClip refs'+group+'[]={'+','.join(entries)+'};')
    return '#include <iterator>\n'+types+'\n#include "'+str(folder/'dinosaur_reference_motion.h')+'"\n'+\
        '#include "wzh_sound_choreography.h"\n'+'\n'.join(arrays)+r'''
void MatchReference(wzh::SoundChoreography& player,const wzh::SoundPlayback& sound,unsigned entropy){
    const wzh_dino::MotionClip* clips=refsDiscover;unsigned count=std::size(refsDiscover);
    using wzh::SoundKind;
    switch(sound.kind){
        case SoundKind::kAffection:clips=refsAffection;count=std::size(refsAffection);break;
        case SoundKind::kLaugh:case SoundKind::kStretch:clips=refsHappy;count=std::size(refsHappy);break;
        case SoundKind::kRoar:clips=refsProud;count=std::size(refsProud);break;
        case SoundKind::kChew:clips=refsEat;count=std::size(refsEat);break;
        case SoundKind::kSearch:case SoundKind::kSteps:clips=refsListen;count=std::size(refsListen);break;
        case SoundKind::kSneeze:clips=refsStartled;count=std::size(refsStartled);break;
        case SoundKind::kSleep:clips=refsSleepy;count=std::size(refsSleepy);break;
        default:break;
    }
    player.SetReferenceClip(clips[player.ReferenceVariant(sound,entropy,count)]);
}
'''


class WzhReferenceExpressionTest(unittest.TestCase):
    def test_paired_roars_face_sky_before_details_and_chewing_stays_by_the_food(self):
        motion_tests.WzhFluentMotionTest.run_cpp(self,
            '#include "'+str(ROOT/'main/boards/wzh_dinosaur/wzh_user_pairs_catalog.inc')+'"\n'+
            reference_adapter()+r'''
#include <cassert>
using namespace wzh;
int main(){
    RegisterUserPairs(wzh_user_assets::kPairs,std::size(wzh_user_assets::kPairs));
    const SoundPose rest{70,90,90,90,90};
    unsigned roars=0,chews=0;
    for(unsigned file=0;file<user_pair_count;++file){
        const auto& pair=user_pairs[file];
        const bool roar=pair.score->kind==SoundKind::kRoar;
        if(!roar&&pair.score->kind!=SoundKind::kChew)continue;
        roar?++roars:++chews;
        SoundChoreography player;
        for(unsigned v=0;v<SoundChoreography::kVariantCount;++v){
            SoundPlayback s;s.sound_index=kUserPairIndexBase+file;s.score=pair.score;
            s.kind=pair.score->kind;s.duration_ms=pair.audio_ms;
            MatchReference(player,s,v*7919U);
            player.Begin(s,PetMechanism::kDinosaur,rest,v*7919U);
            bool powered=false;
            if(roar){
                auto peak=player.Sample(650,powered);
                assert(peak[0]<.01f&&peak[1]==90&&peak[2]==90&&peak[4]==90);
                auto a=player.Sample(649,powered),b=player.Sample(651,powered);
                for(unsigned axis=0;axis<5;++axis)assert(std::abs(a[axis]-b[axis])<1);
            }
            // Evaluate the audible body, excluding the deliberate entry and
            // separate return. A loud call must not become a feeding gesture.
            unsigned matching=0,total=0;
            const auto body=player.body_duration();
            for(unsigned t=std::max(950U,body/4);t<body*9/10;t+=20){
                const auto q=player.Sample(t,powered);++total;
                matching+=roar?q[0]<=45:q[0]>=140;
            }
            assert(total>0&&matching*100/total>=90);
            const auto last=player.Sample(body-1,powered),out=player.Sample(body,powered);
            for(unsigned axis=0;axis<5;++axis)assert(std::abs(last[axis]-out[axis])<.05f);
            assert(player.Sample(player.duration(),powered)==rest&&!powered);
            if(roar){
                // A touch-triggered call may interrupt eating. It must take
                // longer to lift from the ground, with all axes synchronized.
                const SoundPose feeding{180,20,160,55,170};
                player.Begin(s,PetMechanism::kDinosaur,feeding,v*7919U);
                assert(player.body_duration()>=2300);
                auto before=player.Sample(0,powered);assert(before==feeding);
                for(unsigned t=5;t<=1350;t+=5){
                    const auto q=player.Sample(t,powered);
                    for(unsigned axis=0;axis<5;++axis){
                        assert(std::abs(q[axis]-before[axis])*200<=251);
                        const float progress=(q[0]-feeding[0])/(0-feeding[0]);
                        const SoundPose sky{0,90,90,150,90};
                        assert(std::abs(q[axis]-(feeding[axis]+(sky[axis]-feeding[axis])*progress))<.01f);
                    }
                    before=q;
                }
                assert(before[0]<.01f&&before[1]==90&&before[2]==90&&before[4]==90);
                auto a=player.Sample(player.body_duration()-1,powered);
                auto b=player.Sample(player.body_duration(),powered);
                for(unsigned axis=0;axis<5;++axis)assert(std::abs(a[axis]-b[axis])<.05f);
            }
        }
    }
    assert(roars==16&&chews==1);
}
''')

    def test_every_paired_sound_has_its_own_score_and_32_distinct_bounded_performances(self):
        for board in BOARDS:
            with self.subTest(board=board):
                motion_tests.WzhFluentMotionTest.run_cpp(self,
                    '#include "'+str(ROOT/'main/boards'/board/'wzh_user_pairs_catalog.inc')+'"\n'+
                    (reference_adapter() if board=='wzh_dinosaur' else '')+r'''
#include <cassert>
#include <cmath>
#include <set>
#include <string>
#include <iostream>
#include "wzh_sound_choreography.h"
using namespace wzh;
int main(){
    RegisterUserPairs(wzh_user_assets::kPairs,std::size(wzh_user_assets::kPairs));
    const bool dino=std::string("__BOARD__")=="wzh_dinosaur";
    const bool tail=std::string("__BOARD__")=="wzh_tailRedPanda";
    const SoundPose rest=dino?SoundPose{70,90,90,90,90}:tail?SoundPose{90,90,150,0,0}:SoundPose{90,90,90,0,0};
    const auto mechanism=dino?PetMechanism::kDinosaur:tail?PetMechanism::kTail:PetMechanism::kCrawl;
    SoundChoreography player;
    for(unsigned file=0;file<user_pair_count;++file){
        const auto& pair=user_pairs[file];
        assert(pair.score&&std::string(pair.score->name)==pair.name&&pair.score->duration_ms==pair.audio_ms);
        assert(pair.score->count==(pair.audio_ms+99)/100);
        bool nonzero=false;for(unsigned i=0;i<pair.score->count;++i)nonzero|=pair.score->envelope[i]>0;
        assert(nonzero);
        std::set<unsigned> choices;std::set<std::string> paths;
        for(unsigned turn=0;turn<32;++turn){
            SoundPlayback s;s.sound_index=kUserPairIndexBase+file;s.score=pair.score;
            s.kind=pair.score->kind;s.duration_ms=pair.audio_ms;
            __MATCH_REFERENCE__
            player.Begin(s,mechanism,rest,turn*7919U+file*104729U);
            assert(choices.insert(player.variant()).second);
            bool powered=false;std::string path;
            auto previous=player.Sample(0,powered);assert(previous==rest);
            const auto body=player.body_duration();
            for(unsigned t=5;t<player.duration();t+=5){
                const auto pose=player.Sample(t,powered);
                for(unsigned a=0;a<pair.axes;++a){
                    const float low=dino?(a==3?55:0):a==0?30:tail?(a==1?6:0):8;
                    const float high=dino?180:a==0?150:tail?(a==1?174:180):172;
                    assert(std::isfinite(pose[a])&&pose[a]>=low-.01f&&pose[a]<=high+.01f);
                    if(std::abs(pose[a]-previous[a])*200>=400)
                        std::cerr<<"SPEED pair="<<pair.name<<" t="<<t<<" a="<<a
                                 <<" prev="<<previous[a]<<" now="<<pose[a]<<"\n";
                    assert(std::abs(pose[a]-previous[a])*200<400);
                    if(t>body+5)assert(std::abs(pose[a]-previous[a])*200<=(pair.score->kind==SoundKind::kSleep?65:dino?100:tail?130:115)+.1f);
                    if(t<body&&t%80==0)path+=std::to_string(std::lround(pose[a]*10))+",";
                }
                previous=pose;
            }
            assert(player.Sample(player.duration(),powered)==rest&&!powered);
            if(!paths.insert(path).second){std::cerr<<"Duplicate "<<pair.name<<" style "<<player.variant()<<"\n";return 2;}
        }
        assert(choices.size()==32&&paths.size()==32);
    }
}
'''.replace('__BOARD__',board).replace('__MATCH_REFERENCE__',
    'MatchReference(player,s,turn*7919U+file*104729U);' if board=='wzh_dinosaur' else ''))

    def test_all_29_actual_reference_stories_resolve_keep_and_blend_smoothly(self):
        folder=ROOT/'main/boards/wzh_dinosaur'
        header=(folder/'dinosaur_controller.h').read_text()
        types=header[header.index('namespace wzh_dino {'):header.index('}  // namespace wzh_dino')+len('}  // namespace wzh_dino')]
        names=re.findall(r'constexpr Keyframe (k\w+)\[\]',(folder/'dinosaur_reference_motion.h').read_text())
        self.assertEqual(len(names),29)
        entries=',\n'.join('{"'+n+'",reference_motion::'+n+',std::size(reference_motion::'+n+')}' for n in names)
        motion_tests.WzhFluentMotionTest.run_cpp(self,'#include <cstdint>\n#include <iterator>\n'+types+
            '\n#include "'+str(folder/'dinosaur_reference_motion.h')+'"\n'+r'''
#include <cassert>
#include "wzh_sound_expression.h"
using namespace wzh;
const wzh_dino::MotionClip clips[]={__CLIPS__};
int main(){
    const SoundPose rest{70,90,90,90,90};
    const uint8_t envelope[]={0,0,0,0,0,255,255,255,255,255};
    const SoundScore score{"test",SoundKind::kCall,1000,7,envelope,10};
    for(const auto& clip:clips){
        SoundExpression e;e.SetReferenceClip(clip);
        for(unsigned variant=0;variant<32;++variant){
            e.Begin(PetMechanism::kDinosaur,&score,SoundKind::kCall,6000,variant,7,true);
            auto previous=e.Apply(rest,0);assert(previous==rest);
            float travelled=0;
            for(unsigned t=5;t<=6000;t+=5){
                const auto q=e.Apply(rest,t);
                for(unsigned a=0;a<5;++a){
                    assert(std::isfinite(q[a])&&q[a]>=(a==3?55:0)&&q[a]<=180);
                    assert(std::abs(q[a]-previous[a])*200<240);
                    travelled+=std::abs(q[a]-previous[a]);
                }
                previous=q;
            }
            assert(previous==rest&&travelled>40);
        }
    }
}
'''.replace('__CLIPS__',entries))

    def test_all_64_sources_keep_independent_shuffle_coverage(self):
        motion_tests.WzhFluentMotionTest.run_cpp(self,r'''
#include <array>
#include <cassert>
#include <set>
#include "wzh_sound_choreography.h"
using namespace wzh;
int main(){
    SoundVariantSelector<32> selector;
    std::array<SoundScore,32> scores{};
    std::array<int,64> previous;previous.fill(-1);
    for(unsigned round=0;round<2;++round){
        std::array<std::set<int>,64> seen;
        for(unsigned turn=0;turn<32;++turn)for(unsigned file=0;file<64;++file){
            SoundPlayback sound;sound.sound_index=file;sound.score=&scores[file%32];
            const auto chosen=selector.Draw(sound,file*104729U+turn*7919U+round*1234567U);
            assert(chosen!=previous[file]&&seen[file].insert(chosen).second);
            previous[file]=chosen;
        }
        for(const auto& variants:seen)assert(variants.size()==32);
    }
    SoundChoreography reference;
    for(unsigned file=0;file<64;++file){
        SoundPlayback sound;sound.sound_index=file;
        int previous=-1;
        for(unsigned round=0;round<3;++round){
            std::set<unsigned> seen;
            for(unsigned turn=0;turn<4;++turn){
                const auto choice=reference.ReferenceVariant(sound,turn*7919U+round,4);
                assert(int(choice)!=previous&&seen.insert(choice).second);
                previous=choice;
            }
        }
    }
}
''')
