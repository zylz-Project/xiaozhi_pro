"""Production-code checks for ES8311 reference capture and local speech interruption."""
from pathlib import Path
import json
import unittest

from test_wzh_action_queue import queue_function
import test_firmware_runtime_optimizations as runtime_tests
import test_wzh_conversation_toggle as conversation_tests

ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / 'main/audio'
HEADER = '#include "' + str(AUDIO/'voice_barge_in.h') + '"\n' + r'''
#include <vector>
#include <cmath>
inline std::vector<int16_t> BargeWave(int amplitude,bool far=false,size_t samples=960){
 std::vector<int16_t>pcm(samples);
 for(size_t i=0;i<samples;++i){
  const double t=static_cast<double>(i)/16000;
  pcm[i]=static_cast<int16_t>(amplitude*(0.7*std::sin(6.283185307179586*(far?233:827)*t)
                  +0.3*std::sin(6.283185307179586*(far?571:1301)*t)));
 }
 return pcm;
}
inline void FeedBargeReference(VoiceBargeIn&detector,int64_t now,float rms=1000,
                              bool clipped=false,bool muted=false){
 detector.UpdateReference(rms,clipped,now,muted);
 auto reference=BargeWave(static_cast<int>(rms),true);std::vector<int16_t>mr(reference.size()*2);
 for(size_t i=0;i<reference.size();++i)mr[i*2+1]=reference[i];
 detector.FeedReference(mr.data(),reference.size());
}
'''



class VoiceBargeInTest(unittest.TestCase):
    run_cpp = runtime_tests.FirmwareRuntimeOptimizationsTest.run_cpp

    def test_post_aec_speech_duration_noise_guard_and_generation(self):
        self.run_cpp(HEADER + r'''
#include <cassert>
#include <vector>
int main(){
 VoiceBargeIn detector;std::vector<int16_t>quiet(960,0);auto speech=BargeWave(1500),low=BargeWave(50);
 int64_t now=0;
 auto observe=[&](const std::vector<int16_t>&pcm,bool vad){
  now+=60000;FeedBargeReference(detector,now);
  return detector.Observe(pcm.data(),pcm.size(),vad,now);
 };
 auto old=detector.Begin();
 for(int i=0;i<10;++i)assert(!observe(low,true)); // Settling plus low energy cannot interrupt.
 assert(!observe(speech,true));
 assert(!observe(quiet,false)); // A brief burst does not interrupt.
 assert(!observe(low,true)); // VAD alone is insufficient.
 for(int i=0;i<10;++i)assert(!observe(speech,false));
 assert(!observe(speech,true));assert(!observe(speech,true));assert(observe(speech,true));
 assert(detector.pending(old,now)&&!detector.Commit(old+1,now));
 assert(!observe(speech,true)); // One event per reply.
 assert(detector.Commit(old,now)&&!detector.monitoring());
 detector.Begin();detector.Cancel();auto current=detector.Begin();
 assert(current!=old&&!detector.pending(old,now)&&!detector.Commit(old,now));
 for(int i=0;i<10000;++i)assert(!observe(quiet,false));
 assert(!observe(speech,true));assert(!observe(speech,true));assert(observe(speech,true));
 BargeInReplyGate gate;assert(!gate.blocked());gate.Begin();assert(gate.blocked());
 gate.OnTranscript();assert(!gate.blocked());gate.Begin();gate.Cancel();assert(!gate.blocked());
}
''')

    def test_codec_stereo_capture_and_mono_playback_have_equal_frame_counts(self):
        source=(AUDIO/'codecs/es8311_audio_codec.cc').read_text()
        self.run_cpp(r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#define ESP_LOGW(...) ((void)0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) static_cast<void>(x)
constexpr int ESP_CODEC_DEV_OK=0;
std::vector<int16_t>physical;bool read_fail=false,write_fail=false;
int esp_codec_dev_write(void*,const void*data,int bytes){
 if(write_fail)return -1;
 auto*p=static_cast<const int16_t*>(data);physical.insert(physical.end(),p,p+bytes/2);return 0;
}
int esp_codec_dev_read(void*,void*dest,int bytes){
 if(read_fail)return -1;
 auto*p=static_cast<int16_t*>(dest);
 for(int i=0;i<bytes/2;i+=2){p[i]=123;p[i+1]=456;}return 0;
}
struct Es8311AudioCodec{
 bool input_enabled_=true,output_enabled_=true,input_reference_=true;void*dev_=this;
 int Read(int16_t*,int);int Write(const int16_t*,int);
};
''' + queue_function(source,'int Es8311AudioCodec::Read(')
            + queue_function(source,'int Es8311AudioCodec::Write(') + r'''
int main(){
 Es8311AudioCodec codec;
 for(int count:{1,239,240,241,1440,2880}){
  std::vector<int16_t>mono(count);for(int i=0;i<count;++i)mono[i]=i-100;
  physical.clear();assert(codec.Write(mono.data(),count)==count);
  assert(physical.size()==mono.size()*2);
  for(int i=0;i<count;++i)assert(physical[2*i]==mono[i]&&physical[2*i+1]==mono[i]);
  // 24000 mono frames become exactly 24000 two-slot I2S frames per second.
 }
 std::vector<int16_t>capture(480);assert(codec.Read(capture.data(),480)==480);
 for(int i=0;i<480;i+=2)assert(capture[i]==123&&capture[i+1]==456);
 read_fail=true;assert(codec.Read(capture.data(),480)==0);
 codec.input_enabled_=false;assert(codec.Read(capture.data(),480)==0);
 write_fail=true;assert(codec.Write(capture.data(),480)==0);write_fail=false;
 codec.input_reference_=false;physical.clear();assert(codec.Write(capture.data(),480)==480);
 assert(physical==capture); // Ordinary boards keep their original mono layout.
}
''')

    def test_delayed_playback_and_residual_echo_cannot_calibrate_on_silence(self):
        self.run_cpp(HEADER + r'''
#include <cassert>
#include <vector>
int main(){
 VoiceBargeIn detector;detector.Begin();int64_t now=0;
 auto frame=[&](int residual,int reference){
  now+=60000;FeedBargeReference(detector,now,reference);
  auto pcm=BargeWave(residual,residual<5000);
  return detector.Observe(pcm.data(),pcm.size(),true,now);
 };
 for(int i=0;i<40;++i)assert(!frame(1500,0)); // TTS start precedes actual PCM by 2.4 s.
 for(int i=0;i<10;++i)assert(!frame(1500,1000)); // Guard starts at actual playback.
 assert(detector.threshold()>=160&&detector.threshold()<=480);
 for(int i=0;i<100;++i)assert(!frame(i%2?1500:3000,1000)); // VAD sees residual TTS.
 assert(!frame(5000,1000));assert(!frame(5000,1000));assert(frame(5000,1000));
}
''')

    def test_reference_spike_loss_stalled_input_and_recovery_invalidate_confirmations(self):
        self.run_cpp(HEADER + r'''
#include <cassert>
#include <vector>
int main(){
 VoiceBargeIn detector;detector.Begin();int64_t now=0;std::vector<int16_t>pcm(960,2000);
 auto frame=[&](int reference,int value=2000){
  now+=60000;FeedBargeReference(detector,now,reference);pcm=BargeWave(value);
  return detector.Observe(pcm.data(),pcm.size(),true,now);
 };
 assert(!frame(1000)); // One nonzero reference frame must never arm a reply forever.
 for(int i=0;i<100;++i)assert(!frame(i%2?0:2)); // User's failing trace: reference 0/2.
 for(int i=0;i<10;++i)assert(!frame(1000,50));
 assert(!frame(1000));assert(!frame(1000));assert(frame(1000));
 auto token=detector.token();assert(detector.pending(token,now));
 now+=120001;
 assert(!detector.pending(token,now)&&!detector.Commit(token,now)); // No new input.
 detector.UpdateReference(1000,false,now);
 assert(!detector.pending(token,now)&&detector.token()!=token); // Recovery cannot revive it.
 for(int i=0;i<10;++i)assert(!frame(1000,50));
 assert(!frame(1000));assert(!frame(1000));assert(frame(1000));
 token=detector.token();now+=120001;detector.UpdateReference(0,false,now);
 assert(!detector.pending(token,now)&&!detector.Commit(token,now));
 assert(detector.monitoring()); // Keep recording; only reject unsafe interruption.
}
''')

    def test_clipping_and_muted_playback_require_live_capture(self):
        self.run_cpp(HEADER + r'''
#include <cassert>
#include <vector>
int main(){
 VoiceBargeIn detector;auto token=detector.Begin();int64_t now=0;
 std::vector<int16_t>pcm(960,2000);
 for(int i=0;i<50;++i){
  now+=60000;detector.UpdateReference(1000,true,now);
  assert(!detector.Observe(pcm.data(),pcm.size(),true,now));
 }
 assert(!detector.pending(token,now));detector.Cancel();token=detector.Begin();
 for(int i=0;i<5;++i){
  now+=60000;detector.UpdateReference(0,false,now,true);
  assert(!detector.Observe(pcm.data(),pcm.size(),true,now,true));
 }
 for(int i=0;i<3;++i){
  now+=60000;detector.UpdateReference(0,false,now,true);
  assert(detector.Observe(pcm.data(),pcm.size(),true,now,true)==(i==2));
 }
 assert(detector.pending(token,now,true));
 assert(!detector.pending(token,now+120001,true)); // Muting does not excuse dead capture.
}
''')

    def test_capture_detects_adc_clipping_before_rate_conversion(self):
        source=(AUDIO/'audio_service.cc').read_text()
        self.run_cpp('#define CONFIG_USE_VOICE_BARGE_IN 1\n'+HEADER+r'''
#include <cassert>
#include <chrono>
#include <mutex>
#include <vector>
#define CONFIG_USE_AUDIO_DEBUGGER 0
constexpr int TAG=0,AUDIO_POWER_CHECK_INTERVAL_MS=1000;
template<class...T>void Log(T...){ }
#define ESP_LOGI(...) Log(__VA_ARGS__)
int64_t clock_us=10000000;int64_t esp_timer_get_time(){return clock_us;}
void esp_timer_stop(int){}void esp_timer_start_periodic(int,int){}
using esp_ae_sample_t=int16_t*;
void esp_ae_rate_cvt_get_max_out_sample_num(void*,uint32_t n,uint32_t*out){*out=n*2/3;}
void esp_ae_rate_cvt_process(void*,int16_t*,uint32_t,int16_t*out,uint32_t*n){
 std::fill(out,out+*n*2,0); // A converter can hide full-scale raw ADC samples.
}
struct Codec{
 bool clipped=false,fail=false;bool input_enabled(){return true;}void EnableInput(bool){}
 int input_sample_rate(){return 24000;}int input_channels(){return 2;}
 bool input_reference(){return true;}int output_volume(){return 70;}
 bool InputData(std::vector<int16_t>&data){
  if(fail)return false;
  for(size_t i=0;i<data.size();i+=2){data[i]=clipped?32767:500;data[i+1]=1000;}return true;
 }
};
struct AudioService{
 Codec codec;Codec*codec_=&codec;int audio_power_timer_=0;
 void*input_resampler_=this;std::mutex input_resampler_mutex_,audio_queue_mutex_;
 VoiceBargeIn voice_barge_in_;
 std::atomic<bool>voice_detected_{false};
 uint32_t reference_log_frames_=0,reference_active_frames_=0,reference_clipped_frames_=0;
 float reference_peak_rms_=0,microphone_peak_rms_=0;
 std::chrono::steady_clock::time_point last_input_time_;struct{int input_count=0;}debug_statistics_;
 bool ReadAudioData(std::vector<int16_t>&,int,int);
};
'''+queue_function(source,'bool AudioService::ReadAudioData(')+r'''
int main(){
 AudioService service;service.voice_barge_in_.Begin();std::vector<int16_t>data;
 assert(service.ReadAudioData(data,16000,160)&&data.size()==320);
 assert(service.voice_barge_in_.ReferenceReady(clock_us));
 service.codec.clipped=true;clock_us+=10000;
 assert(service.ReadAudioData(data,16000,160));for(auto sample:data)assert(sample==0);
 assert(!service.voice_barge_in_.ReferenceReady(clock_us)&&service.reference_clipped_frames_==1);
 service.codec.fail=true;assert(!service.ReadAudioData(data,16000,160));
 assert(service.debug_statistics_.input_count==2);
}
''')

    def test_echo_matching_handles_delay_inversion_dc_fragmentation_and_ring_wrap(self):
        self.run_cpp(HEADER+r'''
#include <cassert>
int main(){
 EchoReferenceGuard guard;uint32_t random=123456;
 std::vector<int16_t>mr(20000*2);
 for(size_t i=0;i<20000;++i){
  random^=random<<13;random^=random>>17;random^=random<<5;
  mr[i*2+1]=static_cast<int>(random%12001)-6000;
 }
 size_t fed=0;
 while(fed<20000){size_t n=std::min<size_t>(127,20000-fed);guard.Feed(mr.data()+fed*2,n);fed+=n;}
 for(int delay:{0,512,2048,4096,7168}){
  std::vector<int16_t>echo(960);
  for(size_t i=0;i<echo.size();++i)echo[i]=-mr[(20000-delay-960+i)*2+1]/3+200;
  assert(guard.Correlation(echo.data(),echo.size())>.99f);
 }
 auto near=BargeWave(500);
 assert(guard.Correlation(near.data(),near.size())>=0&&guard.Correlation(near.data(),near.size())<.55f);
 for(size_t i=0;i<near.size();++i)near[i]+=mr[(20000-2048-960+i)*2+1]/40;
 assert(guard.Correlation(near.data(),near.size())<.55f); // Near speech plus a quieter echo.
 guard.Reset();assert(guard.Correlation(near.data(),near.size())<0);
 std::vector<int16_t>dc(960*2,1000);guard.Feed(dc.data(),960);
 assert(guard.Correlation(near.data(),near.size())<0); // DC is not an audio reference.
}
''')

    def test_loud_correlated_echo_does_not_raise_gate_above_quiet_independent_speech(self):
        self.run_cpp(HEADER+r'''
#include <cassert>
int main(){
 VoiceBargeIn detector;detector.Begin();int64_t now=0;
 auto echo=BargeWave(12000,true),near=BargeWave(500);
 for(int i=0;i<80;++i){
  now+=60000;FeedBargeReference(detector,now);
  assert(!detector.Observe(echo.data(),echo.size(),true,now));
 }
 assert(detector.correlation()>=.55f&&detector.threshold()<=480);
 for(int i=0;i<3;++i){
  now+=60000;FeedBargeReference(detector,now);
  assert(detector.Observe(near.data(),near.size(),true,now)==(i==2));
 }
 assert(detector.correlation()<.55f&&detector.pending(detector.token(),now));
}
''')

    def test_intermittent_clipping_and_phoneme_gaps_do_not_restart_settling_forever(self):
        self.run_cpp(HEADER+r'''
#include <cassert>
int main(){
 VoiceBargeIn detector;detector.Begin();int64_t now=0;
 auto echo=BargeWave(8000,true),near=BargeWave(500);
 for(int i=0;i<90;++i){
  now+=60000;FeedBargeReference(detector,now,1000,i%9==0);
  assert(!detector.Observe(echo.data(),echo.size(),true,now));
 }
 assert(detector.settling_ms()>=300);
 // A normal punctuation pause loses instantaneous reference but preserves the completed settling.
 for(int i=0;i<5;++i){
  now+=60000;FeedBargeReference(detector,now,0);
  assert(!detector.Observe(echo.data(),echo.size(),true,now));
 }
 assert(detector.settling_ms()>=300);
 for(int i=0;i<3;++i){
  now+=60000;FeedBargeReference(detector,now);
  assert(detector.Observe(near.data(),near.size(),true,now)==(i==2));
 }
}
''')

    def test_one_correlated_phoneme_does_not_erase_near_end_speech_but_sustained_echo_does(self):
        self.run_cpp(HEADER+r'''
#include <cassert>
int main(){
 VoiceBargeIn detector;detector.Begin();int64_t now=0;
 auto echo=BargeWave(8000,true),near=BargeWave(500);
 auto frame=[&](const std::vector<int16_t>&pcm){
  now+=60000;FeedBargeReference(detector,now);
  return detector.Observe(pcm.data(),pcm.size(),true,now);
 };
 for(int i=0;i<8;++i)assert(!frame(echo));
 assert(!frame(near));assert(!frame(near));assert(!frame(echo));assert(frame(near));
 detector.Cancel();detector.Begin();
 for(int i=0;i<8;++i)assert(!frame(echo));
 assert(!frame(near));assert(!frame(echo));assert(!frame(echo));
 assert(!frame(near));assert(!frame(near));assert(frame(near));
}
''')

    def test_compressed_preroll_is_bounded_ordered_and_never_uploads_before_commit(self):
        source=(AUDIO/'audio_service.cc').read_text()
        methods='\n'.join(queue_function(source,marker) for marker in (
            'bool AudioService::BeginVoiceInterruption(',
            'bool AudioService::IsVoiceInterruptionMonitoring(',
            'bool AudioService::IsVoiceInterruptionPending(',
            'bool AudioService::CommitVoiceInterruption(',
            'void AudioService::QueueVoicePacket(',
            'void AudioService::PushTaskToEncodeQueue('))
        self.run_cpp('#define CONFIG_USE_VOICE_BARGE_IN 1\n' + HEADER + r'''
#include <cassert>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "fixed_queue.h"
constexpr int MAX_ENCODE_TASKS_IN_QUEUE=2,MAX_TIMESTAMPS_IN_QUEUE=3;
#define ESP_LOGW(...) ((void)0)
int64_t esp_timer_get_time(){return 10000000;}
enum AudioTaskType{kAudioTaskTypeEncodeToSendQueue,kAudioTaskTypeEncodeToTestingQueue};
struct AudioTask{AudioTaskType type;std::vector<int16_t>pcm;uint32_t timestamp=0,capture_generation=0;};
struct AudioStreamPacket{int number=0;};
struct Codec{bool reference=true;bool input_reference(){return reference;}int output_volume(){return 70;}};
struct Engine{bool ready=true;bool CanMonitorVoiceInterruption(){return ready;}};
struct AudioService{
 Engine engine;Engine*audio_engine_=&engine;
 Codec codec;Codec*codec_=&codec;bool device_aec_enabled_=true,capture_enabled_=true,running=true;
 bool IsAudioProcessorRunning(){return running;}
 std::mutex audio_queue_mutex_;VoiceBargeIn voice_barge_in_;uint32_t capture_generation_=0;
 FixedQueue<AudioTask,2>audio_encode_queue_;
 FixedQueue<uint32_t,3>timestamp_queue_;std::condition_variable audio_queue_cv_;
 std::atomic<bool>voice_detected_{true};
 struct{uint32_t encode_drop_count=0;}debug_statistics_;int64_t last_encode_drop_log_time_=0;
 void PushTaskToEncodeQueue(AudioTaskType,std::vector<int16_t>&&);
 FixedQueue<std::unique_ptr<AudioStreamPacket>,40>audio_send_queue_;
 FixedQueue<std::unique_ptr<AudioStreamPacket>,12>barge_preroll_;
 struct{std::function<void()>on_send_queue_available;std::function<void(uint32_t)>on_voice_interruption;}callbacks_;
 bool BeginVoiceInterruption();bool IsVoiceInterruptionMonitoring();
 bool IsVoiceInterruptionPending(uint32_t);bool CommitVoiceInterruption(uint32_t);
 void QueueVoicePacket(std::unique_ptr<AudioStreamPacket>,uint32_t);
};
''' + methods + r'''
int main(){
 AudioService service;int notifications=0;
 service.callbacks_.on_send_queue_available=[&]{
  assert(service.audio_queue_mutex_.try_lock());service.audio_queue_mutex_.unlock();++notifications;
 };
 service.codec.reference=false;assert(!service.BeginVoiceInterruption());service.codec.reference=true;
 service.running=false;assert(!service.BeginVoiceInterruption());service.running=true;
 service.engine.ready=false;assert(!service.BeginVoiceInterruption());service.engine.ready=true;
 assert(service.BeginVoiceInterruption());auto generation=service.capture_generation_;
 auto token=service.voice_barge_in_.token();assert(service.BeginVoiceInterruption());
 assert(service.capture_generation_==generation&&service.voice_barge_in_.token()==token);
 for(int i=0;i<20;++i){auto packet=std::make_unique<AudioStreamPacket>();packet->number=i;
  service.QueueVoicePacket(std::move(packet),generation);}
 assert(notifications==0&&service.audio_send_queue_.empty()&&service.barge_preroll_.size()==12);
 int detections=0;
 service.callbacks_.on_voice_interruption=[&](uint32_t detected){
  assert(detected==token);assert(service.audio_queue_mutex_.try_lock());
  service.audio_queue_mutex_.unlock();++detections;
 };
 for(int i=0;i<20;++i)service.PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue,
                                                  BargeWave(2000));
 assert(detections==0); // Never arm on a missing DAC reference while playback is audible.
 FeedBargeReference(service.voice_barge_in_,esp_timer_get_time());
 for(int i=0;i<10;++i)service.PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue,
                                                 std::vector<int16_t>(960,50));
 for(int i=0;i<3;++i)service.PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue,
                                                 BargeWave(2000));
 assert(detections==1);
 assert(service.IsVoiceInterruptionPending(token));
 assert(!service.CommitVoiceInterruption(token+1)&&notifications==0);
 assert(service.CommitVoiceInterruption(token)&&notifications==1);
 assert(!service.IsVoiceInterruptionMonitoring()&&service.barge_preroll_.empty());
 auto packet=std::make_unique<AudioStreamPacket>();packet->number=20;
 service.QueueVoicePacket(std::move(packet),generation);
 for(int i=8;i<=20;++i){assert(service.audio_send_queue_.front()->number==i);service.audio_send_queue_.pop_front();}
 assert(service.audio_send_queue_.empty());
 assert(service.BeginVoiceInterruption());assert(!service.IsVoiceInterruptionPending(token));
 service.QueueVoicePacket(std::make_unique<AudioStreamPacket>(),generation); // Old encoder completion.
 assert(service.barge_preroll_.empty());
 service.capture_enabled_=false;
 service.QueueVoicePacket(std::make_unique<AudioStreamPacket>(),service.capture_generation_);
 assert(service.barge_preroll_.empty()&&!service.CommitVoiceInterruption(token));
}
''')

    def test_test_variant_is_opt_in_and_keeps_the_physical_board(self):
        config=json.loads((ROOT/'main/boards/wzh_tailRedPanda/config.json').read_text())
        builds={b['name']:b for b in config['builds']}
        ordinary=builds['wzh-tailredpanda'];trial=builds['wzh-tailredpanda-barge-in']
        self.assertIn('CONFIG_USE_VOICE_BARGE_IN=n',ordinary['sdkconfig_append'])
        self.assertIn('CONFIG_USE_VOICE_BARGE_IN=y',trial['sdkconfig_append'])
        self.assertEqual(ordinary['default_wake_word'],trial['default_wake_word'])
        self.assertEqual(config['type'],'wzh-tailredpanda')
        self.assertIn('CONFIG_USE_SERVER_AEC=n',trial['sdkconfig_append'])

    def application(self):
        program=conversation_tests.WzhConversationToggleTest.application(self)
        program=program.replace('uint32_t generation=0;', '''
    bool monitoring=false,interruption_pending=false,commit_allowed=true;uint32_t interruption_token=7;
    bool BeginVoiceInterruption(){
        if(!voice)return false;
        if(!monitoring){monitoring=true;interruption_pending=false;++interruption_token;}
        return true;
    }
    bool IsVoiceInterruptionMonitoring(){return monitoring;}
    bool IsVoiceInterruptionPending(uint32_t token){
        return monitoring&&interruption_pending&&token==interruption_token;
    }
    bool CommitVoiceInterruption(uint32_t token){
        if(!commit_allowed||!IsVoiceInterruptionPending(token))return false;
        monitoring=interruption_pending=false;send_packets=3;events.push_back("commit");return true;
    }
    uint32_t generation=0;''')
        program=program.replace('void EnableVoiceProcessing(bool enabled){voice=enabled;if(enabled)ResetDecoder();}',
            'void EnableVoiceProcessing(bool enabled){voice=processor_running=enabled;if(enabled)ResetDecoder();else monitoring=interruption_pending=false;}')
        program=program.replace('std::atomic<bool>interaction_stopped_{false};','''
    BargeInReplyGate barge_in_reply_gate_;
    std::atomic<uint32_t>pending_voice_interruption_{0};
    void HandleVoiceInterruptionEvent();
    std::atomic<bool>interaction_stopped_{false};''')
        program=program.replace('void SendStartListening(int){++listens;}',
            'void SendStartListening(int){++listens;events.push_back("listen");}')
        program=program.replace('void AbortSpeaking(int){++aborts;}',
            'void AbortSpeaking(int){++aborts;aborted_=true;events.push_back("abort");}')
        program=program.replace('enum DeviceState{kDeviceStateIdle',
            'enum DeviceState{kDeviceStateUnknown,kDeviceStateIdle')
        program=program.replace('constexpr int SPEAKING=2,LISTENING=3;',
            'constexpr int STANDBY=1,SPEAKING=2,LISTENING=3;')
        program=program.replace('void SetStatus(int){}',
            'void SetStatus(int){}void ClearChatMessages(){messages=0;}')
        source=(ROOT/'main/application.cc').read_text()
        begin=source.index('        case kDeviceStateUnknown:',source.index('void Application::HandleStateChangedEvent'))
        end=source.index('        case kDeviceStateConnecting:',begin)
        idle=source[begin:end]
        program=program.replace('void SpeakingStateEvent(){',
            'void IdleStateEvent(){auto*display=Board::GetInstance().GetDisplay();switch(state){'+idle+'default:break;}}\n    void SpeakingStateEvent(){')
        return '#define CONFIG_USE_VOICE_BARGE_IN 1\n'+HEADER+program+queue_function(
            source,'void Application::HandleVoiceInterruptionEvent(')

    def test_application_aborts_locally_preserves_capture_and_fences_old_reply(self):
        self.run_cpp(self.application()+r'''
int main(){
 Application app;app.Install();app.protocol.opened=true;app.state=kDeviceStateListening;app.StartListeningAudio();
 app.Tts(R"({"type":"tts","state":"start"})");app.Drain();app.SpeakingStateEvent();
 assert(app.state==kDeviceStateSpeaking&&app.audio_service_.monitoring&&app.audio_service_.voice);
 app.audio_service_.interruption_pending=true;
 app.pending_voice_interruption_=app.audio_service_.interruption_token;
 auto generation=app.interaction_generation_.load();events.clear();
 app.HandleVoiceInterruptionEvent();
 assert(app.state==kDeviceStateListening&&app.aborts==1&&app.barge_in_reply_gate_.blocked());
 assert(app.interaction_generation_==generation+1&&app.audio_service_.voice);
 assert(app.audio_service_.send_packets==3&&!app.play_popup_on_listening_);
 assert(events.size()>=4&&events.front()=="commit"&&events[1]=="reset"&&events[events.size()-2]=="abort"&&events.back()=="listen");
 int resets=app.audio_service_.resets;
 app.ListeningStateEvent();assert(app.audio_service_.resets==resets); // Preserve first syllables.
 int emotions=Board::GetInstance().display.emotions;
 app.Tts(R"({"type":"tts","state":"start"})");
 app.Tts(R"({"type":"tts","state":"sentence_start","text":"old sentence"})");
 app.Tts(R"({"type":"tts","state":"stop"})");
 app.Tts(R"({"type":"llm","emotion":"happy"})");
 app.protocol.audio(std::make_unique<AudioStreamPacket>());app.Drain();
 assert(app.state==kDeviceStateListening&&app.audio_service_.packets==0);
 assert(Board::GetInstance().display.emotions==emotions);
 app.Tts(R"({"type":"stt","text":""})");assert(app.barge_in_reply_gate_.blocked());
 app.Tts(R"({"type":"stt","text":"换个问题"})");assert(!app.barge_in_reply_gate_.blocked());
 app.Tts(R"({"type":"tts","state":"start"})");
 app.protocol.audio(std::make_unique<AudioStreamPacket>());app.Drain();
 assert(app.state==kDeviceStateSpeaking&&app.audio_service_.packets==1);
}
''',use_json=True)

    def test_stale_events_stop_and_queued_old_start_cannot_break_a_new_reply(self):
        self.run_cpp(self.application()+r'''
int main(){
 Application app;app.Install();app.protocol.opened=true;app.state=kDeviceStateListening;app.StartListeningAudio();
 app.Tts(R"({"type":"tts","state":"start"})");app.Drain();
 app.audio_service_.interruption_pending=true;
 auto token=app.audio_service_.interruption_token;
 app.pending_voice_interruption_=token-1;app.HandleVoiceInterruptionEvent();
 assert(app.aborts==0&&!app.barge_in_reply_gate_.blocked());
 app.interaction_stopped_=true;app.pending_voice_interruption_=token;
 app.HandleVoiceInterruptionEvent();assert(app.aborts==0);app.interaction_stopped_=false;
 app.protocol.opened=false;app.HandleVoiceInterruptionEvent();assert(app.aborts==0);app.protocol.opened=true;
 // Reference may expire between precheck and locked acceptance. Nothing should stop in that case.
 app.audio_service_.commit_allowed=false;int resets=app.audio_service_.resets;
 app.HandleVoiceInterruptionEvent();
 assert(app.aborts==0&&app.audio_service_.resets==resets&&!app.barge_in_reply_gate_.blocked());
 app.audio_service_.commit_allowed=true;
 // A start queued before cancellation must not clear a later reply's early audio.
 app.Tts(R"({"type":"tts","state":"start"})");
 app.HandleVoiceInterruptionEvent();assert(app.aborts==1);
 app.Tts(R"({"type":"stt","text":"new question"})");
 app.Tts(R"({"type":"tts","state":"start"})");
 app.protocol.audio(std::make_unique<AudioStreamPacket>());
 app.Drain();assert(app.state==kDeviceStateSpeaking&&app.audio_service_.packets==1);
 app.barge_in_reply_gate_.Begin();app.StopInteraction();assert(!app.barge_in_reply_gate_.blocked());
 app.Drain();app.IdleStateEvent();
 assert(app.state==kDeviceStateIdle&&!app.barge_in_reply_gate_.blocked()&&!app.audio_service_.voice);
 app.HandleVoiceInterruptionEvent();assert(app.state==kDeviceStateIdle);
}
''',use_json=True)

    def test_monitor_survives_short_reply_and_physical_tail_then_returns_to_auto_listening(self):
        self.run_cpp(self.application()+r'''
int main(){
 Application app;app.Install();app.protocol.opened=true;app.state=kDeviceStateListening;app.StartListeningAudio();
 app.Tts(R"({"type":"tts","state":"start"})");
 app.protocol.audio(std::make_unique<AudioStreamPacket>());
 app.Tts(R"({"type":"tts","state":"stop"})");app.Drain();
 assert(app.state==kDeviceStateListening&&app.audio_service_.monitoring);
 app.audio_service_.playback_idle=false;app.ListeningStateEvent();
 assert(app.pending_listening_start_&&app.audio_service_.monitoring&&app.audio_service_.voice);
 app.audio_service_.interruption_pending=true;
 app.pending_voice_interruption_=app.audio_service_.interruption_token;
 app.HandleVoiceInterruptionEvent();assert(app.aborts==1&&app.state==kDeviceStateListening);
 Application ordinary;ordinary.Install();ordinary.protocol.opened=true;ordinary.state=kDeviceStateListening;ordinary.StartListeningAudio();
 ordinary.Tts(R"({"type":"tts","state":"start"})");ordinary.Drain();
 ordinary.Tts(R"({"type":"tts","state":"stop"})");ordinary.Drain();
 ordinary.audio_service_.playback_idle=true;ordinary.ListeningStateEvent();
 assert(!ordinary.audio_service_.monitoring&&ordinary.audio_service_.voice);
 assert(!ordinary.pending_listening_start_&&ordinary.protocol.listens==2);
}
''',use_json=True)

    def test_shared_abort_and_auto_listen_messages_keep_both_transport_contracts(self):
        source=(ROOT/'main/protocols/protocol.cc').read_text()
        methods=queue_function(source,'void Protocol::SendAbortSpeaking(')+queue_function(
            source,'void Protocol::SendStartListening(')
        self.run_cpp(r'''
#include <cassert>
#include <string>
#include <vector>
#include "cJSON.h"
using AbortReason=int;using ListeningMode=int;
constexpr int kAbortReasonNone=0,kAbortReasonWakeWordDetected=1;
constexpr int kListeningModeRealtime=2,kListeningModeAutoStop=1;
struct Protocol{
 std::string session_id_="session";std::vector<std::string>messages;
 bool SendText(const std::string&s){messages.push_back(s);return true;}
 void SendAbortSpeaking(AbortReason);void SendStartListening(ListeningMode);
};
'''+methods+r'''
int main(){
 for(int transport=0;transport<2;++transport){
  Protocol protocol;protocol.SendAbortSpeaking(kAbortReasonNone);
  protocol.SendStartListening(kListeningModeAutoStop);
  assert(protocol.messages.size()==2);
  auto*abort=cJSON_Parse(protocol.messages[0].c_str());auto*listen=cJSON_Parse(protocol.messages[1].c_str());
  assert(std::string(cJSON_GetObjectItem(abort,"type")->valuestring)=="abort");
  assert(!cJSON_GetObjectItem(abort,"reason"));
  assert(std::string(cJSON_GetObjectItem(listen,"mode")->valuestring)=="auto");
  assert(std::string(cJSON_GetObjectItem(listen,"state")->valuestring)=="start");
  cJSON_Delete(abort);cJSON_Delete(listen);
 }
}
''',use_json=True)

    def test_codec_open_uses_both_capture_slots_and_reopens_without_a_second_device(self):
        source=(AUDIO/'codecs/es8311_audio_codec.cc').read_text()
        self.run_cpp(r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#define ESP_ERROR_CHECK(expr) assert((expr)==0)
constexpr int ESP_CODEC_DEV_TYPE_IN_OUT=3,GPIO_NUM_NC=-1;
struct esp_codec_dev_cfg_t{int dev_type;void*codec_if;void*data_if;};
struct esp_codec_dev_sample_info_t{
 int bits_per_sample;uint8_t channel;int channel_mask;uint32_t sample_rate;int mclk_multiple;
};
int creates=0,opens=0,closes=0,deletes=0;esp_codec_dev_sample_info_t configured{};
void*esp_codec_dev_new(const esp_codec_dev_cfg_t*cfg){
 assert(cfg->dev_type==3);++creates;return &creates;
}
int esp_codec_dev_open(void*,esp_codec_dev_sample_info_t*fs){configured=*fs;++opens;return 0;}
int esp_codec_dev_set_in_gain(void*,float){return 0;}
int esp_codec_dev_set_out_vol(void*,int){return 0;}
int esp_codec_dev_close(void*){++closes;return 0;}
void esp_codec_dev_delete(void*){++deletes;}
int mux=0,writes=0;
int esp_codec_dev_write_reg(void*,int reg,int value){assert(reg==0x44);mux=value;++writes;return 0;}
int esp_codec_dev_read_reg(void*,int reg,int*value){*value=reg==0x44?mux:0x0c;return 0;}
#define ESP_LOGI(...) ((void)0)
void gpio_set_level(int,int){}
struct Es8311AudioCodec{
 bool input_enabled_=true,output_enabled_=true,pa_inverted_=false,input_reference_=false;
 void*dev_=nullptr;void*codec_if_=this;void*data_if_=this;
 int input_channels_=1,input_sample_rate_=24000,output_volume_=70,pa_pin_=GPIO_NUM_NC;
 float input_gain_=36;
 void UpdateDeviceState();
};
''' + queue_function(source,'void Es8311AudioCodec::UpdateDeviceState(') + r'''
int main(){
 for(int channels:{1,2}){
  Es8311AudioCodec codec;codec.input_channels_=channels;codec.input_reference_=channels==2;
  int start=creates;codec.UpdateDeviceState();assert(creates==start+1);
  assert(configured.channel==channels&&configured.bits_per_sample==16&&configured.sample_rate==24000);
  codec.output_enabled_=false;codec.UpdateDeviceState();assert(creates==start+1&&codec.dev_);
  codec.input_enabled_=false;codec.UpdateDeviceState();assert(!codec.dev_);
  codec.output_enabled_=true;codec.UpdateDeviceState();assert(creates==start+2&&codec.dev_);
  assert(configured.channel==channels);
  codec.output_enabled_=false;codec.UpdateDeviceState();
 }
 assert(creates==4&&opens==4&&closes==4&&deletes==4);
 assert(writes==2&&mux==0x58); // Reapply ADCL/DACR on both first open and resume.
}
''')

    def test_afe_publishes_current_vad_before_pcm_across_session_reset(self):
        source=(AUDIO/'engines/afe_audio_engine.cc').read_text()
        self.run_cpp(r'''
#define CONFIG_USE_VOICE_BARGE_IN 1
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
constexpr int VAD_SPEECH=1,VAD_SILENCE=0;
struct afe_fetch_result_t{int16_t*data;int data_size;int vad_state;};
struct AfeAudioEngine{
 std::atomic<bool>output_reset_pending_{false};std::vector<int16_t>output_buffer_;
 std::atomic<uint32_t>control_generation_{0};
 int frame_samples_=960;
 std::function<void(bool)>vad_state_change_callback_;
 std::function<void(std::vector<int16_t>&&)>output_callback_;
 void HandleVoiceResult(const afe_fetch_result_t*,uint32_t);
};
''' + queue_function(source,'void AfeAudioEngine::HandleVoiceResult(') + r'''
int main(){
 AfeAudioEngine engine;bool current=false;int states=0,packets=0;
 engine.vad_state_change_callback_=[&](bool speaking){current=speaking;++states;};
 engine.output_callback_=[&](std::vector<int16_t>&&pcm){assert(current);assert(pcm.size()==960);++packets;};
 std::vector<int16_t>pcm(512,1000);afe_fetch_result_t speech{pcm.data(),1024,VAD_SPEECH};
 engine.HandleVoiceResult(&speech,0);engine.HandleVoiceResult(&speech,0);
 assert(states==2&&packets==1&&current);
 engine.output_reset_pending_=true;
 engine.output_callback_=[&](std::vector<int16_t>&&fresh){
  assert(!current&&fresh.size()==960);for(auto sample:fresh){assert(sample==42);}++packets;
 };
 std::vector<int16_t>fresh(960,42);afe_fetch_result_t silence{fresh.data(),1920,VAD_SILENCE};
 engine.HandleVoiceResult(&silence,0);assert(states==3&&packets==2&&!current);
 // Repeated silence must still overwrite a stale AudioService VAD latch.
 current=true;engine.HandleVoiceResult(&silence,0);assert(states==4&&!current);
 engine.control_generation_=1;engine.HandleVoiceResult(&speech,0);
 assert(states==4&&packets==3); // Old fetch generation cannot publish fresh-session PCM/VAD.
}
''')
