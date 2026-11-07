"""Exercise the production Ali PCM ring with concurrent producer/playback."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AliPcmBufferTest(unittest.TestCase):
    def test_order_backpressure_and_cancellation(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        source = (ROOT / 'main/boards/common/wzh/voice_tts/voice_session.cc').read_text()
        buffer = source[source.index('class AliPcmBuffer {'):source.index('}  // namespace')]
        code = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
using TaskHandle_t = void*;
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
void* heap_caps_malloc(size_t n,int){return malloc(n);}
void vTaskDelay(int ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
void vTaskDelete(void*){}
int xTaskCreate(void (*fn)(void*),const char*,int,void* p,int,TaskHandle_t* task){
 *task=p;std::thread([=]{fn(p);}).detach();return pdPASS;
}
struct AudioService {
 std::vector<int16_t> samples;
 std::atomic<size_t> delivered{0};
 bool PushPcmToPlaybackQueue(std::vector<int16_t> data,int rate,uint32_t,bool){
  if(rate!=24000)return false;
  samples.insert(samples.end(),data.begin(),data.end());
  delivered.fetch_add(data.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(1));return true;
 }
};
''' + buffer + r'''
int main(){
 AudioService audio;std::atomic<bool> cancel{false};
 AliPcmBuffer ring(audio,1,[&]{return cancel.load();});
 if(!ring.Start())return 1;
 std::vector<uint8_t> input(240002);
 for(size_t i=0;i<input.size();++i)input[i]=static_cast<uint8_t>(i*17);
 for(size_t i=0;i<input.size();){
  size_t n=std::min<size_t>(997,input.size()-i);
  if(!ring.Write(input.data()+i,n))return 2;i+=n;
 }
 if(!ring.Finish()||audio.samples.size()!=input.size()/2)return 3;
 for(size_t i=0;i<audio.samples.size();++i){
  uint16_t expected=uint16_t(input[2*i])|(uint16_t(input[2*i+1])<<8);
  if(static_cast<uint16_t>(audio.samples[i])!=expected)return 4;
 }
 // A new short burst after an empty ring should re-prime promptly, not wait
 // for the original 500 ms prefix or the 1.2 second startup deadline.
 AudioService gaps; AliPcmBuffer gap_ring(gaps,3,[&]{return cancel.load();});
 if(!gap_ring.Start())return 8;
 std::vector<uint8_t> burst(24000,0);
 if(!gap_ring.Write(burst.data(),burst.size()))return 9;
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(gaps.delivered.load()<12000 && std::chrono::steady_clock::now()<deadline)
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
 if(gaps.delivered.load()!=12000)return 10;
 std::this_thread::sleep_for(std::chrono::milliseconds(75));
 if(!gap_ring.Write(burst.data(),5760))return 11;
 deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(600);
 while(gaps.delivered.load()<14880 && std::chrono::steady_clock::now()<deadline)
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
 if(gaps.delivered.load()!=14880)return 12;
 if(!gap_ring.Finish())return 13;
 AudioService other; AliPcmBuffer stopped(other,2,[&]{return cancel.load();});
 if(!stopped.Start())return 5;
 cancel=true;
 if(stopped.Write(input.data(),input.size()))return 6;
 if(stopped.Finish())return 7;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cc').write_text(code)
            result = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                     str(path / 'test.cc'), '-o', str(path / 'test')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / 'test')], check=True, timeout=20)
