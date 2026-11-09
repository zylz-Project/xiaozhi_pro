"""Speaker protection trims sub-bass and caps near-full-scale peaks pre-DAC."""
from pathlib import Path
import unittest

import test_wzh_fluent_motion as motion_tests
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT/'main/audio/speaker_protection.h'


class SpeakerProtectionTest(unittest.TestCase):
    def test_loudness_makeup_raises_quiet_audio_and_keeps_extreme_pcm_bounded(self):
        self.run_program(r'''
#include <cassert>
double Energy(const std::vector<int16_t>&pcm,size_t begin){
 double value=0;for(size_t i=begin;i<pcm.size();++i)value+=double(pcm[i])*pcm[i];return value;
}
int main(){
 for(int rate:{8000,12000,16000,24000,48000})for(bool clarity:{false,true}){
  SpeakerProtection before,after;before.Configure(rate,clarity,clarity);
  after.Configure(rate,clarity,clarity,9.0f);
  std::vector<int16_t>quiet(rate);
  for(int i=0;i<rate;++i)quiet[i]=std::lrint(500*std::sin(6.283185307179586*1000*i/rate));
  auto boosted=quiet;before.Process(quiet);after.Process(boosted);
  double increase=10*std::log10(Energy(boosted,rate/2)/Energy(quiet,rate/2));
  assert(increase>8.95&&increase<9.05);
  std::vector<int16_t>extreme(rate);uint32_t random=17;
  for(int i=0;i<rate;++i){random=random*1664525U+1013904223U;extreme[i]=int16_t(random>>16);}
  after.Reset();const auto*storage=extreme.data();auto capacity=extreme.capacity();
  after.Process(extreme,3.9810717f); // +12 dB Opus header gain plus the new makeup.
  assert(extreme.data()==storage&&extreme.capacity()==capacity);
  for(int value:extreme)assert(std::abs(value)<=27525);
 }
}
''')

    def test_wzh_12db_louder_than_previous_profile_and_bounded(self):
        self.run_program(r'''
#include <cassert>
int main(){
 for(int rate:{8000,16000,24000,48000})for(bool clarity:{false,true}){
  std::vector<int16_t> input(rate);
  for(int i=0;i<rate;++i)input[i]=std::lrint(300*std::sin(6.283185307179586*1000*i/rate));
  auto previous=input, louder=input;SpeakerProtection a,b;
  a.Configure(rate,clarity,clarity,9);b.Configure(rate,clarity,clarity,12);
  a.Process(previous);b.Process(louder);double x=0,y=0;
  for(int i=rate/2;i<rate;++i){x+=double(previous[i])*previous[i];y+=double(louder[i])*louder[i];}
  assert(10*std::log10(y/x)>2.95&&10*std::log10(y/x)<3.05);
  uint32_t random=42;for(auto& v:input){random=random*1664525U+1013904223U;v=int16_t(random>>16);}
  auto reference=input;b.Reset();b.Process(reference,3.9810717f);
  b.Reset();std::vector<int16_t> chunks;
  for(size_t pos=0;pos<input.size();){size_t n=std::min<size_t>(1+pos%137,input.size()-pos);
   std::vector<int16_t> part(input.begin()+pos,input.begin()+pos+n);b.Process(part,3.9810717f);
   chunks.insert(chunks.end(),part.begin(),part.end());pos+=n;}
  assert(reference==chunks);for(auto v:reference)assert(std::abs(int(v))<=27525);
 }
}
''')

    def test_loudness_fragmentation_reset_and_disable_preserve_stream_boundaries(self):
        self.run_program(r'''
#include <cassert>
int main(){
 for(int rate:{8000,16000,24000,48000})for(bool clarity:{false,true}){
  std::vector<int16_t>signal(rate);uint32_t random=83;
  for(auto&value:signal){random=random*1664525U+1013904223U;value=int16_t(random>>16);}
  SpeakerProtection whole,parts;whole.Configure(rate,clarity,false,9);parts.Configure(rate,clarity,false,9);
  auto expected=signal;whole.Process(expected);std::vector<int16_t>actual;
  for(size_t pos=0;pos<signal.size();){
   size_t count=std::min<size_t>(1+pos%131,signal.size()-pos);
   std::vector<int16_t>chunk(signal.begin()+pos,signal.begin()+pos+count);
   parts.Process(chunk);actual.insert(actual.end(),chunk.begin(),chunk.end());pos+=count;
  }
  assert(actual==expected);
  parts.Reset();std::vector<int16_t>silence(rate/10,0);parts.Process(silence);
  for(int value:silence)assert(value==0);
  parts.Configure(rate,clarity,clarity,0);SpeakerProtection original;original.Configure(rate,clarity,clarity);
  auto a=signal,b=signal;parts.Process(a);original.Process(b);assert(a==b);
 }
}
''')

    def test_loudness_keeps_one_ms_latency_and_does_not_raise_gain_during_silence(self):
        self.run_program(r'''
#include <cassert>
int main(){
 for(int rate:{8000,16000,24000,48000}){
  SpeakerProtection protection;protection.Configure(rate,false,false,9);
  std::vector<int16_t>pulse(rate/10,0);const int onset=rate/100,delay=rate/1000;
  pulse[onset]=32767;protection.Process(pulse);
  for(int i=0;i<onset+delay;++i)assert(pulse[i]==0);
  assert(std::abs(int(pulse[onset+delay]))>1000);
  protection.Reset();std::vector<int16_t>silence(rate*2,0);protection.Process(silence);
  for(int value:silence)assert(value==0);
  // Fixed makeup does not turn into an AGC that keeps amplifying background.
  std::vector<int16_t>low(rate*2);
  for(size_t i=0;i<low.size();++i)low[i]=std::lrint(40*std::sin(6.283185307179586*1000*i/rate));
  protection.Process(low);double early=0,late=0;
  for(int i=rate/2;i<rate;++i){early+=double(low[i])*low[i];late+=double(low[i+rate])*low[i+rate];}
  assert(std::abs(late/early-1)<.001);
 }
}
''')

    def test_codec_routes_makeup_through_every_existing_output_configuration(self):
        source=(ROOT/'main/audio/audio_codec.cc').read_text()
        method=queue_function(source,'void AudioCodec::EnableSpeakerProtection(')
        for quality in [0,1]:
            for clarity,smooth in [(0,0),(1,0),(1,1)]:
                with self.subTest(quality=quality,clarity=clarity,smooth=smooth):
                    self.run_program(f'''
#define CONFIG_AUDIO_PRODUCT_QUALITY {quality}
#define CONFIG_AUDIO_SPEAKER_VOICE_CLARITY {clarity}
#define CONFIG_AUDIO_SPEAKER_SMOOTH_LIMITER {smooth}
'''+r'''
#include <cassert>
#define ESP_LOGI(...) ((void)0)
struct CapturedProtection{
 int calls=0;float makeup=0;bool clarity=false,smooth=false;
 void Configure(int rate,bool c,bool s,float gain){assert(rate==24000);++calls;clarity=c;smooth=s;makeup=gain;}
};
struct AudioCodec{
 bool speaker_protection_enabled_=false;int output_sample_rate_=24000;
 float speaker_makeup_gain_db_=9;CapturedProtection speaker_protection_;
 std::vector<int16_t>finish_padding_;void EnableSpeakerProtection(bool);
};
'''+method+r'''
int main(){
 AudioCodec codec;codec.EnableSpeakerProtection(true);
 assert(codec.speaker_protection_enabled_&&codec.speaker_protection_.calls==1);
 assert(codec.speaker_protection_.makeup==9);
 assert(codec.speaker_protection_.clarity==bool(CONFIG_AUDIO_SPEAKER_VOICE_CLARITY));
 assert(codec.speaker_protection_.smooth==bool(CONFIG_AUDIO_SPEAKER_SMOOTH_LIMITER));
 codec.EnableSpeakerProtection(false);assert(!codec.speaker_protection_enabled_&&codec.speaker_protection_.calls==1);
}
''')

    def test_smoothed_attack_reduces_pre_transient_steps_without_extra_latency(self):
        self.run_program(r'''
#include <cassert>
#include <cmath>
#include <vector>
int main(){
 for(int rate:{8000,12000,16000,24000,48000}){
  const int delay=std::lround(rate*.001),spike=rate/5;
  std::vector<int16_t> signal(rate/2);
  for(size_t i=0;i<signal.size();++i)
   signal[i]=std::lrint(.15*32767*std::cos(6.283185307179586*200*(i+delay)/rate));
  auto unmodulated=signal;SpeakerProtection reference;reference.Configure(rate,true);reference.Process(unmodulated);
  signal[spike]=32767;
  auto abrupt=signal,smooth=signal;
  SpeakerProtection old_path,new_path;old_path.Configure(rate,true);new_path.Configure(rate,true,true);
  old_path.Process(abrupt);new_path.Process(smooth);
  int old_step=0,new_step=0;
  // Before the delayed transient arrives, sudden gain steps modulate a quiet
  // underlying tone. The FIR must spread this attack across the existing delay.
  for(int i=spike;i<spike+delay;++i){
   old_step=std::max(old_step,std::abs((int(abrupt[i])-unmodulated[i])-(int(abrupt[i-1])-unmodulated[i-1])));
   new_step=std::max(new_step,std::abs((int(smooth[i])-unmodulated[i])-(int(smooth[i-1])-unmodulated[i-1])));
  }
  assert(old_step>100&&new_step<old_step*.5);
  for(int sample:smooth)assert(sample<=27525&&sample>=-27525);
  // Quiet material is sample-aligned and effectively unity through the new FIR.
  signal[spike]=0;for(auto& v:signal)v/=10;
  old_path.Reset();new_path.Reset();abrupt=signal;smooth=signal;
  old_path.Process(abrupt);new_path.Process(smooth);
  for(size_t i=0;i<signal.size();++i)assert(std::abs(int(abrupt[i])-smooth[i])<=1);
 }
}
''')

    def test_smoothed_music_bounds_fragmentation_reset_and_reconfiguration(self):
        self.run_program(r'''
#include <cassert>
#include <cmath>
#include <vector>
int main(){
 for(int rate:{8000,12000,16000,24000,48000}){
  std::vector<int16_t> signal(rate*3);uint32_t random=19;
  for(size_t i=0;i<signal.size();++i){
   random=random*1664525U+1013904223U;signal[i]=static_cast<int16_t>(random>>16);
   if(i%11==0)signal[i]=32767;
   if(i%17==0)signal[i]=-32768;
  }
  SpeakerProtection whole,fragments;whole.Configure(rate,true,true);fragments.Configure(rate,true,true);
  auto expected=signal;whole.Process(expected);std::vector<int16_t> joined;
  for(size_t start=0;start<signal.size();){
   size_t n=std::min<size_t>(1+start%173,signal.size()-start);
   std::vector<int16_t> packet(signal.begin()+start,signal.begin()+start+n);
   const auto* address=packet.data();auto capacity=packet.capacity();fragments.Process(packet);
   assert(packet.data()==address&&packet.capacity()==capacity);
   joined.insert(joined.end(),packet.begin(),packet.end());start+=n;
  }
  assert(joined==expected);for(int v:joined)assert(v<=27525&&v>=-27525);
  fragments.Reset();std::vector<int16_t> silence(1000,0);fragments.Process(silence);
  for(int v:silence)assert(v==0);
  fragments.Configure(rate,true,false);SpeakerProtection original;original.Configure(rate,true);
  auto a=signal,b=signal;fragments.Process(a);original.Process(b);assert(a==b);
 }
}
''')

    def test_clarity_preserves_speech_level_and_recovers_limiter_headroom(self):
        self.run_program(r'''
#include <cassert>
#include <cmath>
#include <vector>

double Rms(const std::vector<int16_t>& pcm, size_t start) {
 double energy=0;
 for(size_t i=start;i<pcm.size();++i)energy+=double(pcm[i])*pcm[i];
 return std::sqrt(energy/(pcm.size()-start));
}
std::vector<int16_t> Tone(int rate,double hz,double amplitude) {
 std::vector<int16_t> pcm(rate);
 for(int i=0;i<rate;++i)pcm[i]=std::lrint(amplitude*32767*std::sin(6.283185307179586*hz*i/rate));
 return pcm;
}
int main(){
 for(int rate:{8000,16000,24000,48000}){
  for(double hz:{300.0,1000.0,2500.0}){
   SpeakerProtection original,clarity;
   original.Configure(rate);clarity.Configure(rate,true);
   auto before=Tone(rate,hz,.1),after=before;
   original.Process(before);clarity.Process(after);
   double ratio=Rms(after,rate/2)/Rms(before,rate/2);
   if(hz==300)assert(ratio>.93&&ratio<.98);
   if(hz==1000)assert(ratio>1.02&&ratio<1.18);
   if(hz==2500)assert(ratio>1.30&&ratio<1.35);
  }
  // Speech-band mixture at normal level: do not buy clarity by making it quiet.
  std::vector<int16_t> voice(rate);
  for(int i=0;i<rate;++i){
   double t=6.283185307179586*i/rate;
   voice[i]=std::lrint(32767*(.06*std::sin(200*t)+.04*std::sin(600*t)
                      +.05*std::sin(1400*t)+.03*std::sin(2800*t)));
  }
  SpeakerProtection before,after;before.Configure(rate);after.Configure(rate,true);
  auto enhanced=voice;before.Process(voice);after.Process(enhanced);
  assert(Rms(enhanced,rate/2)>Rms(voice,rate/2)*.97);

  // Dense full-scale input used to receive both independent attenuation gains.
  SpeakerProtection loud_before,loud_after;
  loud_before.Configure(rate);loud_after.Configure(rate,true);
  auto loud=Tone(rate,1000,1),improved=loud;
  loud_before.Process(loud);loud_after.Process(improved);
  assert(Rms(improved,rate/2)>Rms(loud,rate/2)*1.15);
  for(int sample:improved)assert(sample<=27525&&sample>=-27525);
 }
}
''')

    def test_clarity_ramp_is_short_and_does_not_restart_at_packet_boundaries(self):
        self.run_program(r'''
#include <cassert>
#include <cmath>
#include <vector>
int main(){
 for(int rate:{8000,16000,24000,48000}){
  std::vector<int16_t> signal(rate/5);
  for(size_t i=0;i<signal.size();++i)
   signal[i]=std::lrint(3000*std::cos(6.283185307179586*1000*i/rate));
  SpeakerProtection ramp,warm;
  ramp.Configure(rate,true);warm.Configure(rate,true);
  std::vector<int16_t> silence(rate/10,0);warm.Process(silence);
  auto faded=signal,direct=signal;ramp.Process(faded);warm.Process(direct);
  const size_t delay=std::lround(rate*.001);
  assert(faded[delay]==0&&std::abs(int(direct[delay]))>1000);
  const size_t fade=rate/200;
  for(size_t i=delay+fade;i<signal.size();++i)assert(faded[i]==direct[i]);
  SpeakerProtection fragmented;fragmented.Configure(rate,true);
  std::vector<int16_t> joined;
  for(size_t pos=0;pos<signal.size();){
   size_t count=std::min<size_t>(1+pos%173,signal.size()-pos);
   std::vector<int16_t> chunk(signal.begin()+pos,signal.begin()+pos+count);
   fragmented.Process(chunk);joined.insert(joined.end(),chunk.begin(),chunk.end());pos+=count;
  }
  assert(joined==faded);
 }
}
''')

    def test_clarity_extreme_pcm_remains_bounded_and_reset_clears_all_state(self):
        self.run_program(r'''
#include <cassert>
#include <vector>
int main(){
 for(int rate:{8000,16000,24000,48000}){
  SpeakerProtection processor;processor.Configure(rate,true);
  std::vector<int16_t> pcm(rate);
  uint32_t random=9;
  for(size_t i=0;i<pcm.size();++i){
   random=random*1664525U+1013904223U;pcm[i]=static_cast<int16_t>(random>>16);
   if(i%5==0)pcm[i]=32767;
   if(i%7==0)pcm[i]=-32768;
  }
  const auto* storage=pcm.data();const auto capacity=pcm.capacity();
  processor.Process(pcm);
  assert(pcm.data()==storage&&pcm.capacity()==capacity&&pcm.size()==size_t(rate));
  for(int sample:pcm)assert(sample<=27525&&sample>=-27525);
  processor.Reset();std::vector<int16_t> silence(1000,0);processor.Process(silence);
  for(auto sample:silence)assert(sample==0);
  // Turning the optional profile off must clear its filter and ramp state.
  processor.Configure(rate);SpeakerProtection reference;reference.Configure(rate);
  std::vector<int16_t> a(1000,100),b=a;processor.Process(a);reference.Process(b);assert(a==b);
 }
}
''')

    def test_high_pass_limiter_lookahead_and_unity_for_quiet_audio(self):
        motion_tests.WzhFluentMotionTest.run_cpp(self,
            '#include "'+str(HEADER)+r'"' + r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace {
constexpr int kRate = 24000;
constexpr float kPi = 3.14159265358979323846f;

std::vector<int16_t> Tone(float hz, float amplitude, int count) {
    std::vector<int16_t> pcm(count);
    for (int i = 0; i < count; ++i) {
        pcm[i] = static_cast<int16_t>(
            std::lrintf(amplitude * std::sin(2.0f * kPi * hz * i / kRate) * 32767.0f));
    }
    return pcm;
}

float Rms(const std::vector<int16_t>& pcm, size_t from) {
    double sum = 0;
    for (size_t i = from; i < pcm.size(); ++i) {
        const double v = pcm[i];
        sum += v * v;
    }
    return static_cast<float>(std::sqrt(sum / (pcm.size() - from)));
}

int Peak(const std::vector<int16_t>& pcm, size_t from) {
    int peak = 0;
    for (size_t i = from; i < pcm.size(); ++i) {
        peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
    }
    return peak;
}
}  // namespace

int main() {
    // 40 Hz (unreproducible by the toy speaker) drops well below the passband.
    SpeakerProtection bass;
    bass.Configure(kRate);
    auto low = Tone(40.0f, 0.5f, kRate / 2);
    const float low_input_rms = Rms(low, 0);
    bass.Process(low);
    const float low_output_rms = Rms(low, kRate / 10);
    assert(low_output_rms < low_input_rms * 0.2f);

    // 1 kHz remains essentially untouched at a quiet level.
    SpeakerProtection quiet;
    quiet.Configure(kRate);
    auto mid = Tone(1000.0f, 0.1f, kRate / 2);
    const float mid_input_rms = Rms(mid, kRate / 10);
    quiet.Process(mid);
    const float mid_output_rms = Rms(mid, kRate / 10);
    assert(std::fabs(mid_output_rms - mid_input_rms) < mid_input_rms * 0.03f);

    // A full-scale tone is held around -1.5 dBFS instead of clipping.
    SpeakerProtection loud;
    loud.Configure(kRate);
    auto full = Tone(1000.0f, 1.0f, kRate / 2);
    loud.Process(full);
    assert(Peak(full, kRate / 10) <= static_cast<int>(0.86f * 32767.0f));

    // 1 ms look-ahead: the delay line emits nothing until the window fills.
    SpeakerProtection lookahead;
    lookahead.Configure(kRate);
    std::vector<int16_t> impulse(200, 0);
    impulse[0] = 32767;
    lookahead.Process(impulse);
    for (size_t i = 0; i < kRate / 1000; ++i) {
        assert(impulse[i] == 0);
    }
    assert(Peak(impulse, kRate / 1000) > 0);

    // Saturation guard: DC is blocked without any int16 wraparound.
    SpeakerProtection extreme;
    extreme.Configure(kRate);
    std::vector<int16_t> dc(1000, 32767);
    extreme.Process(dc);
    for (int16_t sample : dc) {
        assert(sample > -32768 && sample < 32767);
    }
    assert(std::abs(static_cast<int>(dc.back())) < 1000);
}
''')

    def test_loudness_guard_trims_dense_music_and_recovers(self):
        motion_tests.WzhFluentMotionTest.run_cpp(self,
            '#include "'+str(HEADER)+r'"' + r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace {
constexpr int kRate = 24000;
constexpr float kPi = 3.14159265358979323846f;

std::vector<int16_t> Tone(float hz, float amplitude, int count) {
    std::vector<int16_t> pcm(count);
    for (int i = 0; i < count; ++i) {
        pcm[i] = static_cast<int16_t>(
            std::lrintf(amplitude * std::sin(2.0f * kPi * hz * i / kRate) * 32767.0f));
    }
    return pcm;
}

double Rms(const std::vector<int16_t>& pcm, size_t from) {
    double sum = 0;
    for (size_t i = from; i < pcm.size(); ++i) {
        const double v = pcm[i];
        sum += v * v;
    }
    return std::sqrt(sum / (pcm.size() - from));
}
}  // namespace

int main() {
    // Normal speech level (RMS ~ -19.5 dBFS) passes untouched.
    SpeakerProtection speech;
    speech.Configure(kRate);
    auto voice = Tone(1000.0f, 0.15f, kRate);
    const double voice_in = Rms(voice, 0);
    speech.Process(voice);
    assert(std::fabs(Rms(voice, kRate / 2) - voice_in) < voice_in * 0.02);

    // Dense music (RMS ~ -7.5 dBFS) is held at the -4 dB floor.
    SpeakerProtection music;
    music.Configure(kRate);
    auto loud = Tone(1000.0f, 0.6f, kRate);
    const double loud_in = Rms(loud, 0);
    music.Process(loud);
    const double ratio = Rms(loud, kRate / 2) / loud_in;
    assert(ratio < 0.66 && ratio > 0.60);

    // The guard releases within its 500 ms time constant after music stops.
    SpeakerProtection recovery;
    recovery.Configure(kRate);
    auto mix = Tone(1000.0f, 0.6f, kRate);
    auto quiet = Tone(1000.0f, 0.1f, kRate * 2);
    const double quiet_in = Rms(quiet, kRate / 2);
    mix.insert(mix.end(), quiet.begin(), quiet.end());
    recovery.Process(mix);
    const double quiet_out = Rms(mix, kRate * 5 / 2);
    assert(quiet_out > quiet_in * 0.9);
}
''')

    def test_codec_applies_protection_only_when_enabled(self):
        source=(ROOT/'main/audio/audio_codec.cc').read_text()
        method=queue_function(source,'void AudioCodec::OutputData(')
        motion_tests.WzhFluentMotionTest.run_cpp(self,r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <vector>
struct SpeakerProtection {
    int process_calls = 0;
    void Reset() {}
    void Process(std::vector<int16_t>&) { ++process_calls; }
};
struct AudioCodec {
    bool speaker_protection_enabled_ = false;
    SpeakerProtection speaker_protection_;
    std::atomic<bool> output_processing_reset_pending_{true};
    int written = 0;
    void OutputData(std::vector<int16_t>&);
    void Write(const int16_t*, int) { ++written; }
};
'''+method+r'''
int main() {
    std::vector<int16_t> pcm(4, 0);
    AudioCodec codec;
    codec.OutputData(pcm);
    assert(codec.written == 1 && codec.speaker_protection_.process_calls == 0);
    codec.speaker_protection_enabled_ = true;
    codec.OutputData(pcm);
    assert(codec.written == 2 && codec.speaker_protection_.process_calls == 1);
}
''')

    def test_robot_boards_enable_protection_at_audio_init(self):
        source=(ROOT/'main/audio/audio_service.cc').read_text()
        init=queue_function(source,'void AudioService::Initialize(')
        self.assertIn('EnableSpeakerProtection(true)', init)
        self.assertIn('buffered_playback_', init)

    def test_transients_fragmented_processing_and_reset_at_supported_rates(self):
        self.run_program(r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>
int main(){
 for(int rate:{8000,16000,24000,48000}){
  std::vector<int16_t> original(rate/2);
  uint32_t random=23;
  for(size_t i=0;i<original.size();++i){
   random=random*1664525U+1013904223U;
   original[i]=static_cast<int16_t>(random>>16);
   if(i%23==0){original[i]=32767;}
   if(i%41==0){original[i]=-32768;}
  }
  SpeakerProtection whole,fragmented;
  whole.Configure(rate);fragmented.Configure(rate);
  auto expected=original;whole.Process(expected);
  std::vector<int16_t>actual;
  for(size_t offset=0;offset<original.size();){
   size_t count=std::min<size_t>(1+offset%137,original.size()-offset);
   std::vector<int16_t>chunk(original.begin()+offset,original.begin()+offset+count);
   fragmented.Process(chunk);actual.insert(actual.end(),chunk.begin(),chunk.end());offset+=count;
  }
  assert(actual==expected);
  for(int sample:actual){assert(sample<=27525&&sample>=-27525);}
  fragmented.Reset();std::vector<int16_t> silence(1000,0);fragmented.Process(silence);
  for(int sample:silence){assert(sample==0);}
 }
}
''')

    def run_program(self, program):
        motion_tests.WzhFluentMotionTest.run_cpp(
            self, '#include "' + str(HEADER) + '"\n' + program)

    def test_codec_reset_request_is_applied_by_output_writer(self):
        source = (ROOT/'main/audio/audio_codec.cc').read_text()
        self.run_program(r'''
#include <atomic>
#include <cassert>
struct AudioCodec{
 bool speaker_protection_enabled_=true;
 SpeakerProtection speaker_protection_;
 std::atomic<bool>output_processing_reset_pending_{true};
 void OutputData(std::vector<int16_t>&);
 void ResetOutputProcessing();
 void Write(const int16_t*,int){}
};
''' + queue_function(source, 'void AudioCodec::OutputData(')
            + queue_function(source, 'void AudioCodec::ResetOutputProcessing(') + r'''
int main(){
 AudioCodec codec;codec.speaker_protection_.Configure(24000);
 std::vector<int16_t>old(1,32767);codec.OutputData(old);
 // A cancellation request must not let the look-ahead sample enter new playback.
 codec.ResetOutputProcessing();assert(codec.output_processing_reset_pending_.load());
 std::vector<int16_t>new_session(100,0);codec.OutputData(new_session);
 assert(!codec.output_processing_reset_pending_.load());
 for(auto sample:new_session){assert(sample==0);}
}
''')
