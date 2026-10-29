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
    buffer.Enable(false);
    assert(buffer.WaitMs(5,1,3000)==0);
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
 int writes=0;bool enabled=false;
 int output_sample_rate(){return 16000;}
 bool output_enabled(){return enabled;}void EnableOutput(bool e){enabled=e;}
 void OutputData(std::vector<int16_t>&){++writes;}
};
struct AudioService{
 Codec codec;Codec*codec_=&codec;void*audio_power_timer_=nullptr;
 std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<std::unique_ptr<AudioTask>>audio_playback_queue_;
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
 service.audio_playback_queue_.push_back(std::make_unique<AudioTask>());
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
