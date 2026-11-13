"""Exercise console task admission and retry using the production Start method."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]

class ConsoleMemoryRetryTest(unittest.TestCase):
    def test_low_memory_retry_and_idempotence(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        source = (ROOT / 'main/boards/common/wzh/wzh_web_console.cc').read_text()
        start = queue_function(source, 'bool WebConsole::Start(')
        code = r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
size_t free_bytes=4000, largest=4000; int calls=0; bool fail=false;
size_t heap_caps_get_free_size(int){return free_bytes;}
size_t heap_caps_get_largest_free_block(int){return largest;}
int xTaskCreate(void (*)(void*),const char*,int,void*,int,void*){++calls;return fail?0:pdPASS;}
struct WebConsole {std::atomic<bool> started_{false}; static void StartTaskEntry(void*){}; bool Start();};
''' + start + r'''
int main(){WebConsole w; assert(!w.Start() && calls==0); free_bytes=30000;largest=9000;
fail=true;assert(!w.Start() && !w.started_ && calls==1);fail=false;
assert(w.Start() && w.started_ && calls==2);free_bytes=0;assert(w.Start() && calls==2);}
'''
        with tempfile.TemporaryDirectory() as temp:
            src = Path(temp)/'test.cc'
            exe = Path(temp)/'test'
            src.write_text(code)
            subprocess.run(['g++', '-std=c++17', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
