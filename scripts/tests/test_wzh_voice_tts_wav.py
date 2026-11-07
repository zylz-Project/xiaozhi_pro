"""Exercise the production WAV parser without an ESP32."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class VoiceTtsWavTest(unittest.TestCase):
    def test_api_key_storage_validation_and_erase(self):
        if not shutil.which("g++"):
            self.skipTest("host C++ compiler required")
        stub = r'''
#pragma once
#include <cstddef>
using nvs_handle_t=int; using esp_err_t=int;
constexpr int ESP_OK=0, ESP_ERR_NVS_NOT_FOUND=1, NVS_READWRITE=1, NVS_READONLY=0;
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_set_str(nvs_handle_t,const char*,const char*);
esp_err_t nvs_get_str(nvs_handle_t,const char*,char*,size_t*);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_erase_key(nvs_handle_t,const char*);
esp_err_t nvs_commit(nvs_handle_t);
'''
        source = r'''
#include <cassert>
#include <cstring>
#include <string>
#include "nvs.h"
#include "voice_config.h"
static std::string stored; static bool commit_failure=false;
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*){return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t){return ESP_OK;}
esp_err_t nvs_open(const char*,int,nvs_handle_t* h){*h=1;return ESP_OK;}
void nvs_close(nvs_handle_t){}
esp_err_t nvs_set_str(nvs_handle_t,const char*,const char* v){stored=v;return ESP_OK;}
esp_err_t nvs_erase_key(nvs_handle_t,const char*){if(stored.empty())return ESP_ERR_NVS_NOT_FOUND;stored.clear();return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t){return commit_failure?2:ESP_OK;}
esp_err_t nvs_get_str(nvs_handle_t,const char*,char* out,size_t* size){
 if(stored.empty())return ESP_ERR_NVS_NOT_FOUND;
 if(out) { std::memcpy(out,stored.c_str(),stored.size()+1); }
 *size=stored.size()+1;return ESP_OK;
}
int main(){
 assert(!!voice_tts::LoadAliKey().empty());assert(voice_tts::SaveAliKey("test-only-key"));
 assert(!voice_tts::LoadAliKey().empty() && voice_tts::LoadAliKey()=="test-only-key");
 assert(!voice_tts::SaveAliKey("bad\r\nheader"));assert(!voice_tts::SaveAliKey(std::string(257,'x')));
 assert(voice_tts::SaveAliKey(""));assert(!!voice_tts::LoadAliKey().empty());
 assert(voice_tts::SaveAliKey(""));commit_failure=true;assert(!voice_tts::SaveAliKey("key"));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "nvs.h").write_text(stub)
            (path / "test.cc").write_text(source)
            provider = ROOT / "main/boards/common/wzh/voice_tts"
            subprocess.run([
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I", str(path), "-I", str(provider), str(provider / "voice_config.cc"),
                str(path / "test.cc"), "-o", str(path / "test"),
            ], check=True, capture_output=True)
            subprocess.run([str(path / "test")], check=True, timeout=10)

    def test_playback_passes_ordered_pcm_and_generation_to_single_output_owner(self):
        if not shutil.which("g++"):
            self.skipTest("host C++ compiler required")
        stub = r'''
#include <cstdint>
#include <utility>
#include <vector>
class AudioService {
public:
 std::vector<int16_t> samples; uint32_t generation=0; bool accept=true;
 bool PushPcmToPlaybackQueue(std::vector<int16_t> frame,int rate,uint32_t gen,bool wait) {
  if (!accept || rate!=24000 || !wait) return false;
  generation=gen; samples.insert(samples.end(),frame.begin(),frame.end()); return true;
 }
};
'''
        source = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include "audio_service.h"
#include "wav_playback.h"
static void u32(std::vector<uint8_t>& v,unsigned n){for(int i=0;i<4;i++)v.push_back((n>>(8*i))&255);}
static void u16(std::vector<uint8_t>& v,unsigned n){v.push_back(n&255);v.push_back((n>>8)&255);}
int main(){
 std::vector<uint8_t> v={'R','I','F','F'};u32(v,40);
 v.insert(v.end(),{'W','A','V','E','f','m','t',' '});u32(v,16);
 u16(v,1);u16(v,1);u32(v,24000);u32(v,48000);u16(v,2);u16(v,16);
 v.insert(v.end(),{'d','a','t','a'});u32(v,4);v.insert(v.end(),{0xff,0xff,0x34,0x12});
 AudioService audio;
 assert(voice_tts::PlayWav(audio,v,24000,9,{}));
 assert(audio.generation==9 && audio.samples.size()==2 && audio.samples[0]==-1 && audio.samples[1]==0x1234);
 assert(!voice_tts::PlayWav(audio,v,24000,9,[]{return true;}));
 assert(!voice_tts::PlayWav(audio,v,16000,9,{}));
 audio.accept=false;assert(!voice_tts::PlayWav(audio,v,24000,9,{}));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "audio_service.h").write_text(stub)
            (path / "test.cc").write_text(source)
            provider = ROOT / "main/boards/common/wzh/voice_tts"
            subprocess.run([
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I", str(path), "-I", str(provider), str(provider / "wav_playback.cc"),
                str(path / "test.cc"), "-o", str(path / "test"),
            ], check=True, capture_output=True)
            subprocess.run([str(path / "test")], check=True, timeout=10)

    def test_pcm_queue_is_bounded_and_reset_cancels_waiting_producer(self):
        if not shutil.which("g++"):
            self.skipTest("host C++ compiler required")
        method = queue_function(
            (ROOT / "main/audio/audio_service.cc").read_text(),
            "bool AudioService::PushPcmToPlaybackQueue(",
        )
        source = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
enum {kAudioTaskTypeDecodeToPlaybackQueue};
struct AudioTask { int type; uint32_t playback_generation; std::vector<int16_t> pcm; };
struct Codec { int output_sample_rate() { return 24000; } };
struct Queue : std::deque<AudioTask> {
  bool push_back(AudioTask value) { std::deque<AudioTask>::push_back(std::move(value)); return true; }
};
struct AudioService {
  Codec codec; Codec* codec_=&codec; std::mutex audio_queue_mutex_;
  std::condition_variable audio_queue_cv_; std::atomic<bool> service_stopped_{false};
  uint32_t playback_generation_=7; bool decode_in_flight_=false, playback_drained_notified_=true;
  size_t max_playback_tasks_=2; Queue audio_playback_queue_;
  bool PushPcmToPlaybackQueue(std::vector<int16_t>,int,uint32_t,bool);
};
''' + method + r'''
int main() {
  AudioService audio;
  assert(!audio.PushPcmToPlaybackQueue({},24000,7,false));
  assert(!audio.PushPcmToPlaybackQueue(std::vector<int16_t>(1441),24000,7,false));
  assert(!audio.PushPcmToPlaybackQueue({1},16000,7,false));
  audio.decode_in_flight_=true;
  assert(audio.PushPcmToPlaybackQueue({0},24000,7,false));
  assert(!audio.PushPcmToPlaybackQueue({0},24000,7,false));
  audio.audio_playback_queue_.clear(); audio.decode_in_flight_=false;
  assert(audio.PushPcmToPlaybackQueue({1},24000,7,false));
  assert(audio.PushPcmToPlaybackQueue({2},24000,7,false));
  assert(!audio.PushPcmToPlaybackQueue({3},24000,7,false));
  bool result=true;
  std::thread worker([&] { result=audio.PushPcmToPlaybackQueue({4},24000,7,true); });
  { std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_); ++audio.playback_generation_; }
  audio.audio_queue_cv_.notify_all(); worker.join();
  assert(!result && audio.audio_playback_queue_.size()==2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.cc").write_text(source)
            subprocess.run([
                "g++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                str(path / "test.cc"), "-o", str(path / "test"),
            ], check=True, capture_output=True)
            subprocess.run([str(path / "test")], check=True, timeout=10)

    def test_complete_pcm_and_invalid_or_truncated_responses(self):
        if not shutil.which("g++"):
            self.skipTest("host C++ compiler required")
        source = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include "wav_pcm.h"
using namespace voice_tts;
static void u32(std::vector<uint8_t>& v, unsigned n) {
  for (int i=0;i<4;++i) v.push_back((n>>(8*i))&255);
}
static void u16(std::vector<uint8_t>& v, unsigned n) {
  v.push_back(n&255); v.push_back((n>>8)&255);
}
int main() {
  std::vector<uint8_t> v={'R','I','F','F'}; u32(v,40);
  v.insert(v.end(),{'W','A','V','E','f','m','t',' '}); u32(v,16);
  u16(v,1); u16(v,1); u32(v,24000); u32(v,48000); u16(v,2); u16(v,16);
  v.insert(v.end(),{'d','a','t','a'}); u32(v,4); v.insert(v.end(),{0,0,1,0});
  WavPcmView out;
  assert(ParseWavPcm(v.data(),v.size(),24000,out));
  assert(out.size==4 && out.sample_rate==24000 && out.data==v.data()+44);
  assert(!ParseWavPcm(v.data(),v.size(),16000,out) && !out.data);
  assert(!ParseWavPcm(v.data(),v.size()-1,24000,out));
  auto bad=v; bad[22]=2; assert(!ParseWavPcm(bad.data(),bad.size(),24000,out));
  bad=v; bad[20]=3; assert(!ParseWavPcm(bad.data(),bad.size(),24000,out));
  bad=v; bad[40]=5; assert(!ParseWavPcm(bad.data(),bad.size(),24000,out));
  bad=v; bad[0]='X'; assert(!ParseWavPcm(bad.data(),bad.size(),24000,out));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.cc").write_text(source)
            subprocess.run([
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "main/boards/common/wzh/voice_tts"),
                str(path / "test.cc"), "-o", str(path / "test"),
            ], check=True, capture_output=True)
            subprocess.run([str(path / "test")], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
