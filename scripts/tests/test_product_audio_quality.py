"""Execute product container timing and speaker-ending production code."""
from pathlib import Path
import unittest

import test_firmware_runtime_optimizations as runtime
from test_wzh_action_queue import queue_function

ROOT=Path(__file__).resolve().parents[2]
AUDIO=ROOT/'main/audio'


class ProductAudioQualityTest(unittest.TestCase):
    def run_cpp(self, program, sources=()):
        runtime.FirmwareRuntimeOptimizationsTest.run_cpp(self, program, sources=sources)

    def test_offline_excerpt_finishes_on_exact_and_partial_packet_boundaries(self):
        source=(ROOT/'main/boards/common/wzh/wzh_audio_player.cc').read_text()
        callback=queue_function(source,'demuxer.OnOpusPacket(')+');'
        program=r'''
#include <atomic>
#include <cassert>
#include <memory>
#include "demuxer/ogg_demuxer.h"
#include "../boards/common/wzh/wzh_sound_score.h"
using wzh::OpusPacketDurationMs;
struct AudioStreamPacket {
 int sample_rate=0,frame_duration=0;OpusStreamMetadata stream;std::vector<uint8_t>payload;
};
struct Service {
 std::vector<std::unique_ptr<AudioStreamPacket>>packets;
 bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>p,bool,uint32_t){
  packets.push_back(std::move(p));return true;
 }
};
struct App {Service service;Service&GetAudioService(){return service;}};
struct Player {
 std::atomic<uint32_t>playback_generation_{7};std::atomic<int>local_packet_ms_{0};
 void Run(uint32_t duration,uint32_t preskip);
};
void Player::Run(uint32_t duration,uint32_t preskip){
 App app;bool ok=true;uint64_t submitted_48k=0;uint32_t submitted_ms=0;
 struct {uint32_t duration_ms;}file{duration};
 const uint32_t generation=7,decoder_generation=1;const int16_t content_gain=256;
 OggDemuxer demuxer;
'''+callback+r'''
 // Feed real Ogg pages with 20 ms packets, and more data than the excerpt.
 auto page=[&](uint8_t flags,const std::vector<uint8_t>&body){
  std::vector<uint8_t>b(28,0);std::memcpy(b.data(),"OggS",4);b[5]=flags;
  b[26]=1;b[27]=body.size();b.insert(b.end(),body.begin(),body.end());
  demuxer.Process(b.data(),b.size());
 };
 std::vector<uint8_t>head(19,0);std::memcpy(head.data(),"OpusHead",8);
 head[8]=1;head[9]=1;head[10]=preskip&255;head[11]=preskip>>8;
 page(2,head);page(0,{'O','p','u','s','T','a','g','s'});
 for(int i=0;i<10;++i)page(0,{0xd8,0});
 assert(ok&&submitted_48k==uint64_t(duration)*48&&submitted_ms==duration);
 assert(app.service.packets.size()==(preskip?3:1));
 assert(app.service.packets.back()->stream.end);
 assert(app.service.packets.back()->stream.gain_q8==256);
 uint64_t audible=0;
 for(auto&p:app.service.packets){
  assert(p->sample_rate==48000&&p->frame_duration==20);
  std::vector<int16_t>pcm(960,1);TrimOpusPcm(pcm,48000,p->stream);audible+=pcm.size();
 }
 assert(audible==uint64_t(duration)*48);
}
int main(){Player player;player.Run(20,0);player.Run(40,312);}
'''
        self.run_cpp(program, sources=(AUDIO/'demuxer/ogg_demuxer.cc',))

    def test_container_metadata_preskip_end_trim_gain_and_every_fragment_boundary(self):
        self.run_cpp(r'''
#include <cassert>
#include <vector>
#include "demuxer/ogg_demuxer.h"
void Page(std::vector<uint8_t>& out,uint8_t flags,uint64_t granule,
          const std::vector<uint8_t>& laces,const std::vector<uint8_t>& body){
 size_t at=out.size();out.resize(at+27,0);std::memcpy(out.data()+at,"OggS",4);
 out[at+5]=flags;for(unsigned i=0;i<8;++i)out[at+6+i]=granule>>(8*i);
 out[at+26]=laces.size();out.insert(out.end(),laces.begin(),laces.end());
 out.insert(out.end(),body.begin(),body.end());
}
int main(){
 std::vector<uint8_t> head(19);std::memcpy(head.data(),"OpusHead",8);
 head[8]=1;head[9]=1;head[10]=1200&255;head[11]=1200>>8;
 // Original recording rate 44100 is informational, not an Opus decoder rate.
 head[12]=0x44;head[13]=0xac;head[17]=1; // +1 dB Q8.
 std::vector<uint8_t> stream;
 Page(stream,2,0,{19},head);Page(stream,0,0,{8},{'O','p','u','s','T','a','g','s'});
 Page(stream,4,1800,{2,2},{0xd8,0,0xd8,0});
 for(size_t chunk=1;chunk<=stream.size();++chunk){
  OggDemuxer parser;std::vector<OpusStreamMetadata> records;
  parser.OnOpusPacket([&](const uint8_t* data,size_t size,const OpusStreamMetadata& info){
   assert(OpusPacketSamples48k(data,size)==960);records.push_back(info);
  });
  for(size_t i=0;i<stream.size();i+=chunk){
   size_t n=std::min(chunk,stream.size()-i);assert(parser.Process(stream.data()+i,n)==n);
  }
  assert(parser.valid_stream()&&records.size()==2);
  assert(records[0].start&&!records[0].end&&records[0].skip_48k==960);
  assert(!records[1].start&&records[1].end&&records[1].skip_48k==240);
  assert(records[1].keep_end_48k==840&&records[1].gain_q8==256);
  std::vector<int16_t> a(960,1),b(960);for(int i=0;i<960;++i)b[i]=i;
  TrimOpusPcm(a,48000,records[0]);TrimOpusPcm(b,48000,records[1]);
  assert(a.empty()&&b.size()==600&&b.front()==240&&b.back()==839);
  parser.Reset();assert(!parser.valid_stream());
 }
}
''', sources=(AUDIO/'demuxer/ogg_demuxer.cc',))

    def test_opus_durations_limits_and_trim_bounds(self):
        self.run_cpp(r'''
#include <cassert>
#include "opus_stream_metadata.h"
int main(){
 assert(OpusPacketSamples48k(nullptr,0)==0);
 for(unsigned config=0;config<32;++config){
  const unsigned silk[]={480,960,1920,2880};
  unsigned frame=config<12?silk[config%4]:config<16?480U<<(config%2):120U<<(config%4);
  uint8_t packet[]={uint8_t(config<<3),0};
  assert(OpusPacketSamples48k(packet,2)==frame);
  packet[0]|=3;packet[1]=0;assert(OpusPacketSamples48k(packet,2)==0);
  packet[1]=3;assert(OpusPacketSamples48k(packet,2)==(frame*3<=5760?frame*3:0));
  assert(OpusPacketSamples48k(packet,1)==0);
 }
 std::vector<int16_t> pcm(100,3);OpusStreamMetadata info;
 TrimOpusPcm(pcm,24000,info);assert(pcm.size()==100);
 info.skip_48k=UINT32_MAX;TrimOpusPcm(pcm,48000,info);assert(pcm.empty());
 pcm.assign(100,1);info={};info.keep_end_48k=10;info.skip_48k=20;
 TrimOpusPcm(pcm,48000,info);assert(pcm.empty());
 pcm.assign(100,1);TrimOpusPcm(pcm,0,info);assert(pcm.empty());
}
''')

    def test_recording_replay_preserves_all_packets_and_has_independent_boundaries(self):
        source=(AUDIO/'audio_service.cc').read_text()
        program=r'''
#define CONFIG_AUDIO_PRODUCT_QUALITY 1
#include <cassert>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include "opus_stream_metadata.h"
#define ESP_LOGI(...) ((void)0)
constexpr int AS_EVENT_AUDIO_TESTING_RUNNING=1;
void xEventGroupSetBits(void*,int){}void xEventGroupClearBits(void*,int){}
struct Packet {OpusStreamMetadata stream;int value=0;};
struct AudioService{
 void*event_group_=nullptr;std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<std::unique_ptr<Packet>>audio_decode_queue_,audio_testing_queue_;
 bool playback_drained_notified_=true;
 void EnableAudioTesting(bool);
};
'''+queue_function(source,'void AudioService::EnableAudioTesting(')+r'''
int main(){
 AudioService service;
 for(int i=0;i<166;++i){auto p=std::make_unique<Packet>();p->value=i;
  service.audio_testing_queue_.push_back(std::move(p));}
 service.EnableAudioTesting(false);
 assert(service.audio_testing_queue_.empty()&&service.audio_decode_queue_.size()==166);
 assert(!service.playback_drained_notified_);
 for(int i=0;i<166;++i){auto&p=service.audio_decode_queue_[i];
  assert(p->value==i&&p->stream.start==(i==0)&&p->stream.end==(i==165));}
}
'''
        self.run_cpp(program)

    def test_actual_codec_finish_flushes_valid_tail_and_cancellation_only_fades(self):
        source=(AUDIO/'audio_codec.cc').read_text()
        program=r'''
#define CONFIG_AUDIO_PRODUCT_QUALITY 1
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include "speaker_protection.h"
struct AudioCodec{
 SpeakerProtection speaker_protection_;bool speaker_protection_enabled_=true,output_enabled_=true;
 std::atomic<bool>output_processing_reset_pending_{true};
 std::vector<int16_t>finish_padding_,written;float output_stream_gain_=1;
 int output_sample_rate_=24000;
 int16_t last_output_sample_=0,output_gain_q8_=0;
 void OutputData(std::vector<int16_t>&);void ResetOutputProcessing();
 void SetOutputStreamGain(int16_t);size_t FinishOutput();
 int Write(const int16_t*data,int size){written.insert(written.end(),data,data+size);return size;}
};
'''
        for marker in ['void AudioCodec::OutputData(', 'void AudioCodec::ResetOutputProcessing(',
                       'void AudioCodec::SetOutputStreamGain(', 'size_t AudioCodec::FinishOutput(']:
            program+=queue_function(source,marker)+'\n'
        program+=r'''
int main(){
 for(int rate:{8000,16000,24000,48000}){
  AudioCodec codec;codec.output_sample_rate_=rate;codec.speaker_protection_.Configure(rate,true,true);
  codec.finish_padding_.assign(rate/1000,0);
  std::vector<int16_t>pcm(rate/10+5);
  for(size_t i=0;i<pcm.size();++i)pcm[i]=std::lrint(15000*std::cos(6.283185307179586*1000*i/rate));
  codec.SetOutputStreamGain(12*256);codec.OutputData(pcm);
  for(int sample:codec.written)assert(std::abs(sample)<=27525);
  codec.written.clear();codec.FinishOutput();
  assert(codec.written.size()>=size_t(rate/1000)&&codec.written.back()==0);
  assert(codec.last_output_sample_==0);
  codec.SetOutputStreamGain(0);codec.OutputData(pcm);
  codec.ResetOutputProcessing();int last=codec.last_output_sample_;codec.written.clear();
  codec.FinishOutput();
  assert(codec.written.size()==(last?size_t(rate/200):0));
  for(int sample:codec.written)assert(std::abs(sample)<=std::abs(last));
  if(last)assert(codec.written.back()==0);
  std::vector<int16_t>silent(100,0);codec.OutputData(silent);
  for(int sample:silent)assert(sample==0);
 }
}
'''
        self.run_cpp(program)

    def test_finish_request_rearms_drained_notification_for_already_empty_queue(self):
        source=(AUDIO/'audio_service.cc').read_text()
        program=r'''
#define CONFIG_AUDIO_PRODUCT_QUALITY 1
#include <cassert>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include "opus_stream_metadata.h"
struct Packet{OpusStreamMetadata stream;};
struct AudioTask{OpusStreamMetadata stream;};
struct AudioService{
 std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<std::unique_ptr<Packet>>audio_decode_queue_;std::deque<AudioTask>audio_playback_queue_;
 bool decode_in_flight_=false,output_in_flight_=false,playback_drained_notified_=true;
 std::atomic<uint32_t> playback_drained_count_{0};
 bool finish_requested_=false,cancel_fade_requested_=false;
 void FinishPlayback();bool IsPlaybackDrainedLocked()const;bool MarkPlaybackDrainedLocked();
};
'''
        for marker in ['void AudioService::FinishPlayback(', 'bool AudioService::IsPlaybackDrainedLocked(',
                       'bool AudioService::MarkPlaybackDrainedLocked(']:
            program+=queue_function(source,marker)+'\n'
        program+=r'''
int main(){
 AudioService service;service.FinishPlayback();
 assert(service.finish_requested_&&!service.playback_drained_notified_);
 assert(!service.IsPlaybackDrainedLocked()&&!service.MarkPlaybackDrainedLocked());
 service.finish_requested_=false;assert(service.MarkPlaybackDrainedLocked());
 assert(!service.MarkPlaybackDrainedLocked());
 service.audio_decode_queue_.push_back(std::make_unique<Packet>());service.FinishPlayback();
 assert(service.audio_decode_queue_[0]->stream.end&&!service.finish_requested_);
 service.audio_decode_queue_.clear();service.audio_playback_queue_.push_back({});
 service.FinishPlayback();assert(service.audio_playback_queue_[0].stream.end);
 service.audio_playback_queue_.clear();service.decode_in_flight_=true;service.FinishPlayback();
 assert(service.finish_requested_&&!service.IsPlaybackDrainedLocked());
 service.finish_requested_=false;service.audio_playback_queue_.push_back({});service.FinishPlayback();
 assert(service.finish_requested_&&!service.audio_playback_queue_[0].stream.end);
 service.audio_playback_queue_.clear();
 service.decode_in_flight_=false;service.finish_requested_=false;
 assert(service.MarkPlaybackDrainedLocked());
}
'''
        self.run_cpp(program)

    def test_output_writer_fades_cancelled_session_before_new_pcm_and_drains_once(self):
        source=(AUDIO/'audio_service.cc').read_text()
        program=r'''
#define CONFIG_AUDIO_PRODUCT_QUALITY 1
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "audio_playout_buffer.h"
#include "opus_stream_metadata.h"
#define ESP_LOGW(...) ((void)0)
constexpr unsigned AUDIO_POWER_CHECK_INTERVAL_MS=1000;
void esp_timer_stop(void*){}void esp_timer_start_periodic(void*,unsigned){}
struct Packet{};
struct AudioTask{uint32_t playback_generation=7,timestamp=0;OpusStreamMetadata stream;std::vector<int16_t>pcm;};
struct Codec{
 std::vector<std::string>events;bool enabled=true;
 void ResetOutputProcessing(){}int output_sample_rate(){return 16000;}
 bool output_enabled(){return enabled;}void EnableOutput(bool value){enabled=value;}
 void SetOutputStreamGain(int16_t){}void OutputData(std::vector<int16_t>&){events.push_back("data");}
 size_t FinishOutput(){events.push_back("finish");return 96;}
};
struct AudioService{
 Codec codec;Codec*codec_=&codec;void*audio_power_timer_=nullptr;
 std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<AudioTask>audio_playback_queue_;std::deque<std::unique_ptr<Packet>>audio_decode_queue_;
 std::atomic<bool>service_stopped_{false};uint32_t playback_generation_=7;
 bool finish_requested_=false,cancel_fade_requested_=true;
 bool decode_in_flight_=false,output_in_flight_=false,playback_drained_notified_=false;
 std::atomic<uint32_t> playback_drained_count_{0};
 std::atomic<bool>waiting{false};
 AudioPlayoutBuffer playout_buffer_;uint32_t BufferedPlaybackMsLocked(){waiting=true;return 60;}
 bool buffered_playback_=true;float output_fast_env_=0,output_slow_env_=0;
 std::function<void(float,float)>output_audio_level_listener_;
 std::chrono::steady_clock::time_point last_output_time_;
 struct{int playback_count=0;}debug_statistics_;
 struct{std::function<void()>on_playback_drained;}callbacks_;
 uint32_t GetPlaybackGeneration(){std::lock_guard<std::mutex>lock(audio_queue_mutex_);return playback_generation_;}
 bool IsPlaybackDrainedLocked()const;bool MarkPlaybackDrainedLocked();void AudioOutputTask();
};
'''
        for marker in ['bool AudioService::IsPlaybackDrainedLocked(', 'bool AudioService::MarkPlaybackDrainedLocked(',
                       'void AudioService::AudioOutputTask(']:
            program+=queue_function(source,marker)+'\n'
        program+=r'''
int main(){
 AudioService service;AudioTask task;task.pcm.assign(960,1000);task.stream.end=true;
 service.audio_playback_queue_.push_back(task);int drained=0;
 service.callbacks_.on_playback_drained=[&]{++drained;service.service_stopped_=true;service.audio_queue_cv_.notify_all();};
 std::thread worker([&]{service.AudioOutputTask();});worker.join();
 assert(service.codec.events==std::vector<std::string>({"finish","data","finish"}));
 assert(drained==1&&service.IsPlaybackDrainedLocked()&&!service.output_in_flight_);
 // Reset while a short prefix is waiting must still wake the writer, fade
 // only the old physical tail, and never deliver the dequeued old PCM.
 AudioService cancelled;cancelled.cancel_fade_requested_=false;
 cancelled.playout_buffer_.Enable(true);cancelled.audio_playback_queue_.push_back(task);
 cancelled.callbacks_.on_playback_drained=[&]{cancelled.service_stopped_=true;cancelled.audio_queue_cv_.notify_all();};
 std::thread other([&]{cancelled.AudioOutputTask();});
 while(!cancelled.waiting.load())std::this_thread::yield();
 {
  std::lock_guard<std::mutex>lock(cancelled.audio_queue_mutex_);
  ++cancelled.playback_generation_;cancelled.audio_playback_queue_.clear();
  cancelled.cancel_fade_requested_=true;cancelled.playback_drained_notified_=false;
  cancelled.audio_queue_cv_.notify_all();
 }
 other.join();assert(cancelled.codec.events==std::vector<std::string>({"finish"}));
 // A failed final decode still finishes playback without an empty DAC write.
 AudioService failed;failed.cancel_fade_requested_=false;task.pcm.clear();
 failed.audio_playback_queue_.push_back(task);
 failed.callbacks_.on_playback_drained=[&]{failed.service_stopped_=true;failed.audio_queue_cv_.notify_all();};
 std::thread last([&]{failed.AudioOutputTask();});last.join();
 assert(failed.codec.events==std::vector<std::string>({"finish"}));
}
'''
        self.run_cpp(program)

    def test_calibration_matches_trusted_asset_and_rejects_modified_or_unknown_audio(self):
        import json
        import re
        calibration=json.loads((ROOT/'docs/wzh/data/tailredpanda_audio_calibration.json').read_text())
        first=next(item for item in calibration if not item['source'].startswith('paired-'))
        data=(ROOT/'main/assets/locales/zh-CN'/first['source']).read_bytes()
        self.run_cpp('#include "'+str(ROOT/'main/boards/wzh_tailRedPanda/audio_calibration.inc')+'"\n'+r'''
#include <cassert>
#include <vector>
int main(){
 std::vector<uint8_t> source={'''+','.join(map(str,data))+r'''};
 assert(tail_audio_calibration::Find(source.data(),source.size())=='''+str(first['gain_q8'])+r''');
 source.back()^=1;assert(tail_audio_calibration::Find(source.data(),source.size())==0);
 assert(tail_audio_calibration::Find(nullptr,0)==0);
 assert(tail_audio_calibration::Find(source.data(),1)==0);
}
''')

    def test_decoder_applies_metadata_before_conversion_and_fences_stale_results(self):
        source=(AUDIO/'audio_service.cc').read_text()
        begin=source.index('        /* Decode the audio from decode queue */')
        end=source.index('        /* Encode the audio to send queue */',begin)
        block=source[begin:end]
        program=r'''
#define CONFIG_AUDIO_PRODUCT_QUALITY 1
#define ESP_LOGE(...) ((void)0)
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include "opus_stream_metadata.h"
constexpr int ESP_AUDIO_ERR_OK=0,ESP_AE_ERR_OK=0,ESP_AUDIO_DEC_RECOVERY_NONE=0;
using esp_ae_sample_t=int16_t*;
struct esp_audio_dec_in_raw_t{uint8_t*buffer;uint32_t len,consumed;int frame_recover;};
struct esp_audio_dec_out_frame_t{uint8_t*buffer;uint32_t len,decoded_size;};
struct esp_audio_dec_info_t{};
int decoder_resets=0,resampler_resets=0;bool fail_decode=false,fail_convert=false;
std::function<void()>during_decode;
void esp_opus_dec_reset(void*){++decoder_resets;}void esp_ae_rate_cvt_reset(void*){++resampler_resets;}
int esp_opus_dec_decode(void*,esp_audio_dec_in_raw_t*,esp_audio_dec_out_frame_t*out,esp_audio_dec_info_t*){
 if(during_decode)during_decode();
 if(fail_decode)return -1;
 assert(out->len>=1920);auto*p=reinterpret_cast<int16_t*>(out->buffer);
 for(int i=0;i<960;++i)p[i]=i;
 out->decoded_size=1920;return 0;
}
int esp_ae_rate_cvt_get_max_out_sample_num(void*,size_t count,uint32_t*out){*out=count/2;return 0;}
int esp_ae_rate_cvt_process(void*,int16_t*in,size_t count,int16_t*out,uint32_t*n){
 if(fail_convert)return -1;
 assert(*n>=count/2);for(size_t i=0;i<count/2;++i)out[i]=in[i*2];*n=count/2;return 0;
}
struct AudioStreamPacket{int sample_rate=48000,frame_duration=20;uint32_t timestamp=0;std::vector<uint8_t>payload{0xd8,0};OpusStreamMetadata stream;};
constexpr int kAudioTaskTypeDecodeToPlaybackQueue=1;
struct AudioTask{int type=0;uint32_t timestamp=0,playback_generation=0;std::vector<int16_t>pcm;OpusStreamMetadata stream;};
struct Codec{int output_sample_rate(){return 24000;}};
struct AudioService{
 Codec codec;Codec*codec_=&codec;std::mutex audio_queue_mutex_,decoder_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<std::unique_ptr<AudioStreamPacket>>audio_decode_queue_;std::deque<AudioTask>audio_playback_queue_;
 size_t max_playback_tasks_=6;bool decode_in_flight_=false;uint32_t playback_generation_=7;
 int decoder_sample_rate_=48000,decoder_frame_size_=960;void*opus_decoder_=this,*output_resampler_=this;
 std::atomic<bool>service_stopped_{false};struct{int decode_count=0;}debug_statistics_;
 struct{std::function<void()>on_playback_drained;}callbacks_;
 bool SetDecodeSampleRate(int rate,int frame){assert(rate==48000&&frame==20);return true;}
 bool MarkPlaybackDrainedLocked(){return false;}
 void DecodeOne();
};
void AudioService::DecodeOne(){
 std::vector<int16_t>decode_scratch;std::unique_lock<std::mutex>lock(audio_queue_mutex_);
'''+block+r'''
}
int main(){
 AudioService service;
 auto feed=[&](OpusStreamMetadata metadata){
  auto packet=std::make_unique<AudioStreamPacket>();packet->stream=metadata;
  service.audio_decode_queue_.push_back(std::move(packet));service.DecodeOne();
 };
 OpusStreamMetadata info;info.start=true;info.skip_48k=312;info.gain_q8=256;
 feed(info);assert(service.audio_playback_queue_.size()==1);
 auto& first=service.audio_playback_queue_.back();
 assert(first.pcm.size()==324&&first.pcm.front()==312&&first.stream.gain_q8==256);
 assert(decoder_resets==1&&resampler_resets==1);service.audio_playback_queue_.clear();
 info={};info.end=true;info.keep_end_48k=480;feed(info);
 assert(service.audio_playback_queue_.back().pcm.size()==360); // 240 real + 120 drain samples.
 assert(service.audio_playback_queue_.back().pcm.back()==0);service.audio_playback_queue_.clear();
 info={};info.skip_48k=960;feed(info);assert(service.audio_playback_queue_.empty());
 fail_decode=true;info={};info.end=true;feed(info);fail_decode=false;
 assert(service.audio_playback_queue_.size()==1&&service.audio_playback_queue_.back().stream.end);
 assert(service.audio_playback_queue_.back().pcm.empty());service.audio_playback_queue_.clear();
 fail_convert=true;info={};feed(info);fail_convert=false;assert(service.audio_playback_queue_.empty());
 during_decode=[&]{++service.playback_generation_;};feed(info);
 assert(service.audio_playback_queue_.empty()&&!service.decode_in_flight_);
}
'''
        self.run_cpp(program)

    def test_capture_reuses_buffers_and_rejects_conversion_errors_or_wrong_rate(self):
        source=(AUDIO/'audio_service.cc').read_text()
        self.run_cpp(r'''
#define CONFIG_AUDIO_PRODUCT_QUALITY 1
#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <vector>
constexpr int ESP_AE_ERR_OK=0,AUDIO_POWER_CHECK_INTERVAL_MS=1000;
using esp_ae_sample_t=int16_t*;
void esp_timer_stop(void*){}void esp_timer_start_periodic(void*,unsigned){}
bool fail_size=false,fail_convert=false;
int esp_ae_rate_cvt_get_max_out_sample_num(void*,uint32_t n,uint32_t*out){*out=n*2/3;return fail_size?-1:0;}
int esp_ae_rate_cvt_process(void*,int16_t*,uint32_t,int16_t*out,uint32_t*n){
 if(fail_convert)return -1;
 std::fill(out,out+*n*2,123);return 0;
}
struct Codec{
 bool input_enabled(){return true;}void EnableInput(bool){}
 int input_sample_rate(){return 24000;}int input_channels(){return 2;}
 bool InputData(std::vector<int16_t>&p){std::fill(p.begin(),p.end(),1000);return true;}
};
struct AudioService{
 Codec codec;Codec*codec_=&codec;void*audio_power_timer_=nullptr,*input_resampler_=this;
 std::mutex input_resampler_mutex_;std::vector<int16_t>input_resample_scratch_;
 std::chrono::steady_clock::time_point last_input_time_;struct{int input_count=0;}debug_statistics_;
 bool ReadAudioData(std::vector<int16_t>&,int,int);
};
'''+queue_function(source,'bool AudioService::ReadAudioData(')+r'''
int main(){
 AudioService s;std::vector<int16_t>pcm;
 for(int i=0;i<10;++i){assert(s.ReadAudioData(pcm,16000,160)&&pcm.size()==320);}
 const auto capacity=pcm.capacity()+s.input_resample_scratch_.capacity();
 const auto*a=pcm.data();const auto*b=s.input_resample_scratch_.data();
 for(int i=0;i<100;++i){
  assert(s.ReadAudioData(pcm,16000,160)&&pcm.size()==320);
  assert(pcm.capacity()+s.input_resample_scratch_.capacity()==capacity);
  assert((pcm.data()==a&&s.input_resample_scratch_.data()==b)||
         (pcm.data()==b&&s.input_resample_scratch_.data()==a));
  for(int v:pcm)assert(v==123);
 }
 fail_size=true;assert(!s.ReadAudioData(pcm,16000,160));fail_size=false;
 fail_convert=true;assert(!s.ReadAudioData(pcm,16000,160));fail_convert=false;
 s.input_resampler_=nullptr;assert(!s.ReadAudioData(pcm,16000,160));
}
''')
