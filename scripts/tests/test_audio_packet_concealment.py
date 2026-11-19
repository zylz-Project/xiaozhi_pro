"""Legacy concealment helper and upstream-compatible live Opus playback."""
from pathlib import Path
import unittest

import test_firmware_runtime_optimizations as runtime

ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT/'main/audio'
CONCEAL = AUDIO/'audio_packet_conceal.h'


class AudioPacketConcealmentTest(unittest.TestCase):
    def test_frames_repeat_last_pcm_and_fade_to_silence(self):
        self.run_cpp(
            '#include "'+str(CONCEAL)+'"\n'+r'''
#include <cassert>
#include <vector>
int main(){
    AudioPacketConcealment conceal;
    std::vector<int16_t> out;
    // Nothing decoded yet: the caller must skip the gap entirely.
    assert(!conceal.BuildFrame(out, 960) && out.empty() && conceal.streak() == 0);

    const std::vector<int16_t> frame(960, 1000);
    conceal.Observe(frame.data(), frame.size());
    assert(conceal.streak() == 0);

    // A real frame resets the fade, so the first concealed frame is a full copy.
    assert(conceal.BuildFrame(out, 960));
    assert(out.size() == 960 && out[0] == 1000 && out[959] == 1000);
    assert(conceal.streak() == 1);

    // Every following frame is quieter; the sequence reaches silence instead of
    // looping a tone for the whole outage.
    int previous = 1000;
    for (unsigned i = 1; i < AudioPacketConcealment::kFadeFrames; ++i) {
        assert(conceal.BuildFrame(out, 960));
        assert(out[0] <= previous);
        previous = out[0];
    }
    assert(conceal.BuildFrame(out, 960));
    assert(out[0] == 0);

    // A fresh real frame restores full volume.
    conceal.Observe(frame.data(), frame.size());
    assert(conceal.BuildFrame(out, 960) && out[0] == 1000);
}
''')

    def test_a_shorter_frame_never_splices_stale_samples(self):
        self.run_cpp(
            '#include "'+str(CONCEAL)+'"\n'+r'''
#include <cassert>
#include <vector>
int main(){
    AudioPacketConcealment conceal;
    std::vector<int16_t> long_frame(960, 700), short_frame(480, -200), out;
    conceal.Observe(long_frame.data(), long_frame.size());
    conceal.Observe(short_frame.data(), short_frame.size());
    assert(conceal.BuildFrame(out, 480));
    for (int16_t sample : out) assert(sample == -200);
    // A requested frame larger than the stored one repeats within the new frame.
    assert(conceal.BuildFrame(out, 960));
    assert(out.size() == 960 && out[0] < 0 && out[959] < 0);

    // Explicit gain overrides the automatic fade (used for a known short gap).
    conceal.Observe(long_frame.data(), long_frame.size());
    assert(conceal.BuildFrame(out, 960, 128));
    assert(out[0] == 350);
}
''')

    def test_live_decode_never_replays_pcm_or_drops_real_audio_for_gap_fillers(self):
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
#include "audio_packet_conceal.h"
#include "fixed_queue.h"
constexpr int ESP_AUDIO_ERR_OK=0,ESP_AE_ERR_OK=0,ESP_AUDIO_DEC_RECOVERY_NONE=0;
using esp_ae_sample_t=int16_t*;
struct esp_audio_dec_in_raw_t{uint8_t*buffer;uint32_t len,consumed;int frame_recover;};
struct esp_audio_dec_out_frame_t{uint8_t*buffer;uint32_t len,decoded_size;};
struct esp_audio_dec_info_t{};
int decoder_resets=0,resampler_resets=0;
void esp_opus_dec_reset(void*){++decoder_resets;}void esp_ae_rate_cvt_reset(void*){++resampler_resets;}
int esp_opus_dec_decode(void*,esp_audio_dec_in_raw_t*,esp_audio_dec_out_frame_t*out,esp_audio_dec_info_t*){
 assert(out->len>=1920);auto*p=reinterpret_cast<int16_t*>(out->buffer);
 for(int i=0;i<960;++i)p[i]=1000;
 out->decoded_size=1920;return 0;
}
int esp_ae_rate_cvt_get_max_out_sample_num(void*,size_t count,uint32_t*out){*out=count/2;return 0;}
int esp_ae_rate_cvt_process(void*,int16_t*in,size_t count,int16_t*out,uint32_t*n){
 assert(*n>=count/2);for(size_t i=0;i<count/2;++i)out[i]=in[i*2];*n=count/2;return 0;
}
struct AudioStreamPacket{int sample_rate=48000,frame_duration=20;uint32_t timestamp=0;
 std::vector<uint8_t>payload{0xd8,0};OpusStreamMetadata stream;uint8_t missing_before=0;};
constexpr int kAudioTaskTypeDecodeToPlaybackQueue=1;
struct AudioTask{int type=0;uint32_t timestamp=0,playback_generation=0;std::vector<int16_t>pcm;
 OpusStreamMetadata stream;};
struct Codec{int output_sample_rate(){return 24000;}};
struct AudioService{
 Codec codec;Codec*codec_=&codec;std::mutex audio_queue_mutex_,decoder_mutex_;
 std::condition_variable audio_queue_cv_;
 std::deque<std::unique_ptr<AudioStreamPacket>>audio_decode_queue_;
 FixedQueue<AudioTask,6>audio_playback_queue_;
 size_t max_playback_tasks_=6;bool decode_in_flight_=false;uint32_t playback_generation_=7;
 int decoder_sample_rate_=48000,decoder_frame_size_=960;void*opus_decoder_=this,*output_resampler_=this;
 std::atomic<bool>service_stopped_{false};
 struct{int decode_count=0,conceal_count=0;}debug_statistics_;
 AudioPacketConcealment concealment_;
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
 auto feed=[&](uint32_t timestamp,uint8_t missing){
  auto packet=std::make_unique<AudioStreamPacket>();
  packet->timestamp=timestamp;packet->missing_before=missing;
  service.audio_decode_queue_.push_back(std::move(packet));service.DecodeOne();
 };
 // Sequence gaps must never synthesize repeated PCM, even at startup.
 feed(1,2);
 assert(service.audio_playback_queue_.size()==1);
 feed(2,2);
 assert(service.audio_playback_queue_.size()==2);
 assert(service.audio_playback_queue_[1].pcm.size()==480);
 assert(service.audio_playback_queue_[1].timestamp==2);
 // The real packet must fit when only one PCM slot remains. The previous
 // filler implementation occupied that slot and silently dropped real audio.
 feed(3,0);feed(4,0);feed(5,0);feed(6,255);
 assert(service.audio_playback_queue_.size()==6);
 assert(service.audio_playback_queue_[5].timestamp==6);
 assert(service.debug_statistics_.conceal_count==0);

}
'''
        self.run_cpp(program)

    def run_cpp(self, program):
        runtime.FirmwareRuntimeOptimizationsTest.run_cpp(self, program)


if __name__ == '__main__':
    unittest.main()
