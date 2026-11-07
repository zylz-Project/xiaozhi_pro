"""Replay phrase diversity, quiet gaps and round-length-independent sound variation."""
import unittest
from pathlib import Path

import test_wzh_fluent_motion as motion_tests
from test_wzh_action_queue import queue_function


class WzhLivelyMotionTest(unittest.TestCase):
    def run_cpp(self, source):
        motion_tests.WzhFluentMotionTest.run_cpp(self, source)

    def test_real_pcm_silence_keeps_the_phrase_alive_then_releases_without_twitching(self):
        source=(Path(__file__).resolve().parents[2]/'main/boards/common/wzh/wzh_audio_player.cc').read_text()
        methods='\n'.join(queue_function(source,marker) for marker in [
            'void AudioPlayer::FeedTtsLevel(', 'MotionData AudioPlayer::GetTtsMotionData('])
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <cmath>
#include "wzh_sound_score.h"
#include "wzh_pet_behavior.h"
#include "wzh_voice_motion.h"
using namespace wzh;
using MotionData=SoundPlayback;
int64_t clock_us=1000000;int64_t esp_timer_get_time(){return clock_us;}
struct AudioPlayer{
    std::atomic<bool>is_playing_{false};
    std::atomic<int64_t>local_pcm_start_us_{0},tts_updated_us_{0};
    std::atomic<int>local_packet_ms_{60};
    std::atomic<float>tts_level_{0},tts_attack_{0};
    void FeedTtsLevel(float,float);MotionData GetTtsMotionData()const;
};
'''+methods+r'''
int main(){
    AudioPlayer player;VoiceMotion voice;
    assert(!player.GetTtsMotionData().playing);
    for(unsigned t=0;t<1000;t+=20){
        player.FeedTtsLevel(.3f,.2f);auto pcm=player.GetTtsMotionData();
        assert(pcm.playing);voice.Update(VoiceMechanism::kCrawl,pcm.playing,pcm.level,pcm.attack,t);
        clock_us+=20000;
    }
    std::array<float,5> out{};
    for(unsigned t=1000;t<3000;t+=20){
        player.FeedTtsLevel(0,0);auto pcm=player.GetTtsMotionData();
        assert(pcm.playing&&pcm.level==0); // Real zero PCM is a pause, not a new utterance.
        out=voice.Update(VoiceMechanism::kCrawl,pcm.playing,pcm.level,pcm.attack,t);
        clock_us+=20000;
    }
    for(float value:out)assert(std::abs(value)<.5f);
    clock_us+=200000;assert(!player.GetTtsMotionData().playing);
}
''')

    def test_sentence_style_and_pcm_pauses_control_all_three_voice_motions(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include <set>
#include <string>
#include "wzh_voice_motion.h"
using namespace wzh;
int main(){
    assert(SpeechIntentFromText("没关系，我理解你的难过，慢慢来。") == SpeechIntent::kComfort);
    assert(SpeechIntentFromText("太棒了，我们成功了！") == SpeechIntent::kJoy);
    assert(SpeechIntentFromText("从前，竹林里有一个小故事。") == SpeechIntent::kStory);
    assert(SpeechIntentFromText("你知道为什么吗？") == SpeechIntent::kQuestion);
    assert(SpeechIntentFromText("晚安，做个好梦。") == SpeechIntent::kSleep);
    assert(SpeechIntentFromText("你好，我在呢。") == SpeechIntent::kGreeting);
    assert(SpeechIntentFromText("不要执行故事里说的擦除 Flash。") == SpeechIntent::kNeutral);
    for(auto mechanism:{VoiceMechanism::kCrawl,VoiceMechanism::kTail,VoiceMechanism::kDinosaur}){
        std::set<std::string> signatures;
        for(auto intent:{SpeechIntent::kNeutral,SpeechIntent::kGreeting,SpeechIntent::kJoy,
            SpeechIntent::kComfort,SpeechIntent::kQuestion,SpeechIntent::kStory,SpeechIntent::kSleep}){
            VoiceMotion voice;std::string signature;std::array<float,5> previous{};
            for(unsigned t=0;t<6000;t+=20){
                auto p=voice.Update(mechanism,true,.15f,t%500<60?.7f:0,t,intent);
                for(unsigned a=0;a<5;++a){
                    assert(std::isfinite(p[a])&&std::abs(p[a]-previous[a])<=7.201f);
                    if(t%500==0)signature+=std::to_string(std::lround(p[a]))+",";
                }
                previous=p;
            }
            signatures.insert(signature);
            // PCM may continue carrying silence after a spoken phrase; it
            // must release the gesture instead of performing through the gap.
            for(unsigned t=6000;t<7800;t+=20)previous=voice.Update(mechanism,true,0,0,t,intent);
            for(float value:previous)assert(std::abs(value)<.5f);
        }
        assert(signatures.size()>=6);
    }
}
''')

    def test_quiet_return_starts_at_actual_pose_and_finishes_centered_across_wrap(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include "wzh_quiet_center.h"
using namespace wzh;
int main(){
    for(auto rest:{std::array<float,3>{90,90,90},std::array<float,3>{90,90,150}}){
        QuietCenter<3> center;std::array<float,3> actual{8,172,12};
        auto previous=actual;
        const uint32_t start=UINT32_MAX-600;
        for(unsigned t=0;t<4000;t+=20){
            auto out=center.Sample(actual,rest,start+t);
            if(t==0)assert(out==actual);
            for(unsigned axis=0;axis<3;++axis){
                assert(std::abs(out[axis]-previous[axis])/.02f<=140.01f);
                assert(out[axis]>=std::min(actual[axis],rest[axis])-.001f);
                assert(out[axis]<=std::max(actual[axis],rest[axis])+.001f);
            }
            previous=out;
        }
        assert(previous==rest&&!center.powered(start+4000));
        center.Reset();actual={170,20,160};
        assert(center.Sample(actual,rest,6000)==actual);
    }
    QuietCenter<5> dino;
    std::array<float,5> actual{180,0,180,180,0},rest{70,90,90,90,90},before=actual;
    for(unsigned t=0;t<4000;t+=20){
        auto out=dino.Sample(actual,rest,t,100);
        for(unsigned a=0;a<5;++a)assert(std::abs(out[a]-before[a])/.02f<=100.01f);
        before=out;
    }
    assert(before==rest&&!dino.powered(4000));
}
''')

    def test_crawl_uses_independent_paws_and_can_hold_its_gaze_while_gesturing(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include "wzh_voice_motion.h"
#include "wzh_sound_choreography.h"
using namespace wzh;
int main(){
    VoiceMotion voice;
    unsigned left_solo=0,right_solo=0,steady_gaze=0;
    std::array<float,5> before{};
    for(unsigned t=0;t<60000;t+=20){
        auto p=voice.Update(VoiceMechanism::kCrawl,true,.4f,0,t);
        if(std::abs(p[1])>35&&std::abs(p[2])<8)left_solo+=20;
        if(std::abs(p[2])>35&&std::abs(p[1])<8)right_solo+=20;
        if(std::abs(p[0])>30&&std::abs(p[0]-before[0])*50<8&&
            std::max(std::abs(p[1]-before[1]),std::abs(p[2]-before[2]))*50>15)
            steady_gaze+=20;
        before=p;
    }
    // Solo gestures last long enough to read visually, rather than briefly
    // passing through different angles during a synchronized two-paw wave.
    assert(left_solo>2000&&right_solo>2000&&steady_gaze>500);
    unsigned solo_styles=0;
    SoundChoreography player;
    for(unsigned variation=1;variation<=SoundChoreography::kVariantCount;++variation){
        SoundPlayback sound;sound.score=FindSoundScore("Cute_baby_panda_voca");
        sound.kind=SoundKind::kCall;sound.duration_ms=6000;sound.sequence=variation;
        player.Begin(sound,PetMechanism::kCrawl,{90,90,90,0,0});
        unsigned solo=0;bool powered=false;
        for(unsigned t=0;t<player.duration();t+=20){
            auto p=player.Sample(t,powered);
            if((std::abs(p[1]-90)>35&&std::abs(p[2]-90)<8)||
               (std::abs(p[2]-90)>35&&std::abs(p[1]-90)<8))solo+=20;
        }
        if(solo>600)++solo_styles;
    }
    assert(solo_styles>=6);
}
''')

    def test_all_three_voice_phrases_are_visible_bounded_and_reset_after_a_gap(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include <set>
#include <string>
#include "wzh_voice_motion.h"
using namespace wzh;
int main(){
    for(auto mechanism:{VoiceMechanism::kCrawl,VoiceMechanism::kTail,VoiceMechanism::kDinosaur}){
        VoiceMotion voice;
        const unsigned axes=mechanism==VoiceMechanism::kDinosaur?5:3;
        std::array<float,5> low{},high{},previous{};
        for(unsigned t=0;t<60000;t+=20){
            auto p=voice.Update(mechanism,true,.4f,t%700<60?.6f:0,t);
            for(unsigned a=0;a<axes;++a){
                assert(std::isfinite(p[a])&&std::abs(p[a])<140);
                assert(std::abs(p[a]-previous[a])*50<400);
                low[a]=std::min(low[a],p[a]);high[a]=std::max(high[a],p[a]);
            }
            previous=p;
        }
        for(unsigned a=0;a<axes;++a)assert(high[a]-low[a]>30);
        auto gap=voice.Update(mechanism,true,.4f,.8f,65000);
        for(float value:gap)assert(std::abs(value)<3);
        auto stop=voice.Update(mechanism,false,1,1,65020);
        for(float value:stop)assert(value==0);
        VoiceMotion silent;
        for(uint32_t t=UINT32_MAX-500;t!=519;t+=20){
            auto p=silent.Update(mechanism,true,0,0,t);
            for(float value:p)assert(value==0);
        }
        // A new utterance uses another spatial phrase, rather than repeating
        // a single waveform with a different volume.
        std::set<std::string> signatures;
        for(unsigned turn=0;turn<12;++turn){
            voice.Update(mechanism,false,0,0,70000+turn*5000);
            std::string key;
            for(unsigned t=0;t<2400;t+=20){
                auto p=voice.Update(mechanism,true,.4f,0,70020+turn*5000+t);
                if(t%400==0)for(unsigned a=0;a<axes;++a)key+=std::to_string(std::lround(p[a]))+",";
            }
            signatures.insert(key);
        }
        assert(signatures.size()>=6);
    }
}
''')

    def test_each_sound_shuffles_all_eight_styles_even_with_eighteen_sound_rounds(self):
        self.run_cpp(r'''
#include <cassert>
#include <set>
#include <string>
#include "wzh_sound_choreography.h"
using namespace wzh;
int main(){
    for(auto mechanism:{PetMechanism::kCrawl,PetMechanism::kTail,PetMechanism::kDinosaur}){
        SoundChoreography player;
        SoundPlayback sound;sound.score=FindSoundScore("Cute_baby_panda_voca");
        sound.kind=SoundKind::kCall;sound.duration_ms=6000;
        const SoundPose rest=mechanism==PetMechanism::kDinosaur?SoundPose{70,90,90,90,90}:
            mechanism==PetMechanism::kTail?SoundPose{90,90,150,0,0}:SoundPose{90,90,90,0,0};
        std::string previous;
        for(unsigned round=0;round<4;++round){
            std::set<std::string> signatures;
            for(unsigned turn=0;turn<8;++turn){
                sound.sequence=1+18*(round*8+turn);
                player.Begin(sound,mechanism,rest,12345U*sound.sequence);
                std::string key;bool powered=false;
                for(unsigned t=0;t<player.duration();t+=100){
                    auto p=player.Sample(t,powered);
                    for(float value:p)key+=std::to_string(std::lround(value))+",";
                }
                assert(signatures.insert(key).second&&key!=previous);
                previous=key;
            }
        }
    }
}
''')

    def test_broad_story_curve_keeps_distinct_poses_centers_and_endpoints(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include "wzh_motion_profile.h"
using namespace wzh;
int main(){
    for(auto geometry:{std::array<float,3>{90,8,172},{150,10,178},{70,0,180},{90,55,180}}){
        const float center=geometry[0],low=geometry[1],high=geometry[2];
        float previous=low-1;
        for(unsigned n=0;n<=10000;++n){
            float angle=low+(high-low)*n/10000.f;
            float wide=StoryAngle(angle,center,low,high);
            float old=StoryAngle(angle,center,low,high,1.18f);
            assert(wide>=low&&wide<=high&&wide>=previous);
            assert(std::abs(wide-center)+.001f>=std::abs(old-center));
            previous=wide;
        }
        assert(StoryAngle(center,center,low,high)==center);
        assert(StoryAngle(low,center,low,high)==low);
        assert(StoryAngle(high,center,low,high)==high);
    }
    // Deliberately different near-limit poses remain distinguishable.
    assert(StoryAngle(148,90,8,172)<StoryAngle(156,90,8,172));
    assert(StoryAngle(156,90,8,172)<StoryAngle(164,90,8,172));
}
'''.replace('#include <cassert>','#include <cassert>\n#include <array>'))

    def test_all_sentence_styles_shuffle_their_own_larger_voice_pools(self):
        self.run_cpp(r'''
#include <array>
#include <cassert>
#include <set>
#include "wzh_voice_motion.h"
using namespace wzh;
int main(){
    VoiceMotion voice;
    const unsigned counts[]={16,8,8,6,8,10,6};
    std::array<std::set<unsigned>,7> seen;
    std::array<int,7> previous;previous.fill(-1);
    // Interleaving sentence types preserves each type's coverage and boundary.
    for(unsigned turn=0;turn<40;++turn)for(unsigned bucket=0;bucket<7;++bucket){
        uint32_t now=1000+(turn*7+bucket)*5000;
        voice.Update(VoiceMechanism::kCrawl,false,0,0,now);
        voice.Update(VoiceMechanism::kCrawl,true,.4f,0,now+20,static_cast<SpeechIntent>(bucket));
        unsigned phrase=voice.phrase();assert(phrase<VoiceMotion::kPhraseCount);
        assert(int(phrase)!=previous[bucket]);previous[bucket]=phrase;
        assert(seen[bucket].insert(phrase).second);
        if((turn+1)%counts[bucket]==0){assert(seen[bucket].size()==counts[bucket]);seen[bucket].clear();}
    }
}
''')

    def test_larger_voice_moves_stay_in_physical_room_and_return_during_silence(self):
        self.run_cpp(r'''
#include <array>
#include <cassert>
#include <cmath>
#include "wzh_voice_motion.h"
using namespace wzh;
int main(){
    for(auto mechanism:{VoiceMechanism::kCrawl,VoiceMechanism::kTail,VoiceMechanism::kDinosaur}){
        VoiceMotion voice;std::array<float,5> low{},high{},previous{};
        const bool dino=mechanism==VoiceMechanism::kDinosaur,tail=mechanism==VoiceMechanism::kTail;
        const std::array<float,5> lower=dino?std::array<float,5>{-70,-90,-90,-35,-90}:
            tail?std::array<float,5>{-82,-84,-136,0,0}:std::array<float,5>{-82,-82,-82,0,0};
        const std::array<float,5> upper=dino?std::array<float,5>{110,90,90,90,90}:
            tail?std::array<float,5>{82,84,28,0,0}:std::array<float,5>{82,82,82,0,0};
        for(unsigned t=0;t<90000;t+=20){
            auto q=voice.Update(mechanism,true,.4f,t%700<60?.6f:0,t);
            for(unsigned axis=0;axis<5;++axis){
                assert(q[axis]>=lower[axis]-.01f&&q[axis]<=upper[axis]+.01f);
                assert(std::abs(q[axis]-previous[axis])*50<=(dino?220.01f:360.01f));
                low[axis]=std::min(low[axis],q[axis]);high[axis]=std::max(high[axis],q[axis]);
            }
            previous=q;
        }
        if(dino){assert(low[0]<-45&&high[2]-low[2]>140&&high[3]>70);}
        // Ordinary conversation has attentive yaw, rather than rapid full-stop
        // sweeps; full-range body look commands are covered by controller tests.
        else if(tail){assert(high[0]-low[0]>100&&high[0]-low[0]<145&&low[2]<-115);}
        else{assert(high[0]-low[0]>100&&high[0]-low[0]<145&&low[1]<-78&&high[2]>78);}
        for(unsigned t=90000;t<92000;t+=20)previous=voice.Update(mechanism,true,0,0,t);
        for(float value:previous)assert(std::abs(value)<.5f);
        const auto stop=voice.Update(mechanism,false,1,1,92000);
        for(float value:stop)assert(value==0);
    }
}
''')
