"""Exercise the measured pack, production trajectories, fairness and sensor mapping."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT/'main/boards/common/wzh'


class WzhSoundScoresTest(unittest.TestCase):
    def run_cpp(self, source):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory)/'test.cc';cpp.write_text(source)
            exe = cpp.with_suffix('')
            result = subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-I',str(COMMON),str(cpp),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_all_65_files_32_variants_have_bounded_synchronized_trajectories(self):
        spec = importlib.util.spec_from_file_location('audit',ROOT/'scripts/audio/audit_wzh_choreography.py')
        module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        manifest, rows = module.audit()
        self.assertEqual(len(rows),2080)
        self.assertEqual(len(manifest),65)
        for file_index in range(len(manifest)):
            self.assertEqual({row['phrase_variant'] for row in rows if row['file'] == file_index},
                             set(range(1,33)))
        pandas = [{Path(f['path']).name:f['sha256'] for f in manifest if f['path'].startswith(product+'/')}
                  for product in ['crawling_panda','tail_wagging_panda']]
        self.assertEqual(pandas[0],pandas[1])

    def test_coverage_without_replacement_including_reorder_busy_and_panda_ambient_exclusion(self):
        self.run_cpp(r'''
#include <cassert>
#include <vector>
#include <algorithm>
#include <cstdio>
#include "wzh_sound_coverage.h"
struct File{char name[64];unsigned size=10;char category[16]="animal";};
int main(){
    std::vector<File> files;
    for(int i=0;i<32;++i){File f{};std::snprintf(f.name,64,"file%02d",i);files.push_back(f);}
    wzh::SoundCoverage bag;
    for(int cycle=0;cycle<4;++cycle){
        std::vector<std::string> chosen;
        for(int i=0;i<32;++i){
            int n=bag.Select(files,17U*i+unsigned(cycle));assert(n>=0);
            const std::string name=files[n].name;
            assert(std::find(chosen.begin(),chosen.end(),name)==chosen.end());
            assert(bag.Select(files,17U*i+unsigned(cycle))==n); // rejected queue consumes nothing
            bag.Remember(files,files[n].name);chosen.push_back(name);
            std::reverse(files.begin(),files.end());
        }
    }
    files={File{"call1"},File{"call2"},File{"long ambience",10,"ambient"}};
    wzh::SoundCoverage panda;
    int last=-1;
    for(int i=0;i<100;++i){int n=panda.Select(files,i,false);assert(n>=0&&n<2&&n!=last);
        panda.Remember(files,files[n].name,false);last=n;}
    // An external legacy writer may label every file animal (or omit categories).
    files={File{"call1"},File{"虫鸣鸟叫下雨流水声 (大自然的声音)",10,"animal"}};
    for(int i=0;i<20;++i){int n=panda.Select(files,i,false);assert(n==0);panda.Remember(files,files[n].name,false);}
    files.clear();assert(panda.Select(files,0)==-1);
}
'''.replace('#include <cstdio>','#include <cstdio>\n#include <string>'))

    def test_every_sensor_has_two_real_pack_files(self):
        manifest = json.loads((ROOT/'docs/wzh_sound_pack_2026-10-19.json').read_text())
        for product in ['crawling_panda','dinosaur']:
            entries = ['{'+json.dumps(f['name'],ensure_ascii=False)+'}' for f in manifest
                       if f['path'].startswith(product+'/animal/')]
            self.run_cpp(r'''
#include <cassert>
#include <vector>
#include <set>
#include "wzh_interaction_sound.h"
struct File{char name[64];unsigned size=100,duration_ms=1000;char category[16]="animal";};
int main(){
    const std::vector<File> files={__FILES__};
    for(unsigned event=1;event<=11;++event){
        std::set<int> choices;
        for(unsigned random=0;random<100;++random){
            wzh::InteractionSoundSelector selector;
            auto interaction=static_cast<wzh::PetInteraction>(event);
            int first=selector.Select(files,interaction,random);assert(first>=0);choices.insert(first);
            selector.Remember(files[first].name);
            int second=selector.Select(files,interaction,random);assert(second>=0&&second!=first);
        }
        assert(choices.size()==2);
    }
}
'''.replace('__FILES__',','.join(entries)))

    def test_pcm_wait_cancel_no_replay_and_completion_based_cadence_across_clock_wrap(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_sound_choreography.h"
using namespace wzh;
int main(){
    const SoundPose rest{90,90,150,0,0};SoundPose out{};bool powered=true;
    SoundPlayback sound;sound.playing=true;sound.sequence=1;
    sound.score=FindSoundScore("熊猫撒娇声音");sound.duration_ms=sound.score->duration_ms;
    sound.kind=sound.score->kind;
    SoundChoreography motion;
    assert(motion.Update(sound,PetMechanism::kTail,rest,100,out,powered)&&!powered);
    sound.audible=true;sound.elapsed_ms=600;
    assert(motion.Update(sound,PetMechanism::kTail,rest,700,out,powered)&&powered&&out[2]<100);
    motion.Cancel(sound.sequence);
    assert(!motion.Update(sound,PetMechanism::kTail,rest,720,out,powered));
    ++sound.sequence;assert(motion.Update(sound,PetMechanism::kTail,rest,740,out,powered));
    sound.playing=false;
    assert(!motion.Update(sound,PetMechanism::kTail,rest,6000,out,powered)||out==rest);
    AutoplayCadence cadence;uint32_t now=UINT32_MAX-5000;
    assert(!cadence.Due(now,true,0));
    assert(!cadence.Due(now+1799,true,0));assert(cadence.Due(now+1800,true,0));
    assert(!cadence.Due(now+10000,false,0));
    assert(!cadence.Due(now+11799,true,0));assert(cadence.Due(now+11800,true,0));
    uint8_t p60[]={24},p20[]={0x98},p120[]={25},bad[]={3,0};
    assert(OpusPacketDurationMs(p60,1)==60&&OpusPacketDurationMs(p20,1)==20);
    assert(OpusPacketDurationMs(p120,1)==120);
    assert(OpusPacketDurationMs(bad,2)==0&&OpusPacketDurationMs(nullptr,0)==0);
}
''')

    def test_decoder_reset_rejects_waiting_and_late_packets_from_previous_local_sound(self):
        from test_wzh_action_queue import queue_function
        source = (ROOT/'main/audio/audio_service.cc').read_text()
        method = queue_function(source, 'bool AudioService::PushPacketToDecodeQueue(')
        self.run_cpp(r'''
#include <cassert>
#include <atomic>
#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <cstdint>
struct AudioStreamPacket{};
constexpr unsigned MAX_DECODE_PACKETS_IN_QUEUE=1;
struct AudioService{
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>>audio_decode_queue_;
    std::atomic<bool>service_stopped_{false};
    bool playback_drained_notified_=true;
    uint32_t playback_generation_=7,decode_queue_full_count_=0;
    size_t max_decode_packets_=MAX_DECODE_PACKETS_IN_QUEUE;
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>,bool,
        std::optional<uint32_t> expected_generation=std::nullopt);
};
'''+method+r'''
int main(){
    AudioService service;
    assert(service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),false,7));
    bool accepted=true;
    std::thread producer([&]{accepted=service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),true,7);});
    {
        std::lock_guard<std::mutex>lock(service.audio_queue_mutex_);
        ++service.playback_generation_;
        service.audio_decode_queue_.clear();
        service.audio_queue_cv_.notify_all();
    }
    producer.join();assert(!accepted&&service.audio_decode_queue_.empty());
    assert(!service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),false,7));
    assert(service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),false,8));
    service.audio_decode_queue_.clear();
    // Existing WebSocket/MQTT/TTS producers keep their default call contract.
    assert(service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),false));
}
''')

    def test_motion_clock_waits_for_actual_pcm_and_does_not_follow_decode_ahead(self):
        from test_wzh_action_queue import queue_function
        source=(ROOT/'main/boards/common/wzh/wzh_audio_player.cc').read_text()
        methods='\n'.join(queue_function(source, marker) for marker in [
            'MotionData AudioPlayer::GetMotionData(', 'void AudioPlayer::FeedTtsLevel('])
        self.run_cpp(r'''
#include <atomic>
#include <array>
#include <cassert>
#include "wzh_sound_score.h"
using namespace wzh;
using MotionData=SoundPlayback;
int64_t now=1000000;
int64_t esp_timer_get_time(){return now;}
struct AudioPlayer{
    std::atomic<bool>maintenance_{false},worker_active_{false};
    std::array<char,64>current_name_{};
    std::atomic<bool>is_playing_{true};
    std::atomic<float>tts_level_{0},tts_attack_{0};
    std::atomic<uint32_t>motion_duration_ms_{2000},motion_sequence_{1};
    std::atomic<int>motion_sound_index_{3},local_packet_ms_{60};
    std::atomic<const SoundScore*>motion_score_{FindSoundScore("Cute_baby_panda_voca")};
    std::atomic<SoundKind>motion_kind_{SoundKind::kCall};
    std::atomic<int64_t>local_pcm_start_us_{0},tts_updated_us_{0};
    MotionData GetMotionData()const;
    void FeedTtsLevel(float,float);
};
'''+methods+r'''
int main(){
    AudioPlayer player;
    now+=5000000;auto pending=player.GetMotionData();
    assert(pending.playing&&!pending.audible&&pending.elapsed_ms==0);
    player.FeedTtsLevel(.2f,.4f);
    auto first=player.GetMotionData();assert(first.audible&&first.elapsed_ms==60);
    assert(first.duration_ms==2000&&first.sequence==1&&first.score==player.motion_score_);
    now+=500000;player.FeedTtsLevel(.3f,0);
    assert(player.GetMotionData().elapsed_ms==560); // Later packets do not restart the epoch.
    player.local_pcm_start_us_=0;player.local_packet_ms_=20;player.motion_sequence_=2;
    now+=1000000;player.FeedTtsLevel(.1f,.1f);
    assert(player.GetMotionData().elapsed_ms==20&&player.GetMotionData().sequence==2);
    player.is_playing_=false;player.local_pcm_start_us_=0;player.FeedTtsLevel(.5f,.5f);
    assert(!player.GetMotionData().audible); // Unrelated TTS cannot start a local score.
}
''')
