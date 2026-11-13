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
struct FakeTask { std::thread thread; std::atomic<bool> suspended{false}; std::atomic<bool> release{false}; };
using TaskHandle_t = FakeTask*;
using BaseType_t = int;
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 1
#define CONFIG_SPIRAM 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
bool fail_allocation=false, fail_task=false;
void* heap_caps_malloc(size_t n,int){return fail_allocation ? nullptr : malloc(n);}
void vTaskDelay(int ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
thread_local FakeTask* current_task = nullptr;
enum TaskState { eRunning, eSuspended };
int xTaskCreateWithCaps(void (*fn)(void*),const char*,int,void* p,int,TaskHandle_t* task,int){
 if(fail_task)return 0;
 auto* t=new FakeTask; *task=t;
 t->thread=std::thread([=]{current_task=t;fn(p);});return pdPASS;
}
void vTaskSuspend(void*) { current_task->suspended=true; while(!current_task->release) std::this_thread::yield(); }
TaskState eTaskGetState(TaskHandle_t task) {return task->suspended ? eSuspended : eRunning;}
void vTaskDeleteWithCaps(TaskHandle_t task) {task->release=true;task->thread.join();delete task;}
struct AudioService {
 std::vector<int16_t> samples;
 std::vector<size_t> frame_sizes;
 std::atomic<size_t> delivered{0};
 bool PushPcmToPlaybackQueue(std::vector<int16_t> data,int rate,uint32_t,bool){
  if(rate!=24000)return false;
  frame_sizes.push_back(data.size());
  samples.insert(samples.end(),data.begin(),data.end());
  delivered.fetch_add(data.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(1));return true;
 }
};
''' + buffer + r'''
int main(){
 AudioService audio;std::atomic<bool> cancel{false};
 fail_allocation=true; AliPcmBuffer no_memory(audio,1,[&]{return cancel.load();});
 if(no_memory.Start())return 10; fail_allocation=false;
 fail_task=true; AliPcmBuffer no_task(audio,1,[&]{return cancel.load();});
 if(no_task.Start())return 11; fail_task=false;
 AliPcmBuffer ring(audio,1,[&]{return cancel.load();});
 if(!ring.Start())return 1;
 std::vector<uint8_t> input(240002);
 for(size_t i=0;i<input.size();++i)input[i]=static_cast<uint8_t>(i*17);
 for(size_t i=0;i<input.size();){
  size_t n=std::min<size_t>(997,input.size()-i);
  if(!ring.Write(input.data()+i,n))return 2;i+=n;
 }
 if(!ring.Finish()||audio.samples.size()!=input.size()/2)return 3;
 for(size_t i=0;i+1<audio.frame_sizes.size();++i)
  if(audio.frame_sizes[i]!=1440)return 15;
 for(size_t i=0;i<audio.samples.size();++i){
  uint16_t expected=uint16_t(input[2*i])|(uint16_t(input[2*i+1])<<8);
  if(static_cast<uint16_t>(audio.samples[i])!=expected)return 4;
 }
 // A short completed sentence should play promptly without waiting for the
 // larger startup prefix; an incomplete burst must remain buffered.
 AudioService gaps; AliPcmBuffer gap_ring(gaps,3,[&]{return cancel.load();});
 if(!gap_ring.Start())return 8;
 // The handshake may take longer than the old startup deadline; the
 // prebuffer deadline must start when PCM arrives, not at task creation.
 std::this_thread::sleep_for(std::chrono::milliseconds(1400));
 std::vector<uint8_t> burst(24000,0);
 if(!gap_ring.Write(burst.data(),burst.size()))return 9;
 std::this_thread::sleep_for(std::chrono::milliseconds(100));
 if(gaps.delivered.load()!=0)return 14;
 gap_ring.CompleteSegment();
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(gaps.delivered.load()<12000 && std::chrono::steady_clock::now()<deadline)
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
 if(gaps.delivered.load()!=12000)return 10;
 std::this_thread::sleep_for(std::chrono::milliseconds(75));
 if(!gap_ring.Write(burst.data(),5760))return 11;
 gap_ring.CompleteSegment();
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
