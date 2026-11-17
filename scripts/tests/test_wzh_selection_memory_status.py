"""Check actual voice-selection admission status on the production task path."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class SelectionMemoryStatusTest(unittest.TestCase):
    def test_config_busy_memory_and_recovery(self):
        if not shutil.which("g++"):
            self.skipTest("C++ compiler required")
        source = (ROOT / "main/boards/common/wzh/voice_tts/ali_voice_enrollment.cc").read_text()
        method = queue_function(source, "bool AliVoiceEnrollment::StartSelection(")
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#define CONFIG_WZH_VOICE_FLASH_DISPATCH_EXPERIMENT 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
using TaskHandle_t=void*; constexpr int pdPASS=1;
bool configured=true, task_ok=true;
size_t heap_caps_get_free_size(int){return 40000;}
size_t heap_caps_get_largest_free_block(int){return 8000;}
int xTaskCreate(void(*)(void*),const char*,int,void*,int,TaskHandle_t* out){
    if(!task_ok)return 0;*out=reinterpret_cast<void*>(1);return pdPASS;
}
namespace voice_tts {
constexpr char kTag[]="test";
std::string LoadAliKey(){return configured?"key":"";}
struct AliVoiceEnrollment {
    enum class Error{kNone,kBusy,kConfig,kMemory};enum class Phase{kIdle,kFailed};
    std::mutex mutex_;TaskHandle_t task_=nullptr;
    std::atomic<Error> error_{Error::kNone};std::atomic<Phase> phase_{Phase::kIdle};
    std::atomic<bool> active_{false};std::atomic<uint32_t> generation_{0};
    bool selection_task_=false;std::string selection_text_;std::function<void(bool)> complete_;
    static void TaskEntry(void*){}
    bool StartSelection(const std::string&,std::function<void(bool)>);
};
''' + method + r'''
}
int main(){using E=voice_tts::AliVoiceEnrollment;E e;
    auto done=[](bool){};
    configured=false;assert(!e.StartSelection("切换音色",done));
    assert(e.error_==E::Error::kConfig&&e.phase_==E::Phase::kFailed);
    configured=true;task_ok=false;assert(!e.StartSelection("切换音色",done));
    assert(e.error_==E::Error::kMemory&&e.phase_==E::Phase::kFailed&&!e.active_&&!e.task_);
    task_ok=true;assert(e.StartSelection("切换音色",done));
    assert(e.error_==E::Error::kNone&&e.phase_==E::Phase::kIdle&&e.active_&&e.task_);
    assert(!e.StartSelection("切换音色",done));assert(e.error_==E::Error::kBusy);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cc"
            exe = Path(directory) / "test"
            cpp.write_text(code)
            subprocess.run(["g++", "-std=c++17", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
