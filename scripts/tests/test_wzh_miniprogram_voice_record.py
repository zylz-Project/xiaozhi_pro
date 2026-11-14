"""Exercise the actual mini-program recording page and BLE task selection."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class MiniProgramVoiceRecordTest(unittest.TestCase):
    def test_failure_reason_is_not_reused_before_a_new_start_attempt(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/application.cc').read_text()
        method = queue_function(source, 'const char* Application::DeviceVoiceEnrollmentFailure() const')
        code = r'''
#include <atomic>
#include <cassert>
#include <cstring>
struct Enrollment {const char* Failure(){return "short";}};
struct Board {Enrollment e;static Board& GetInstance(){static Board b;return b;}
 Enrollment* GetVoiceEnrollment(){return &e;}};
struct Application {std::atomic<bool> device_voice_enrollment_attempted_{false};
 const char* DeviceVoiceEnrollmentFailure() const;};
''' + method + r'''
int main(){Application a;assert(std::strcmp(a.DeviceVoiceEnrollmentFailure(),"")==0);
 a.device_voice_enrollment_attempted_=true;
 assert(std::strcmp(a.DeviceVoiceEnrollmentFailure(),"short")==0);}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cc').write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True, timeout=10)

    def test_real_page_tracks_device_phases_and_recovers_poll_errors(self):
        if not shutil.which('node'):
            self.skipTest('node required')
        code = r'''
const assert = require('assert')
let page
let timers = 0
global.Page = p => { page = p }
global.setInterval = () => { timers++; return timers }
global.clearInterval = () => {}
require(process.argv[1])
const instance = Object.assign({}, page, { data: structuredClone(page.data) })
instance.setData = fields => Object.assign(instance.data, fields)
instance.toast = () => {}
instance.refreshVoices = () => { instance.refreshed = true }
let status = {status: 'recording'}
let requests = []
instance.client = {request: (op, data) => {
 requests.push([op, data])
 if (op === 'voice_record') { status = {status:'recording'}; return Promise.resolve({accepted: true}) }
 if (op === 'voice_record_status') return status instanceof Error ? Promise.reject(status) : Promise.resolve(status)
 return Promise.reject(Error('unexpected request'))
}}
const flush = () => new Promise(resolve => setImmediate(resolve))
;(async () => {
 instance.data.audioName = '测试音色'
 instance.startRecord()
 await flush(); await flush()
 assert.strictEqual(requests[0][0], 'voice_record')
 assert.strictEqual(requests[0][1].statusPoll, true)
 assert.strictEqual(instance.data.audioRecording, true)
 for (let i = 0; i < 100; ++i) instance.tickRecord()
 assert.strictEqual(instance.data.audioElapsed, 10)
 assert.strictEqual(instance.data.audioRecording, true)
 assert(!instance.data.audioRecordStatus.includes('录音结束'))
 const before = timers
 instance.pollDeviceRecord(); await flush()
 assert.strictEqual(timers, before) // repeated recording status must not restart clock
 instance.cancelAudio()
 assert.strictEqual(instance.data.audioRecordPending, true) // closing is not device cancellation
 instance.openAudioModal()
 assert.strictEqual(instance.data.audioModal, true)
 assert.strictEqual(instance.data.audioElapsed, 10)
 for (let i = 0; i < 100; ++i) instance.tickRecord()
 assert.strictEqual(instance.data.audioElapsed, 20)
 assert(instance.data.audioRecordStatus.includes('等待'))
 status = Error('temporary BLE failure')
 instance.pollDeviceRecord(); await flush()
 assert.strictEqual(instance.data.audioRecordPending, true)
 status = {status:'processing', reason:'short'}
 instance.pollDeviceRecord(); await flush()
 assert(instance.data.audioError.includes('太短'))
 assert.strictEqual(instance.data.audioRecordPending, true)
 instance.onBleEvent('voice_record', {status:'recording'}) // late old event cannot regress
 assert.strictEqual(instance.data.audioRecording, false)
 status = {status:'failed', reason:'short'}
 instance.pollDeviceRecord(); await flush()
 assert.strictEqual(instance.data.audioRecordPending, false)
 assert(instance.data.audioError.includes('太短'))
 instance.startRecord(); await flush(); await flush()
 status = {status:'success'}
 instance.pollDeviceRecord(); await flush()
 assert.strictEqual(instance.data.audioDone, true)
 assert.strictEqual(instance.refreshed, true)
 instance.startRecord(); await flush(); await flush()
 status = Object.assign(Error('unknown operation'), {code:1002})
 instance.pollDeviceRecord(); await flush()
 assert.strictEqual(instance._recordPolling, false)
 instance.onBleEvent('voice_record', {status:'failed', reason:'short'})
 assert.strictEqual(instance.data.audioRecordPending, false) // legacy event still works
 instance.client.request = () => Promise.reject(Object.assign(Error('timeout'), {code:1009}))
 instance.startRecord(); await flush()
 assert(instance.data.audioError.includes('结果未知')) // acceptance may have been lost
})().catch(err => { console.error(err); process.exitCode = 1 })
'''
        subprocess.run(['node', '-e', code, str(ROOT / 'wzh-miniprogram/pages/device/device.js')],
                       check=True, timeout=10)

    def test_ble_record_preserves_legacy_event_task_but_polling_needs_no_task(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/boards/common/wzh/wzh_ble_service.cc').read_text()
        method = queue_function(source, 'void BleService::HandleVoiceRecord(')
        code = r'''
#include <cassert>
#include <string>
struct cJSON { bool poll = false; };
cJSON* cJSON_GetObjectItemCaseSensitive(cJSON* o,const char*) {return o;}
bool cJSON_IsTrue(cJSON* o) {return o && o->poll;}
cJSON* cJSON_CreateObject() {return new cJSON;}
void cJSON_AddBoolToObject(cJSON*,const char*,int){}
void cJSON_AddStringToObject(cJSON*,const char*,const char*){}
std::string JsonString(cJSON*,const char*) {return "ok";}
namespace voice_tts {bool IsValidVoiceName(const std::string& s){return !s.empty();}}
struct Application {
 static Application& GetInstance(){static Application a;return a;}
 bool IsVoiceEnrollmentActive(){return false;}
 bool IsExternalVoicePreviewActive(){return false;}
 bool RequestDeviceVoiceEnrollment(const std::string&){return true;}
};
bool VoiceManagementBusy(){return false;}
int tasks=0; void* pending=nullptr;
constexpr int pdPASS=1;
int xTaskCreate(void(*)(void*),const char*,int,void* arg,int,void*) {tasks++;pending=arg;return pdPASS;}
void vTaskDelete(void*){}
#define ESP_LOGW(...) ((void)0)
struct BleService;
struct VoiceWatchArgs {BleService* service;std::string name;};
struct BleService {
 void HandleVoiceRecord(int,cJSON*);
 static void VoiceWatchTask(void*){}
 void SendError(int,int,const char*){assert(false);}
 void SendResponse(int,cJSON* obj){delete obj;}
};
''' + method + r'''
int main(){BleService s;cJSON legacy,modern;modern.poll=true;
 s.HandleVoiceRecord(1,&modern);assert(tasks==0);
 s.HandleVoiceRecord(2,&legacy);assert(tasks==1);delete static_cast<VoiceWatchArgs*>(pending);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cc').write_text(code)
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     str(path / 'test.cc'), '-o', str(path / 'test')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / 'test')], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
