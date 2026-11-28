import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class AudioHubLazyTaskTest(unittest.TestCase):
    def test_manual_task_releases_stack_after_each_job_and_retries_failure(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/boards/common/wzh/wzh_audio_hub.cc').read_text()
        names = ['bool AudioHub::StartManual(', 'bool AudioHub::StartManualTaskLocked(',
                 'bool AudioHub::RequestCheck(', 'bool AudioHub::RequestUpdate(',
                 'void AudioHub::TaskEntry(', 'void AudioHub::ManualTask()']
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
int calls=0, allow_create=0, deleted=0, checks=0, updates=0;
void (*pending)(void*)=nullptr;void* arg=nullptr;
int xTaskCreate(void(*fn)(void*),const char*,int,void* a,int,TaskHandle_t*) {
 ++calls;if(allow_create){assert(!pending);pending=fn;arg=a;return pdPASS;}return 0;
}
void vTaskDelete(void*){++deleted;}
struct AudioStore {};
struct AudioHub {
 std::mutex job_mutex_;AudioStore* store_=nullptr;std::function<bool()> interaction_busy_;
 bool manual_mode_=false,local_maintenance_=false;
 std::atomic<bool> job_running_{false};
 std::vector<std::string> update_names_;
 static void TaskEntry(void*);void Task(){ManualTask();}void ManualTask();
 void DoCheck(){++checks;assert(job_running_);}
 void DoUpdate(){++updates;assert(job_running_&&update_names_[0]=="sound");}
 bool StartManual(AudioStore*,std::function<bool()>);
 bool StartManualTaskLocked();bool RequestCheck();bool RequestUpdate(const std::vector<std::string>&);
};
''' + methods + r'''
int main(){
 AudioHub h;AudioStore store;assert(h.StartManual(&store,{}));assert(calls==0);
 assert(!h.RequestCheck());assert(calls==1 && !h.job_running_);
 assert(!h.RequestUpdate({"sound"})&&h.update_names_.empty());
 allow_create=1;assert(h.RequestCheck());assert(calls==3&&pending&&h.job_running_);
 assert(!h.RequestUpdate({"sound"}));assert(calls==3);
 auto run=[](){auto fn=pending;pending=nullptr;fn(arg);};
 run();assert(checks==1&&deleted==1&&!h.job_running_);
 assert(h.RequestUpdate({"sound"}));assert(calls==4&&pending);
 run();assert(updates==1&&deleted==2&&!h.job_running_&&h.update_names_.empty());
 h.local_maintenance_=true;assert(!h.RequestCheck());
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
