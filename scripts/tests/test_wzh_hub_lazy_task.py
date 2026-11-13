import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class AudioHubLazyTaskTest(unittest.TestCase):
    def test_manual_task_is_created_only_on_demand_and_retries_failure(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/boards/common/wzh/wzh_audio_hub.cc').read_text()
        names = ['bool AudioHub::StartManual(', 'bool AudioHub::EnsureManualTaskLocked(',
                 'bool AudioHub::RequestCheck(', 'bool AudioHub::RequestUpdate(']
        methods = '\n'.join(queue_function(source, name) for name in names)
        program = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
constexpr int pdPASS=1;
using TaskHandle_t=void*;
int calls=0, allow_create=0, notifications=0;
int xTaskCreate(void(*)(void*),const char*,int,void*,int,TaskHandle_t* task) {
 ++calls;if(allow_create){*task=(void*)1;return pdPASS;}return 0;
}
void xTaskNotifyGive(TaskHandle_t t) {assert(t);++notifications;}
struct AudioStore {};
struct AudioHub {
 std::mutex job_mutex_;AudioStore* store_=nullptr;std::function<bool()> interaction_busy_;
 bool manual_mode_=false,local_maintenance_=false;TaskHandle_t task_handle_=nullptr;
 std::atomic<bool> job_running_{false},check_requested_{false},update_requested_{false};
 std::vector<std::string> update_names_;
 static void TaskEntry(void*) {}
 bool StartManual(AudioStore*,std::function<bool()>);
 bool EnsureManualTaskLocked();bool RequestCheck();bool RequestUpdate(const std::vector<std::string>&);
};
''' + methods + r'''
int main(){
 AudioHub h;AudioStore store;assert(h.StartManual(&store,{}));assert(calls==0);
 assert(!h.RequestCheck());assert(calls==1 && !h.job_running_);
 allow_create=1;assert(h.RequestCheck());assert(calls==2 && notifications==1);
 assert(!h.RequestUpdate({"sound"}));assert(calls==2 && notifications==1);
 h.job_running_=false;assert(h.RequestUpdate({"sound"}));
 assert(calls==2 && notifications==2 && h.update_names_[0]=="sound");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'hub.cc'
            binary = Path(directory) / 'hub'
            cpp.write_text(program)
            subprocess.run(['g++', '-std=c++17', '-pthread', str(cpp), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
