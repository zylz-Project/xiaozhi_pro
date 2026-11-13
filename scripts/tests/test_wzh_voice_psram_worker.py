"""Test reuse and handoff of the production persistent PSRAM voice worker."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class VoicePsramWorkerTest(unittest.TestCase):
    def test_worker_reused_for_pending_and_future_turns(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        source = (ROOT / 'main/boards/common/wzh/voice_tts/voice_session.cc').read_text()
        methods = '\n'.join(queue_function(source, 'bool VoiceSession::' + name + '(')
                            for name in ['Ready', 'ReadyForVoice', 'Begin', 'BeginWithVoice', 'Enqueue'])
        methods += '\n' + '\n'.join(queue_function(source, 'void VoiceSession::' + name + '(')
                                     for name in ['Finish', 'Cancel', 'CompleteWorker'])
        code = r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include "voice_text_filter.h"
#define CONFIG_WZH_VOICE_PSRAM_WORKER_EXPERIMENT 1
#define CONFIG_WZH_VOICE_FLASH_DISPATCH_EXPERIMENT 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
size_t internal_free=30000, internal_largest=6144;
size_t heap_caps_get_free_size(int caps){return caps & MALLOC_CAP_INTERNAL ? internal_free : 8*1024*1024;}
size_t heap_caps_get_largest_free_block(int caps){return caps & MALLOC_CAP_INTERNAL ? internal_largest : 4*1024*1024;}
using TaskHandle_t=void*;constexpr int pdPASS=1;
static int creates=0,internal_creates=0,notifications=0;static bool fail_create=false,dispatcher_ready=true;
int xTaskCreateWithCaps(void(*)(void*),const char*,int,void*,int,TaskHandle_t* task,int caps){
 assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(fail_create)return 0;
 *task=reinterpret_cast<void*>(static_cast<uintptr_t>(++creates));return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task){assert(task);++notifications;}
int xTaskCreate(void(*)(void*),const char*,int,void*,int,TaskHandle_t* task){
 ++internal_creates;if(fail_create)return 0;*task=reinterpret_cast<void*>(99);return pdPASS;
}
namespace voice_tts {
bool FlashDispatcherReady(){return dispatcher_ready;}
constexpr size_t kMaxPendingSentences=64,kMaxPendingBytes=16*1024;
std::string LoadAliKey(){return "key";}std::string LoadAliVoice(){return "voice";}
struct VoiceSession {
 struct PendingTurn {std::string provider,api_key,voice_id;std::function<void(bool)> completion;
  uint32_t playback_generation=0,generation=0;};
 std::mutex mutex_;std::deque<std::string> queue_;std::function<void(bool)> completion_;
 std::atomic<uint32_t> generation_{0},active_generation_{0};std::atomic<bool> bypass_next_{false};
 TaskHandle_t task_=nullptr,worker_task_=nullptr;std::unique_ptr<PendingTurn> pending_;bool stopping_=false;
 uint32_t playback_generation_=0;bool finished_=false,failed_=false,had_text_=false,overflow_=false;
 size_t queued_bytes_=0;std::string provider_,api_key_,voice_id_;
 static void TaskEntry(void*){}
 bool Ready();bool ReadyForVoice(const std::string&);bool Begin(uint32_t,std::function<void(bool)>);
 bool BeginWithVoice(uint32_t,const std::string&,std::function<void(bool)>);bool Enqueue(const std::string&);
 void Finish();void Cancel();void CompleteWorker(uint32_t);
};
''' + methods + r'''
}
int main(){
 using voice_tts::VoiceSession;
 VoiceSession s;assert(s.Ready());internal_largest=6143;assert(!s.Ready());internal_largest=6144;
 int calls=0;assert(s.Begin(1,[&](bool){++calls;}));assert(creates==1&&notifications==1);
 auto old=s.active_generation_.load();assert(s.Enqueue("old"));s.Cancel();
 assert(s.Begin(2,[&](bool ok){assert(ok);++calls;}));assert(s.Enqueue("new"));s.Finish();
 s.CompleteWorker(old);assert(s.task_==s.worker_task_&&creates==1);
 s.CompleteWorker(s.active_generation_.load());assert(calls==1&&!s.task_&&s.worker_task_);
 assert(s.Begin(3,[&](bool ok){assert(ok);++calls;}));assert(creates==1);
 assert(s.Enqueue("again"));s.Finish();s.CompleteWorker(s.active_generation_.load());assert(calls==2);
 VoiceSession failure;fail_create=true;assert(!failure.Begin(4,[](bool){}));
 assert(!failure.worker_task_&&!failure.task_);
 fail_create=false;dispatcher_ready=false;VoiceSession fallback;
 assert(!fallback.Ready());internal_largest=8192;assert(fallback.Ready());
 assert(fallback.Begin(5,[](bool){}));assert(internal_creates==1&&fallback.worker_task_);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / 'test.cc').write_text(code)
            subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/boards/common/wzh/voice_tts'),
                            str(p / 'test.cc'), '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True, timeout=10)
