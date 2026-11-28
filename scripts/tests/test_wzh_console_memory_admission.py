"""Exercise the production console start admission under heap pressure."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class ConsoleMemoryAdmissionTest(unittest.TestCase):
    def test_admission_and_latched_failure(self):
        if not shutil.which("g++"):
            self.skipTest("C++ compiler required")
        source = (ROOT / "main/boards/common/wzh/wzh_web_console.cc").read_text()
        start = queue_function(source, "bool WebConsole::Start(")
        program = r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#define ESP_LOGI(...) ((void)0)
int64_t esp_timer_get_time(){return 1000;}
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
constexpr int MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2, pdPASS=1;
size_t free_bytes=0, largest=0;
int creates=0; bool create_ok=true;
size_t heap_caps_get_free_size(int){return free_bytes;}
size_t heap_caps_get_largest_free_block(int){return largest;}
int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*){
    ++creates; return create_ok ? pdPASS : 0;
}
struct WebConsole {
    std::atomic<bool> started_{false}, server_start_failed_{false}; std::atomic<int64_t> last_start_deferral_us_{0};
    static void StartTaskEntry(void*){}
    bool Start();
};
''' + start + r'''
int main(){
    WebConsole c;
    free_bytes=48*1024-1; largest=20*1024;
    assert(!c.Start() && creates==0);
    free_bytes=48*1024; largest=12*1024-1;
    assert(!c.Start() && creates==0);
    largest=12*1024; create_ok=false;
    assert(!c.Start() && creates==1 && !c.started_);
    create_ok=true; assert(c.Start() && creates==2 && c.started_);
    assert(c.Start() && creates==2);
    c.started_=false; c.server_start_failed_=true;
    assert(!c.Start() && creates==2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cc"
            exe = Path(directory) / "test"
            cpp.write_text(program)
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
