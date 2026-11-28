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
    auto wait = [&](uint32_t generation, uint32_t ms, int64_t now) {
        return buffer.WaitMs(generation, now, [ms](uint32_t){return ms;});
    };
    assert(wait(1,20,1000)==0); // unchanged non-robot behavior
    buffer.Enable(true);
    assert(wait(2,60,1000)==80);
    assert(wait(2,60,1030)==50);
    assert(wait(2,120,1040)==0);
    buffer.Written(60,1040);buffer.Written(60,1040);
    // DMA still contains sound while the software queue briefly empties.
    assert(wait(2,0,1080)==0);
    assert(wait(2,60,1100)==0);
    buffer.Written(60,1100);
    assert(wait(2,60,1180)==0);
    buffer.Written(60,1180);
    assert(buffer.rebuffer_count()==0);
    assert(wait(2,20,1310)==80);
    assert(wait(2,20,1389)==1);
    assert(wait(2,20,1390)==0); // a short prompt is never stuck
    assert(buffer.rebuffer_count()==1);
    assert(wait(3,60,2000)==80);
    assert(wait(4,60,2040)==80); // cancellation restarts prefix
    assert(wait(4,60,2119)==1);
    assert(wait(4,60,2120)==0);
    buffer.Enable(false);
    assert(wait(5,1,3000)==0);
}
''')

    def test_cellular_uses_deeper_prefix_only_after_underrun_with_the_same_deadline(self):
        motion_tests.WzhFluentMotionTest.run_cpp(self,
            f'#include "{HEADER}"\n' + r'''
#include <cassert>
int main() {
    AudioPlayoutBuffer buffer;
    buffer.Enable(true);
    buffer.Configure(360, 480, 480);
    auto wait = [&](uint32_t generation, int64_t now, uint32_t ms, uint32_t expected_target) {
        unsigned estimates = 0;
        auto remaining = buffer.WaitMs(generation, now, [&](uint32_t target) {
            ++estimates;
            assert(target == expected_target);
            return ms;
        });
        assert(estimates == (expected_target ? 1 : 0));
        return remaining;
    };

    assert(wait(1, 1000, 60, 360) == 480);
    assert(wait(1, 1060, 360, 360) == 0);
    for (int i = 0; i < 6; ++i)
        buffer.Written(60, 1060);  // The estimated output horizon reaches 1420.
    assert(wait(1, 1440, 60, 0) == 0);  // Brief gaps keep playing without re-estimating.
    assert(wait(1, 1441, 0, 480) == 0);
    assert(buffer.rebuffer_count() == 0);
    assert(wait(1, 1445, 360, 480) == 480);
    assert(wait(1, 1520, 420, 480) == 405);
    assert(wait(1, 1560, 480, 480) == 0);
    assert(buffer.rebuffer_count() == 1);

    buffer.Written(60, 1560);
    assert(wait(1, 1900, 60, 480) == 480);
    assert(wait(1, 2379, 60, 480) == 1);
    assert(wait(1, 2380, 60, 480) == 0);  // Sparse delivery cannot extend a live wait.
    assert(buffer.rebuffer_count() == 2);

    buffer.Written(60, 2380);
    assert(wait(1, 2461, 360, 480) == 480);
    assert(wait(2, 2462, 360, 360) == 0);  // A new reply uses the first-prefix target.
    assert(buffer.rebuffer_count() == 3);
    buffer.Configure(120, 80);  // Moving to a local link clears the cellular target.
    buffer.Written(60, 2462);
    assert(wait(2, 2550, 60, 120) == 80);
    assert(wait(2, 2551, 120, 120) == 0);
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
 void OutputData(std::vector<int16_t>&){++writes;}
};
struct AudioService{
 Codec codec;Codec*codec_=&codec;void*audio_power_timer_=nullptr;
 std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<AudioTask>audio_playback_queue_;
 std::deque<int>audio_decode_queue_;
 std::atomic<bool>service_stopped_{false};uint32_t playback_generation_=7;
 std::atomic<bool>waiting{false};AudioPlayoutBuffer playout_buffer_;
 uint32_t BufferedPrefixMsLocked(uint32_t){waiting=true;return 60;}
 size_t max_playback_tasks_=6;
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
 enum class PlaybackSource { kRegular,kShutdown };
 std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<std::unique_ptr<AudioStreamPacket>>audio_decode_queue_;
 std::atomic<bool>service_stopped_{false};
 bool playback_drained_notified_=true,shutdown_playback_=false;
 uint32_t playback_generation_=7,decode_queue_full_count_=0;
 size_t max_decode_packets_=40;
 bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>,bool,
                              std::optional<uint32_t> expected_generation=std::nullopt,
                              PlaybackSource source=PlaybackSource::kRegular);
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

    def test_encoded_packets_supply_cellular_startup_and_recovery_prefixes(self):
        source = (ROOT / 'main/audio/audio_service.cc').read_text()
        methods = '\n'.join(queue_function(source, name) for name in (
            'bool SupportsOpusDecodeFormat(',
            'uint32_t AudioService::BufferedPlaybackMsLocked(',
            'uint32_t AudioService::BufferedPrefixMsLocked(',
            'void AudioService::AudioOutputTask(',
        ))
        program = f'#include "{HEADER}"\n#include "{HEADER.with_name("opus_stream_metadata.h")}"\n' + r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#define ESP_LOGW(...) ((void)0)
#define AS_OPUS_GET_FRAME_DRU_ENUM(ms) ((ms)==5||(ms)==10||(ms)==20||(ms)==40||(ms)==60||(ms)==80||(ms)==100||(ms)==120?0:-1)
constexpr unsigned AUDIO_POWER_CHECK_INTERVAL_MS = 1000;
void esp_timer_stop(void*) {}
void esp_timer_start_periodic(void*, unsigned) {}

struct Packet {
    int sample_rate = 16000, frame_duration = 20;
    std::vector<uint8_t> payload{0x08, 0x42}; // A 20 ms SILK TOC.
    OpusStreamMetadata stream;
};
struct AudioTask {
    uint32_t playback_generation = 7, timestamp = 0;
    std::vector<int16_t> pcm = std::vector<int16_t>(320, 1000);
    OpusStreamMetadata stream;
};
struct QueueEvent {
    std::function<void()> on_wait;
    void notify_all() {}
    template <typename Predicate>
    void wait(std::unique_lock<std::mutex>&, Predicate ready) { assert(ready()); }
    template <typename Predicate>
    void wait_for(std::unique_lock<std::mutex>& lock, std::chrono::milliseconds timeout,
                  Predicate ready) {
        assert(timeout.count() == 480 && !ready() && on_wait);
        lock.unlock();
        std::exchange(on_wait, {})();
        lock.lock();
        assert(ready()); // Encoded arrivals wake output even though PCM depth is unchanged.
    }
};
struct Codec {
    std::function<void()> wrote;
    bool enabled = false;
    int output_sample_rate() { return 16000; }
    bool output_enabled() { return enabled; }
    void EnableOutput(bool value) { enabled = value; }
    void ResetOutputProcessing() {}
    void SetOutputStreamGain(int16_t) {}
    void OutputData(std::vector<int16_t>&) { wrote(); }
    size_t FinishOutput() { return 0; }
};
struct AudioService {
    Codec codec;
    Codec* codec_ = &codec;
    void* audio_power_timer_ = nullptr;
    std::mutex audio_queue_mutex_;
    QueueEvent audio_queue_cv_;
    std::deque<AudioTask> audio_playback_queue_;
    std::deque<std::unique_ptr<Packet>> audio_decode_queue_;
    const size_t max_playback_tasks_ = 6;
    std::atomic<bool> service_stopped_{false};
    uint32_t playback_generation_ = 7;
    AudioPlayoutBuffer playout_buffer_;
    bool buffered_playback_ = true;
    bool decode_in_flight_ = false, output_in_flight_ = false;
    bool finish_requested_ = false, cancel_fade_requested_ = false;
    float output_fast_env_ = 0, output_slow_env_ = 0;
    std::function<void(float,float)> output_audio_level_listener_;
    std::chrono::steady_clock::time_point last_output_time_;
    struct { int playback_count = 0; } debug_statistics_;
    struct { std::function<void()> on_playback_drained; } callbacks_;
    uint32_t GetPlaybackGeneration() { return playback_generation_; }
    bool MarkPlaybackDrainedLocked() { return false; }
    uint32_t BufferedPlaybackMsLocked() const;
    uint32_t BufferedPrefixMsLocked(uint32_t target_ms) const;
    void AudioOutputTask();
};
''' + methods + r'''
int main() {
    AudioService service;
    service.playout_buffer_.Enable(true);
    service.playout_buffer_.Configure(360, 480);
    service.audio_playback_queue_.resize(6);
    const auto prefix = [&](uint32_t target = 360){ return service.BufferedPrefixMsLocked(target); };
    auto unsupported = std::make_unique<Packet>();
    unsupported->sample_rate = 44100;
    auto mismatch = std::make_unique<Packet>();
    mismatch->frame_duration = 40;
    auto malformed = std::make_unique<Packet>();
    malformed->payload = {0x0b}; // TOC indicates multiple frames without a count byte.
#if CONFIG_AUDIO_PRODUCT_QUALITY
    auto trimmed = std::make_unique<Packet>();
    trimmed->stream.skip_48k = 960;
    service.audio_decode_queue_.push_back(std::move(trimmed));
#endif
    assert(prefix() == 120);
    for (int i = 0; i < 11; ++i)
        service.audio_decode_queue_.push_back(std::make_unique<Packet>());
    assert(prefix() == 340 && service.BufferedPlaybackMsLocked() == 120);
    for (auto* invalid : {&unsupported, &mismatch, &malformed}) {
        service.audio_decode_queue_.insert(service.audio_decode_queue_.end() - 1,
                                           std::move(*invalid));
        assert(prefix() == 320); // A known decode hole ends the compressed prefix.
        service.audio_decode_queue_.erase(service.audio_decode_queue_.end() - 2);
    }
    service.audio_decode_queue_.pop_back();
    assert(prefix() == 320); // A packet removed for decoding cannot still count.
    service.audio_decode_queue_.push_back(std::make_unique<Packet>());

    service.audio_queue_cv_.on_wait = [&] {
        std::lock_guard<std::mutex> lock(service.audio_queue_mutex_);
        service.audio_decode_queue_.push_back(std::make_unique<Packet>());
        assert(prefix() == 360);
    };
    service.codec.wrote = [&] { service.service_stopped_ = true; };
    service.AudioOutputTask();
    assert(service.debug_statistics_.playback_count == 1 && !service.audio_queue_cv_.on_wait);

    ++service.playback_generation_;
    service.audio_decode_queue_.clear();
    service.audio_playback_queue_.clear();
    assert(prefix() == 0);
    service.audio_playback_queue_.resize(5);
    service.audio_decode_queue_.push_back(std::make_unique<Packet>());
    assert(prefix() == 100); // Credit compressed audio only when no decoded slot is available.
    assert(service.playout_buffer_.WaitMs(service.playback_generation_, 2000, prefix) == 480);

    AudioService recovering;
    recovering.playout_buffer_.Enable(true);
    recovering.playout_buffer_.Configure(360, 480, 480);
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto previous_start =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count() - 1000;
    assert(recovering.playout_buffer_.WaitMs(
               recovering.playback_generation_, previous_start, [](uint32_t) { return 360U; }) == 0);
    recovering.playout_buffer_.Written(360, previous_start);
    recovering.audio_playback_queue_.resize(6);
    for (auto& task : recovering.audio_playback_queue_)
        task.pcm.resize(960); // The standard 60 ms server frame.
    assert(recovering.BufferedPrefixMsLocked(480) == 360);
    recovering.audio_queue_cv_.on_wait = [&] {
        std::lock_guard<std::mutex> lock(recovering.audio_queue_mutex_);
        for (unsigned i = 0; i < 2; ++i) {
            auto packet = std::make_unique<Packet>();
            packet->frame_duration = 60;
            packet->payload = {0x18, 0x42};
            recovering.audio_decode_queue_.push_back(std::move(packet));
            assert(recovering.BufferedPrefixMsLocked(480) == 420 + 60 * i);
        }
    };
    recovering.codec.wrote = [&] { recovering.service_stopped_ = true; };
    recovering.AudioOutputTask();
    assert(recovering.debug_statistics_.playback_count == 1 &&
           recovering.playout_buffer_.rebuffer_count() == 1 && !recovering.audio_queue_cv_.on_wait);
}
'''
        for quality in (0, 1):
            with self.subTest(product_quality=quality):
                motion_tests.WzhFluentMotionTest.run_cpp(
                    self, f'#define CONFIG_AUDIO_PRODUCT_QUALITY {quality}\n#include <utility>\n' + program)
