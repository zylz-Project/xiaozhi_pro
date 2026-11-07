"""Exercise recording quiet entry, touch gating and the production task branch."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function
ROOT=Path(__file__).resolve().parents[2]

class PandaRecordingQuietTest(unittest.TestCase):
    def test_recording_releases_servo_and_rejects_touch(self):
        if not shutil.which('g++'): self.skipTest('g++ required')
        source=(ROOT/'main/boards/wzh_crawlPanda/panda_controller.cc').read_text()
        toggle=queue_function(source,'void PandaController::SetVoiceRecordingQuiet(')
        touch=queue_function(source,'void PandaController::HandleTouch(')
        task=queue_function(source,'void PandaController::Task(')
        start=task.index('        if (voice_recording_quiet_.load()) {')
        end=task.index('\n        // 明确请求',start)
        quiet=task[start:end].replace('            continue;','            return true;')
        program=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
using TickType_t=unsigned;
constexpr int kTickMs=20;
#define pdMS_TO_TICKS(x) (x)
unsigned esp_timer_get_time(){return 123000;}
unsigned xTaskGetTickCount(){return 1;}
void vTaskDelayUntil(TickType_t*,int){}
int resets=0;void xQueueReset(void*){++resets;}
struct Pet {int touches=0;void Touch(uint8_t,unsigned){++touches;}};
struct Servo {int releases=0;void Release(){++releases;}};
struct PandaController {
 std::atomic<bool> voice_recording_quiet_{false},request_active_{true};
 std::atomic<unsigned> intent_generation_{0};std::atomic<int> pending_expression_{3};
 bool autonomous_playback_=true,clip_active_=true,running_speech_bound_=true;void* action_queue_=nullptr;
 Servo servo_output_;Pet pet_;int polls=0,cancels=0;
 void CancelSoundMotion(){++cancels;}void PollInteraction(unsigned){++polls;}
 void SetVoiceRecordingQuiet(bool);void HandleTouch(uint8_t);
 bool QuietBranch(TickType_t& motion_tick){
''' + quiet + r'''
 return false;}
};
''' + toggle + '\n' + touch + r'''
int main(){PandaController p;TickType_t t=0;
 assert(!p.QuietBranch(t));p.SetVoiceRecordingQuiet(true);assert(p.intent_generation_==1&&p.pending_expression_==-1);
 p.SetVoiceRecordingQuiet(true);assert(p.intent_generation_==1);
 p.HandleTouch(2);assert(p.pet_.touches==0);assert(p.QuietBranch(t));
 assert(p.servo_output_.releases==1&&resets==1&&!p.request_active_&&!p.clip_active_);
 p.SetVoiceRecordingQuiet(false);p.HandleTouch(2);assert(p.pet_.touches==1&&p.polls==1&&p.intent_generation_==2);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'test.cc';p.write_text(program);exe=p.with_suffix('')
            result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(exe)],check=True,timeout=10)
