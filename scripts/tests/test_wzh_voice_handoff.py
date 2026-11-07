"""Exercise actual voice session cancellation and deferred turn handoff."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class VoiceHandoffTest(unittest.TestCase):
    def test_cancelled_worker_hands_over_without_fallback_or_lost_text(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        text = (ROOT / 'main/boards/wzh_crawlPanda/voice_tts/voice_session.cc').read_text()
        methods = '\n'.join(queue_function(text, 'bool VoiceSession::'+name+'(')
                            for name in ['Ready', 'Begin', 'Enqueue'])
        methods += '\n' + '\n'.join(queue_function(text, 'void VoiceSession::'+name+'(')
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
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
size_t heap_caps_get_free_size(int){return 8*1024*1024;}
size_t heap_caps_get_largest_free_block(int){return 4*1024*1024;}
using TaskHandle_t=void*;
constexpr int pdPASS=1;static int creates=0;static bool fail_create=false;
int xTaskCreate(void(*)(void*),const char*,int,void*,int,TaskHandle_t* task){
 if(fail_create)return 0;*task=reinterpret_cast<void*>(static_cast<uintptr_t>(++creates));return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t){}
namespace voice_tts {
constexpr size_t kMaxPendingSentences=64,kMaxPendingBytes=16*1024;
std::string LoadAliKey(){return "key";}
std::string LoadAliVoice(){return "voice";}
struct VoiceSession {
 struct PendingTurn {std::string provider,api_key,voice_id;std::function<void(bool)> completion;
  uint32_t playback_generation=0,generation=0;};
 std::mutex mutex_;std::deque<std::string> queue_;std::function<void(bool)> completion_;
 std::atomic<uint32_t> generation_{0},active_generation_{0};std::atomic<bool> bypass_next_{false};
 TaskHandle_t task_=nullptr;std::unique_ptr<PendingTurn> pending_;bool stopping_=false;
 uint32_t playback_generation_=0;bool finished_=false,failed_=false,had_text_=false,overflow_=false;
 size_t queued_bytes_=0;std::string provider_,api_key_,voice_id_;
 static void TaskEntry(void*){}
 bool Ready();bool Begin(uint32_t,std::function<void(bool)>);bool Enqueue(const std::string&);
 void Finish();void Cancel();void CompleteWorker(uint32_t);
};
''' + methods + r'''
}
int main(){
 using voice_tts::VoiceSession;
 int old_calls=0,new_calls=0;bool new_ok=false;
 VoiceSession s;assert(s.Ready());assert(s.Begin(4,[&](bool){++old_calls;}));
 auto old=s.active_generation_.load();assert(s.Enqueue("old"));
 s.Cancel();assert(s.Ready());assert(s.Begin(9,[&](bool ok){++new_calls;new_ok=ok;}));
 assert(s.pending_&&s.Enqueue("new")&&s.queue_.front()=="new");s.Finish();
 assert(!s.Ready());s.CompleteWorker(old);
 assert(old_calls==0&&s.task_&&s.active_generation_.load()!=old);
 assert(s.playback_generation_==9&&s.queue_.front()=="new"&&s.finished_);
 s.CompleteWorker(s.active_generation_.load());assert(new_calls==1&&new_ok&&!s.task_);
 VoiceSession cancelled;assert(cancelled.Begin(1,[](bool){}));auto first=cancelled.active_generation_.load();
 cancelled.Cancel();assert(cancelled.Begin(2,[](bool){}));assert(cancelled.Enqueue("discard"));
 cancelled.Cancel();cancelled.CompleteWorker(first);assert(!cancelled.task_&&!cancelled.pending_&&cancelled.queue_.empty());
 VoiceSession failed;int failure=0;assert(failed.Begin(1,[](bool){}));auto f=failed.active_generation_.load();
 failed.Cancel();assert(failed.Begin(2,[&](bool ok){if(!ok)++failure;}));assert(failed.Enqueue("x"));
 fail_create=true;failed.CompleteWorker(f);assert(failure==1&&!failed.task_&&failed.queue_.empty());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);(p/'test.cc').write_text(code)
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-Wno-misleading-indentation','-I',str(ROOT/'main/boards/wzh_crawlPanda/voice_tts'),
                str(p/'test.cc'),'-o',str(p/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(p/'test')],check=True,timeout=10)
