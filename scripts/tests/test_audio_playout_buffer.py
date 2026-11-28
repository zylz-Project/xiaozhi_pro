"""Downlink jitter admission is bounded and does not pause every audio packet."""
from pathlib import Path
import unittest

import test_wzh_fluent_motion as motion_tests
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT/'main/audio/audio_playout_buffer.h'


class AudioPlayoutBufferTest(unittest.TestCase):
    def test_prefix_timeout_i2s_horizon_gap_rebuffer_and_generation_reset(self):
        motion_tests.WzhFluentMotionTest.run_cpp(self,
            '#include "'+str(HEADER)+'"\n'+r'''
#include <cassert>
int main(){
    AudioPlayoutBuffer buffer;
    assert(buffer.WaitMs(1,20,1000)==0); // unchanged non-robot behavior
    buffer.Enable(true);
    assert(buffer.WaitMs(2,60,1000)==80);
    assert(buffer.WaitMs(2,60,1030)==50);
    assert(buffer.WaitMs(2,120,1040)==0);
    buffer.Written(60,1040);buffer.Written(60,1040);
    // DMA still contains sound while the software queue briefly empties.
    assert(buffer.WaitMs(2,0,1080)==0);
    assert(buffer.WaitMs(2,60,1100)==0);
    buffer.Written(60,1100);
    assert(buffer.WaitMs(2,60,1180)==0);
    buffer.Written(60,1180);
    assert(buffer.rebuffer_count()==0);
    assert(buffer.WaitMs(2,20,1310)==80);
    assert(buffer.WaitMs(2,20,1389)==1);
    assert(buffer.WaitMs(2,20,1390)==0); // a short prompt is never stuck
    assert(buffer.rebuffer_count()==1);
    assert(buffer.WaitMs(3,60,2000)==80);
    assert(buffer.WaitMs(4,60,2040)==80); // cancellation restarts prefix
    assert(buffer.WaitMs(4,60,2119)==1);
    assert(buffer.WaitMs(4,60,2120)==0);
    buffer.ConfigureExternalSpeech(5);
    assert(buffer.WaitMs(5,20,3000)==1050);
    assert(buffer.WaitMs(5,100,3030)==1020);
    assert(buffer.WaitMs(5,850,3030)==0);
    buffer.Written(40,3030);
    assert(buffer.first_output_ms()==3030);
    assert(buffer.WaitMs(5,20,3200)==1350);
    assert(buffer.WaitMs(5,950,3240)==1310);
    assert(buffer.WaitMs(5,1080,3250)==0);
    buffer.Written(20,3250);
    assert(buffer.gap_ms()==180);
    assert(buffer.WaitMs(5,20,3350)==1350);
    buffer.FinishSpeech(4);
    assert(buffer.WaitMs(5,20,3360)==1340);
    buffer.FinishSpeech(5);
    assert(buffer.WaitMs(5,20,3360)==0);
    buffer.Written(20,3360);
    assert(buffer.gap_ms()==270);
    buffer.ConfigureExternalSpeech(5);
    assert(buffer.WaitMs(5,20,3500)==1050);
    assert(buffer.first_output_ms()==0);
    assert(buffer.WaitMs(6,60,3300)==80);
    buffer.Enable(false);
    assert(buffer.WaitMs(7,1,3400)==0);
}
''')

    def test_tail_prefills_network_audio_and_caps_the_dma_horizon(self):
        motion_tests.WzhFluentMotionTest.run_cpp(self,
            '#define CONFIG_BOARD_TYPE_WZH_TAILREDPANDA 1\n'
            '#include "'+str(HEADER)+'"\n'+r'''
#include <cassert>
int main(){
    AudioPlayoutBuffer buffer;
    buffer.Enable(true);
    buffer.ConfigureExternalSpeech(1);
    AudioPlayoutBuffer::NetworkSupply supply{60, 1200, false, false, false, false};
    assert(buffer.WaitForNetworkPcm(1,supply,1000)==1050); // Opus alone isn't playable.
    supply.pcm_ms=240;
    assert(buffer.WaitForNetworkPcm(1,supply,1120)==0);
    // Repeated quick writes cannot imply seconds of physical DMA backlog.
    for(int i=0;i<10;++i) buffer.Written(20,1120,1120,60);
    supply.pcm_ms=60;
    assert(buffer.WaitForNetworkPcm(1,supply,1240)==0); // Ready PCM, no artificial refill.
    buffer.Written(60,1240,1325,60);
    assert(buffer.rebuffer_count()==0&&buffer.late_write_count()==1);
    supply.waited_for_pcm=true;
    supply.decode_work_pending=true;
    assert(buffer.WaitForNetworkPcm(1,supply,1340)==320);
    supply.pcm_ms=240;
    assert(buffer.WaitForNetworkPcm(1,supply,1400)==0);
    buffer.Written(60,1400,1460,60);
    assert(buffer.rebuffer_count()==1&&buffer.gap_ms()==100);
    assert(buffer.decode_pending_rebuffers()==1);
    assert(buffer.long_write_count()==0&&buffer.longest_write_ms()==85);
    buffer.Written(20,1470,1530,60);
    assert(buffer.long_write_count()==1);
    buffer.RecordShortWrite(1);
    assert(buffer.short_write_count()==1);
    buffer.ConfigureServerSpeech(2);
    supply={120, 120, true, false, false, false};
    assert(buffer.WaitForNetworkPcm(2,supply,2000)==0); // 20 ms frames can fill queue.
    assert(buffer.short_write_count()==0&&buffer.late_write_count()==0);
    buffer.ConfigureExternalSpeech(3);
    supply={60, 60, false, false, false, false};
    assert(buffer.WaitForNetworkPcm(3,supply,3200)==1050);
    buffer.FinishSpeech(3);
    assert(buffer.WaitForNetworkPcm(3,supply,3210)==1040); // Decoder still working.
    supply.decoding_finished=true;
    assert(buffer.WaitForNetworkPcm(3,supply,3220)==0); // Short speech drains immediately.
    assert(buffer.WaitMs(4,60,3400)==80); // a local cue stays responsive
}
''')

    def test_output_prefix_wait_can_be_cancelled_without_speaker_or_motion(self):
        method=queue_function((ROOT/'main/audio/audio_service.cc').read_text(),
                              'void AudioService::AudioOutputTask(')
        motion_tests.WzhFluentMotionTest.run_cpp(self,
            '#include "'+str(HEADER)+'"\n'+r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#define ESP_LOGW(...) ((void)0)
constexpr unsigned AUDIO_POWER_CHECK_INTERVAL_MS=1000;
void esp_timer_stop(void*){}void esp_timer_start_periodic(void*,unsigned){}
struct AudioTask{uint32_t playback_generation=7,timestamp=0;std::vector<int16_t>pcm=std::vector<int16_t>(960,1000);};
struct Codec{
 void ResetOutputProcessing(){}
 int writes=0;bool enabled=false;
 int output_sample_rate(){return 16000;}
 bool output_enabled(){return enabled;}void EnableOutput(bool e){enabled=e;}
 size_t OutputData(std::vector<int16_t>& data){++writes;return data.size();}
};
struct AudioService{
 Codec codec;Codec*codec_=&codec;void*audio_power_timer_=nullptr;
 std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<AudioTask>audio_playback_queue_;
 std::atomic<bool>service_stopped_{false};uint32_t playback_generation_=7;
 std::atomic<bool>waiting{false};AudioPlayoutBuffer playout_buffer_;
 uint32_t BufferedPlaybackMsLocked(){waiting=true;return 60;}
 bool buffered_playback_=true;
 bool output_in_flight_=false;float output_fast_env_=0,output_slow_env_=0;
 std::function<void(float,float)>output_audio_level_listener_;
 std::chrono::steady_clock::time_point last_output_time_;
 struct{int playback_count=0;}debug_statistics_;
 struct{std::function<void()>on_playback_drained;}callbacks_;
 uint32_t GetPlaybackGeneration(){return playback_generation_;}
 bool MarkPlaybackDrainedLocked(){return audio_playback_queue_.empty()&&!output_in_flight_;}
 void AudioOutputTask();
};
'''+method+r'''
int main(){
 AudioService service;service.playout_buffer_.Enable(true);
 service.audio_playback_queue_.push_back(AudioTask{});
 std::thread worker([&]{service.AudioOutputTask();});
 while(!service.waiting.load())std::this_thread::yield();
 {
  std::lock_guard<std::mutex> lock(service.audio_queue_mutex_);
  ++service.playback_generation_;service.audio_playback_queue_.clear();
  service.service_stopped_=true;service.audio_queue_cv_.notify_all();
 }
 worker.join();
 assert(service.codec.writes==0&&!service.codec.enabled&&!service.output_in_flight_);
}
''')

    def test_tts_burst_budget_is_bounded_nonblocking_and_keeps_generation_fence(self):
        source=(ROOT/'main/audio/audio_service.cc').read_text()
        init=queue_function(source,'void AudioService::Initialize(')
        self.assertIn('SupportsLocalInteraction()',init)
        self.assertIn('buffered_playback_ ? 96 : MAX_DECODE_PACKETS_IN_QUEUE',init)
        method=queue_function(source,'bool AudioService::PushPacketToDecodeQueue(')
        motion_tests.WzhFluentMotionTest.run_cpp(self,r'''
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdint>
struct AudioStreamPacket{};
struct AudioService{
 std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<std::unique_ptr<AudioStreamPacket>>audio_decode_queue_;
 std::atomic<bool>service_stopped_{false};
 bool playback_drained_notified_=true;
 uint32_t playback_generation_=7,decode_queue_full_count_=0;
 size_t max_decode_packets_=40;
 bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>,bool,
                              std::optional<uint32_t> expected_generation=std::nullopt);
};
'''+method+r'''
int main(){
 for(unsigned budget:{40U,96U}){
  AudioService service;service.max_decode_packets_=budget;
  for(unsigned n=0;n<budget;++n)
   assert(service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),false,7));
  assert(!service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),false,7));
  assert(service.audio_decode_queue_.size()==budget&&service.decode_queue_full_count_==1);
  ++service.playback_generation_;service.audio_decode_queue_.clear();
  assert(!service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),false,7));
  assert(service.audio_decode_queue_.empty());
  assert(service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),false,8));
  service.service_stopped_=true;
  assert(!service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),false,8));
 }
}
''')
