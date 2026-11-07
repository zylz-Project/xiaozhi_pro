"""Speaker protection trims sub-bass and caps near-full-scale peaks pre-DAC."""
from pathlib import Path
import unittest

import test_wzh_fluent_motion as motion_tests
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT/'main/audio/speaker_protection.h'


class SpeakerProtectionTest(unittest.TestCase):
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
