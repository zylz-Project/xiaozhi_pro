"""Regression tests for shared behaviors surfaced by the BLE mini program."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class BleParityTests(unittest.TestCase):
    def run_cpp(self, code):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cc').write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/boards/common/wzh'),
                            str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True, timeout=10)

    def test_touch_is_bounded_and_never_replayed_after_stop_or_during_speech(self):
        source = (ROOT / 'main/application.cc').read_text()
        methods = '\n'.join(queue_function(source, 'void Application::' + name + '(')
                            for name in ['QueueTouchConversationInput', 'SendPendingTouchConversationInput'])
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <mutex>
#include <vector>
#include "wzh_conversation_touch.h"
int64_t now=10000000;
int64_t esp_timer_get_time(){ return now; }
enum DeviceState { kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking };
struct Audio { bool voice=false, idle=true;
 bool IsVoiceDetected(){return voice;} bool IsPlaybackIdle(){return idle;} };
struct Protocol { bool opened=true; std::vector<std::string> messages;
 bool IsAudioChannelOpened(){return opened;}
 void SendListenText(const std::string& text){messages.push_back(text);} };
struct Board { static Board& GetInstance(){static Board b;return b;}
 bool IsAudioMaintenanceActive(){return false;} };
struct Application {
 std::atomic<uint32_t> interaction_generation_{1},protocol_generation_{1};
 std::atomic<bool> interaction_stopped_{false},voice_enrollment_active_{false},
   external_voice_preview_active_{false},tts_start_pending_{false},
   barge_in_reconnecting_{false},voice_enrollment_reconnecting_{false};
 std::atomic<int64_t> last_listening_voice_edge_us_{0};
 bool pending_listening_start_=false,network_connected_=true;
 int64_t next_touch_input_us_=0;
 DeviceState state=kDeviceStateListening;
 DeviceState GetDeviceState(){return state;}
 struct TouchConversationInput{std::string text;uint32_t interaction_generation=0,
   protocol_generation=0;int64_t received_us=0;};
 std::mutex touch_input_mutex_; TouchConversationInput pending_touch_input_;
 Audio audio_service_; Protocol transport; Protocol* protocol_=&transport;
 void QueueTouchConversationInput(std::string);
 void SendPendingTouchConversationInput();
};
''' + methods + r'''
int main(){
 using namespace wzh;
 const std::array<std::string_view,4> pads{"头","背","肚子","尾巴"};
 assert(ConversationTouchText({TouchResponse::kNone,8},pads).empty());
 PetBehavior dino(true);
 dino.Touch(4, 100); assert(dino.PollTouch(100) == TouchResponse::kNone);
 dino.Touch(0, 200); dino.Touch(8, 390);
 const auto forward = dino.PollTouchEvent(390);
 assert(forward.response == TouchResponse::kStrokeForward);
 assert(ConversationTouchText(forward, pads).find("顺摸") != std::string::npos);
 dino.Touch(0, 410); dino.Touch(8, 500); dino.Touch(12, 530);
 assert(dino.PollTouch(530) == TouchResponse::kStrokeReverse);
 dino.Touch(0, 540); dino.Touch(4, 600); dino.Touch(0, 650);
 assert(dino.PollTouch(881) == TouchResponse::kAffection);
 dino.Touch(0xff, 900); dino.Touch(8, 910);
 assert(dino.PollTouch(2000) == TouchResponse::kNone);
 dino.Touch(0, 2100); dino.Touch(1, 2200, false);
 assert(dino.PollTouch(2200) == TouchResponse::kNone);
 assert(ConversationTouchText({TouchResponse::kAffection,16},pads).empty());
 const auto phrase=ConversationTouchText({TouchResponse::kPlayful,9},pads);
 assert(phrase.find("头、尾巴")!=std::string::npos);
 Application app; auto& messages=app.transport.messages;
 app.QueueTouchConversationInput("first"); app.QueueTouchConversationInput(phrase);
 app.SendPendingTouchConversationInput(); assert(messages.empty());
 now+=400000; app.audio_service_.voice=true;
 app.SendPendingTouchConversationInput(); assert(messages.empty());
 app.audio_service_.voice=false; app.last_listening_voice_edge_us_.store(now);
 now+=1200000; app.SendPendingTouchConversationInput();
 assert(messages.size()==1 && messages[0]==phrase);
 app.QueueTouchConversationInput("too early"); now+=1000000;
 app.SendPendingTouchConversationInput(); assert(messages.size()==1);
 ++app.interaction_generation_; now+=4000000;
 app.SendPendingTouchConversationInput(); assert(messages.size()==1);
 app.QueueTouchConversationInput("old protocol"); ++app.protocol_generation_; now+=400000;
 app.SendPendingTouchConversationInput(); assert(messages.size()==1);
 app.QueueTouchConversationInput("expired"); now+=7000000;
 app.SendPendingTouchConversationInput(); assert(messages.size()==1);
}
''')

    def test_record_cancel_waits_for_device_and_does_not_accept_a_different_record(self):
        if not shutil.which('node'):
            self.skipTest('node required')
        code = r'''
const assert = require('assert')
let page
Page = p => { page = p }
require(process.argv[1])
const d = Object.assign({}, page, { data: structuredClone(page.data) })
d.setData = f => Object.assign(d.data, f)
d.toast = () => {}
d.refreshVoices = () => {}
global.setInterval = () => 1
global.clearInterval = () => {}
let status = {status: 'recording', request: 17}
let calls = 0
const yieldToPromise = () => new Promise(resolve => setImmediate(resolve))
d.client = {request: async (op, data) => {
  if (op === 'voice_record') return {accepted: true, request: 17}
  if (op === 'voice_record_status') return status
  if (op === 'voice_record_cancel') { assert.deepStrictEqual(data, {request: 17}); calls++; return {} }
  throw new Error(op)
}}
;(async () => {
  d.data.audioName = '测试'
  d.startRecord(); await yieldToPromise()
  assert(d.data.audioRecordPending && d.data.audioRecordCancellable)
  status = {status: 'cancelled', request: 16}
  await d.cancelDeviceRecord(); await yieldToPromise()
  assert.strictEqual(calls, 1)
  assert(d.data.audioRecordPending)
  status = {status: 'cancelled', request: 17}
  d.pollDeviceRecord(); await yieldToPromise()
  assert(!d.data.audioRecordPending && d.data.audioError.includes('已取消'))
  await d.cancelDeviceRecord()
  assert.strictEqual(calls, 1)
})().catch(e => {console.error(e);process.exitCode=1})
'''
        subprocess.run(['node', '-e', code, str(ROOT / 'wzh-miniprogram/pages/device/device.js')],
                       check=True, timeout=10)

    def test_idle_power_rearms_on_activity_and_maintenance_across_tick_wrap(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_idle_power.h"
int main() {
    wzh::IdlePower power;
    uint32_t start = UINT32_MAX - 100;
    power.Observe(start, 1, true);
    assert(!power.ReminderDue(start + 299999));
    assert(power.ReminderDue(start + 300000));
    power.ReminderQueued();
    assert(!power.ReminderDue(start + 300001));
    assert(!power.ShutdownDue(start + 449999));
    assert(power.ShutdownDue(start + 450000));
    start += 450000;
    power.Observe(start, 3, true);
    assert(!power.ShutdownDue(start));
    power.Observe(start + 300000, 3, false);
    assert(!power.ReminderDue(start + 910000));
    assert(!power.ShutdownDue(start + 910000));
    start += 910000;
    power.Observe(start, 3, true);
    assert(power.ReminderDue(start + 359999));
    assert(!power.ReminderDue(start + 360000));
    assert(power.ShutdownDue(start + 450000));
}
''')

    def test_candidate_wifi_never_overwrites_working_password_before_dhcp(self):
        source = (ROOT / 'project_components/esp-wifi-connect/wifi_manager.cc').read_text()
        method = queue_function(source, 'bool WifiManager::TestAndSaveStationNetwork(')
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
std::vector<std::string> calls;
constexpr int WIFI_STORAGE_RAM = 1, ESP_OK = 0;
constexpr int WIFI_REASON_AUTH_FAIL=202, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT=15,
              WIFI_REASON_HANDSHAKE_TIMEOUT=204;
struct wifi_ap_record_t { unsigned char ssid[33]; };
std::string associated = "candidate";
bool storage_available = true;
int esp_wifi_set_storage(int storage) {
    assert(storage == WIFI_STORAGE_RAM);
    calls.push_back("ram"); return storage_available ? ESP_OK : 1;
}
int esp_wifi_sta_get_ap_info(wifi_ap_record_t* ap) {
    std::memcpy(ap->ssid, associated.c_str(), associated.size() + 1);
    calls.push_back("associated"); return ESP_OK;
}
struct SsidManager {
    bool save_ok = true;
    static SsidManager& GetInstance() { static SsidManager manager; return manager; }
    bool AddSsidChecked(const std::string& ssid, const std::string&) {
        assert(ssid == "candidate"); calls.push_back("save"); return save_ok;
    }
};
struct Station {
    bool got_ip = true;
    bool associated = false;
    int reason = WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT;
    bool ConnectNow(const std::string&, const std::string&) {calls.push_back("connect"); return true;}
    bool WaitForConnected(int timeout) {assert(timeout == 20000); calls.push_back("dhcp"); return got_ip;}
    bool IsAssociated() const { return associated; }
    int LastDisconnectReason() const { return reason; }
};
enum class WifiEvent { Connecting };
enum class WifiTrialFailure { None, Unavailable, Association, Authentication, Dhcp, Save };
struct WifiManager {
    bool station_starts = true;
    bool IsInitialized() { return true; }
    struct { bool allow_config_ap = false; } config_;
    std::atomic<bool> station_trial_active_{false};
    Station station, *station_ = &station;
    void StopStation() { calls.push_back("stop"); }
    bool StartStationImpl(bool scan) {assert(!scan); calls.push_back("trial"); return station_starts;}
    void StartStation() { calls.push_back("restore"); }
    void NotifyEvent(WifiEvent, const std::string&) { calls.push_back("event"); }
    bool TestAndSaveStationNetwork(const std::string&, const std::string&, int, WifiTrialFailure*);
};
''' + method + r'''
int main() {
    WifiManager manager;
    WifiTrialFailure failure;
    const auto change = [&] { calls.clear(); return manager.TestAndSaveStationNetwork("candidate", "badpass1", 20000, &failure); };
    manager.station.got_ip = false;
    assert(!change() && failure == WifiTrialFailure::Authentication);
    assert((calls == std::vector<std::string>{"ram", "stop", "trial", "event", "connect", "dhcp", "stop", "restore"}));
    manager.station.associated = true;
    assert(!change() && failure == WifiTrialFailure::Dhcp);
    manager.station.got_ip = true;
    associated = "old";
    assert(!change() && calls.back() == "restore" && failure == WifiTrialFailure::Association);
    associated = "candidate";
    SsidManager::GetInstance().save_ok = false;
    assert(!change() && calls.back() == "restore" && failure == WifiTrialFailure::Save);
    SsidManager::GetInstance().save_ok = true;
    assert(change() && calls.back() == "save" && failure == WifiTrialFailure::None);
    manager.station_starts = false;
    assert(!change() && calls.back() == "restore");
    storage_available = false;
    assert(!change() && calls.size() == 1);
    manager.station_trial_active_.store(true);
    assert(!change() && calls.empty());
}
''')

    def test_mobile_recording_and_audition_report_actual_completion(self):
        if not shutil.which('node'):
            self.skipTest('node required')
        code = r'''
const assert = require('assert')
let page
Page = p => { page = p }
let started, stopped, failed, stops = 0
wx = { getRecorderManager: () => ({
  onStart: fn => { started = fn }, onStop: fn => { stopped = fn }, onError: fn => { failed = fn },
  start: options => assert.deepStrictEqual(options, {
    duration: 20000, sampleRate: 16000, numberOfChannels: 1, format: 'wav'
  }),
  stop: () => { stops++ }
}) }
require(process.argv[1])
const d = Object.assign({}, page, { data: structuredClone(page.data) })
d.setData = f => Object.assign(d.data, f)
d.startRecordClock = () => {}
d.stopRecordClock = () => {}
d.toast = text => { d.message = text }
const yieldToPromise = () => new Promise(resolve => setImmediate(resolve))
let poll = null, cancelCount = 0, status = 'playing'
setTimeout = fn => { poll = fn; return 1 }
d.client = {request: async (op, params) => {
  if (op === 'audition') return {request: 18, accepted: true}
  if (op === 'audition_status') return {request: 18, status}
  if (op === 'audition_stop') { assert.strictEqual(params.request, 18); cancelCount++; return {} }
  throw new Error(op)
}}
;(async () => {
  d.data.audioMode = 'phone'
  d.togglePhoneRecording(); started()
  assert(d.data.audioRecording)
  d.togglePhoneRecording(); assert.strictEqual(stops, 1)
  stopped({duration: 3000, tempFilePath: 'short.wav'})
  assert(!d.data.audioRecording && !d.data.audioPhonePending && d.data.audioError.includes('不足'))
  d.togglePhoneRecording(); started()
  d.cancelPhoneRecording()
  stopped({duration: 6000, tempFilePath: 'cancelled.wav'})
  assert.strictEqual(d.data.audioMode, 'phone')
  d.togglePhoneRecording(); started()
  stopped({duration: 6000, tempFilePath: 'ready.wav'})
  assert(d.data.audioDone && d.data.audioMode === 'upload' && d.data.audioFile === 'ready.wav')
  d.togglePhoneRecording(); failed({errMsg: 'auth denied'})
  assert(!d.data.audioPhonePending && d.data.audioError.includes('设置'))
  d.onTryVoice(); await yieldToPromise()
  assert(d.data.auditioning && poll)
  d.onTryVoice(); await yieldToPromise()
  assert.strictEqual(cancelCount, 1)
  assert(d.data.auditioning)
  status = 'cancelled'; poll(); await yieldToPromise()
  assert(!d.data.auditioning && d.message.includes('停止试听'))
  status = 'complete'; d.onTryVoice(); await yieldToPromise()
  assert(!d.data.auditioning)
})().catch(e => {console.error(e);process.exitCode=1})
'''
        subprocess.run(['node', '-e', code, str(ROOT / 'wzh-miniprogram/pages/device/device.js')],
                       check=True, timeout=10)
