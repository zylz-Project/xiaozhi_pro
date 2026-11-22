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
    def test_authored_roar_reaches_both_sides_and_low_poses_with_flash_and_paired_audio(self):
        folder=ROOT/'main/boards/wzh_dinosaur'
        source=(folder/'dinosaur_controller.cc').read_text()
        start=source.index('constexpr Keyframe kRoarFrontFrames[]')
        end=source.index('constexpr Keyframe kSoundEatNibbleFrames[]',start)
        motion_tests.WzhFluentMotionTest.run_cpp(self,
            '#include "'+str(folder/'wzh_user_pairs_catalog.inc')+'"\n'+
            reference_adapter()+r'''
#include <cassert>
using namespace wzh;
using namespace wzh_dino;
constexpr unsigned kMaxClipFrames=16;
constexpr int SERVO_TAIL_UD_MIN=55,SERVO_TAIL_UD_MAX=180;
'''+source[start:end]+r'''
int main(){
    RegisterUserPairs(wzh_user_assets::kPairs,std::size(wzh_user_assets::kPairs));
    constexpr SoundPose rest{70,90,90,90,90};
    SoundPlayback shortest,medium,longest;
    shortest.duration_ms=UINT32_MAX;
    for(unsigned file=0;file<user_pair_count;++file){
        const auto& pair=user_pairs[file];
        if(pair.score->kind!=SoundKind::kRoar)continue;
        SoundPlayback sound;
        sound.kind=SoundKind::kRoar;sound.duration_ms=pair.audio_ms;
        sound.sound_index=kUserPairIndexBase+file;sound.score=pair.score;
        if(sound.duration_ms<shortest.duration_ms)shortest=sound;
        if(sound.duration_ms>longest.duration_ms)longest=sound;
        if(sound.duration_ms==3680)medium=sound;
    }
    assert(shortest.duration_ms<1500&&medium.duration_ms==3680&&longest.duration_ms>=4500);
    SoundChoreography player;
    for(const bool paired:{false,true}){
        auto sound=shortest;
        if(!paired)sound.sound_index=0;
        bool powered=false;
        for(unsigned clip=12;clip<=14;++clip){
            player.SetRoarClip(kSkyRoarClips[clip],0,true);
            player.Begin(sound,PetMechanism::kDinosaur,rest);
            const auto onset=player.Sample(clip==14?750:600,powered);
            assert(powered);
            if(clip==12)assert(onset[1]<=78&&onset[2]<=66);
            if(clip==13)assert(onset[1]>=103&&onset[2]>=115);
            if(clip==14)assert(onset[0]>=95);
        }
        sound=medium;
        if(!paired)sound.sound_index=0;
        for(unsigned clip:{15U,18U}){
            player.SetRoarClip(kSkyRoarClips[clip],0,true);
            player.Begin(sound,PetMechanism::kDinosaur,rest);
            bool deep=false,lean_left=false,lean_right=false;
            for(uint32_t t=0;t<sound.duration_ms;t+=10){
                const auto p=player.Sample(t,powered);
                deep|=p[0]>=135;
                lean_left|=p[1]>=108&&p[2]>=117;
                lean_right|=p[1]<=72&&p[2]<=70;
            }
            assert(lean_left&&lean_right);
            if(clip==15)assert(deep);
        }
        sound=longest;
        if(!paired)sound.sound_index=0;
        player.SetRoarClip(kSkyRoarClips[16],0,true);
        player.Begin(sound,PetMechanism::kDinosaur,rest);
        bool high=false,low=false,left=false,right=false;
        auto before=player.Sample(0,powered);
        assert(before==rest);
        for(uint32_t t=10;t<player.duration();t+=10){
            const auto p=player.Sample(t,powered);
            if(t<sound.duration_ms){
                high|=p[0]<=48;low|=p[0]>=94;
                left|=p[2]>=113;right|=p[2]<=65;
            }
            for(unsigned a=0;a<p.size();++a){
                assert(p[a]>=(a==3?55:0)&&p[a]<=180);
                assert(std::abs(p[a]-before[a])<=3);
            }
            before=p;
        }
        assert(high&&low&&left&&right);
        assert(player.Sample(player.duration(),powered)==rest&&!powered);
    }

    // Distinct recorded attacks move the same authored landmark within a
    // bounded window. A silent score keeps the duration-only timing.
    std::array<uint8_t,36> early{},late{};
    early[12]=200;late[14]=200;
    const SoundScore early_score{"early",SoundKind::kRoar,3600,0,early.data(),early.size()};
    const SoundScore late_score{"late",SoundKind::kRoar,3600,0,late.data(),late.size()};
    SoundPlayback cue;cue.kind=SoundKind::kRoar;cue.duration_ms=3600;
    cue.score=&early_score;
    player.Begin(cue,PetMechanism::kDinosaur,rest);
    bool powered=false;
    assert(std::abs(player.Sample(1200,powered)[2]-147)<.01f);
    cue.score=&late_score;
    player.Begin(cue,PetMechanism::kDinosaur,rest);
    assert(player.Sample(1200,powered)[2]<146);
    assert(std::abs(player.Sample(1400,powered)[2]-147)<.01f);
}
''', optimize=True)

    def test_dinosaur_flash_roars_keep_authored_sweeps_in_short_and_medium_recordings(self):
        motion_tests.WzhFluentMotionTest.run_cpp(self, reference_adapter()+r'''
#include <cassert>
using namespace wzh;
int main(){
    constexpr wzh_dino::Keyframe frames[]={
        {0,90,90,155,90,900,wzh_dino::MotionEase::kSmooth},
        {0,90,90,155,90,300,wzh_dino::MotionEase::kLinear},
        {8,124,145,160,118,650,wzh_dino::MotionEase::kSmooth},
        {18,56,35,146,66,1050,wzh_dino::MotionEase::kSmooth},
        {22,90,90,131,90,650,wzh_dino::MotionEase::kSmooth},
    };
    const wzh_dino::MotionClip clip{"test-sweep",frames,std::size(frames)};
    constexpr SoundPose rest{70,90,90,90,90};
    SoundPlayback sound;
    sound.kind=SoundKind::kRoar;
    sound.sound_index=0;
    SoundChoreography player;
    for(uint32_t duration:{1600U,2300U,3800U}){
        sound.duration_ms=duration;
        for(unsigned run=0;run<SoundChoreography::kVariantCount;++run){
            player.SetRoarClip(clip);
            player.Begin(sound,PetMechanism::kDinosaur,rest,run*7919U);
            bool powered=false,up=false,left=false,right=false;
            for(uint32_t at=0;at<player.duration();at+=10){
                const auto pose=player.Sample(at,powered);
                up|=pose[0]<1&&std::abs(pose[1]-90)<1&&std::abs(pose[2]-90)<1;
                if(up&&pose[0]<=42){
                    left|=pose[2]<=65;
                    right|=pose[2]>=115;
                }
            }
            assert(up&&(left||right));
            if(duration>=2300)assert(left&&right);
            assert(player.Sample(player.duration(),powered)==rest&&!powered);
        }
    }
    sound.kind=SoundKind::kSearch;
    sound.duration_ms=2000;
    for(unsigned run=0;run<8;++run){
        player.Begin(sound,PetMechanism::kDinosaur,rest,run*7919U);
        bool powered=false,observed_ground=false;
        for(uint32_t at=0;at<player.duration();at+=20)
            observed_ground|=player.Sample(at,powered)[0]>=125;
        assert(observed_ground);
    }
}
''', optimize=True)

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
''', optimize=True)

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
    'MatchReference(player,s,turn*7919U+file*104729U);' if board=='wzh_dinosaur' else ''),
                    optimize=True)

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
