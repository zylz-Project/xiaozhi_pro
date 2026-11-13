"""Exercise enrollment task reuse and allocation fallback."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class EnrollmentWorkerTest(unittest.TestCase):
    def test_reuse_and_fallback(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        source = (ROOT / 'main/boards/common/wzh/voice_tts/ali_voice_enrollment.cc').read_text()
        methods = '\n'.join(queue_function(source, 'bool AliVoiceEnrollment::' + name + '(')
                            for name in ['StartNamed', 'StartSelection', 'EnsureWorker'])
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#define CONFIG_WZH_VOICE_FLASH_DISPATCH_EXPERIMENT 1
#define ESP_LOGI(tag,fmt,arg) do {(void)(tag);(void)(fmt);(void)(arg);} while(0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
using TaskHandle_t=void*; constexpr int pdPASS=1;
static int external=0,internal=0,notifications=0;static bool ready=true,fail=false;
size_t heap_caps_get_free_size(int){return 32768;}
size_t heap_caps_get_largest_free_block(int){return 8192;}
int xTaskCreateWithCaps(void(*)(void*),const char*,int,void*,int,TaskHandle_t* out,int caps){
 assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));++external;if(fail)return 0;
 *out=reinterpret_cast<void*>(1);return pdPASS;
}
int xTaskCreate(void(*)(void*),const char*,int,void*,int,TaskHandle_t* out){
 ++internal;if(fail)return 0;*out=reinterpret_cast<void*>(2);return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task){assert(task);++notifications;}
namespace voice_tts {
constexpr char kTag[]="test";
bool FlashDispatcherReady(){return ready;}
std::string LoadAliKey(){return "key";}bool IsValidVoiceName(const std::string&){return true;}
struct AliVoiceEnrollment {
 enum class Error { kNone,kBusy,kConfig,kMemory };enum class Phase {kPrompt,kFailed};
 std::mutex mutex_;TaskHandle_t task_=nullptr,worker_task_=nullptr;
 std::atomic<Error> error_{Error::kNone};std::atomic<Phase> phase_{Phase::kPrompt};
 std::atomic<bool> active_{false};std::atomic<uint32_t> generation_{0};
 std::atomic<int64_t> selection_deadline_us_{0};bool selection_task_=false;
 std::string selection_text_,requested_name_;std::function<void(bool)> complete_,capture_switch_;
 static void TaskEntry(void*){}
 bool StartNamed(const std::string&,std::function<void(bool)>,std::function<void(bool)>);
 bool StartSelection(const std::string&,std::function<void(bool)>);bool EnsureWorker(const char*);
};
''' + methods + r'''
}
int main(){using voice_tts::AliVoiceEnrollment;
 AliVoiceEnrollment e;assert(e.StartSelection("切换音色",[](bool){}));
 assert(external==1&&internal==0&&notifications==1&&e.task_==e.worker_task_);
 assert(!e.StartSelection("切换音色",[](bool){}));e.task_=nullptr;e.active_=false;
 assert(e.StartNamed("name",[](bool){},[](bool){}));assert(external==1&&notifications==2);
 AliVoiceEnrollment fallback;ready=false;
 assert(fallback.StartSelection("切换音色",[](bool){}));assert(internal==1&&external==1);
 AliVoiceEnrollment failed;ready=true;fail=true;
 assert(!failed.StartNamed("name",[](bool){},[](bool){}));
 assert(!failed.task_&&!failed.worker_task_&&!failed.active_);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / 'test.cc').write_text(code)
            subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                            str(p / 'test.cc'), '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True, timeout=10)
