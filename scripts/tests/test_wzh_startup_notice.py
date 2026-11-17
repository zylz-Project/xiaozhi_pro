"""Exercise each board's real startup protection observer on the host."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / 'main/boards/common/wzh'


class StartupNoticeTest(unittest.TestCase):
    def test_ble_status_serializes_and_clears_the_notice(self):
        if not shutil.which('g++') or not shutil.which('gcc'):
            self.skipTest('g++ and gcc required')
        source = (COMMON / 'wzh_ble_service.cc').read_text()
        method = queue_function(source, 'cJSON* BleService::BuildStatusObject()')
        program = r'''
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "cJSON.h"
namespace voice_tts { struct VoiceName{}; constexpr int kBuiltinVoiceCount=2;
void LoadVoiceNames(std::vector<VoiceName>&){} }
constexpr int kVoiceCapacity=10;
const char* notice="";
const char* StartupProtectionMessage(){return notice;}
struct OnboardingProgress{static constexpr int kStepCount=13;int step=7;bool Done(){return false;}};
bool PeekOnboardingProgress(OnboardingProgress&){return true;}
bool IsOnboardingActive(){return false;}bool OnboardingBlocksConversation(){return true;}
const char* StateName(int){return "idle";}
struct Application{static Application& GetInstance(){static Application app;return app;}
 int GetDeviceState(){return 0;}};
struct WifiManager{static WifiManager& GetInstance(){static WifiManager wifi;return wifi;}
 bool IsConnected(){return true;}int GetRssi(){return -50;}int GetChannel(){return 1;}
 std::string GetSsid(){return "home";}};
std::string BandText(int){return "2.4G";}const char* SignalText(int){return "ok";}
int SignalBars(int){return 3;}std::string RuntimeText(int){return "0";}
int JsonInt(cJSON* obj,const char* key,int fallback){auto* n=cJSON_GetObjectItem(obj,key);return cJSON_IsNumber(n)?n->valueint:fallback;}
struct Console{void BuildStatus(cJSON*){}};
struct BleService{struct Config{Console* console=nullptr;}config_;
 cJSON* BuildStatusObject();std::string CurrentVoiceId(){return "v";}
 std::string CurrentVoiceName(){return "voice";}};
''' + method + r'''
int main(){
 BleService service;
 for(const char* value:{"", "设备温度过高"}){
  notice=value;auto* status=service.BuildStatusObject();
  auto* item=cJSON_GetObjectItem(status,"startup_notice");
  assert(cJSON_IsString(item) && std::strcmp(item->valuestring,value)==0);
  auto* guide=cJSON_GetObjectItem(status,"onboarding");
  assert(cJSON_IsObject(guide) && cJSON_IsTrue(cJSON_GetObjectItem(guide,"blocked")));
  assert(cJSON_GetObjectItem(guide,"next_step")->valueint==7);
  cJSON_Delete(status);
 }
}
'''
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory); cpp = tmp / 'status.cc'; cpp.write_text(program)
            subprocess.run(['gcc', '-c', str(cjson / 'cJSON.c'), '-o', str(tmp / 'cjson.o')], check=True)
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(cjson), str(cpp), str(tmp / 'cjson.o'),
                                     '-o', str(tmp / 'status')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(tmp / 'status')], check=True, timeout=5)

    def test_all_boards_report_protection_and_clear_only_after_greeting(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        for board, stem in (('wzh_crawlPanda', 'panda'), ('wzh_dinosaur', 'dinosaur'),
                            ('wzh_tailRedPanda', 'panda')):
            source = (ROOT / 'main/boards' / board / f'{stem}_power.cc').read_text()
            block = queue_function(source, 'if (startup_protection_monitoring && now >= 5000)')
            program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include "wzh_protection.h"
#include "wzh_system_prompts.h"
constexpr int BATTERY_LOW_MV=3300;
bool greeting=false;
namespace wzh {
StartupProtectionNotice last=StartupProtectionNotice::kNone;
void ObserveStartupProtection(StartupProtectionNotice n){last=n;}
bool StartupGreetingFinished(){return greeting;}
}
struct Output{uint32_t mask=0;uint32_t faults()const{return mask;}};
struct Battery{bool is_ready=true,is_low=false;bool ready()const{return is_ready;}bool low()const{return is_low;}};
struct Power{
 std::atomic<Output*> servo_output_{nullptr};Battery battery_protection_;
 std::atomic<bool>battery_sample_valid_{false};std::atomic<int>battery_live_mv_{0};
 bool startup_protection_monitoring=true;
 void check(uint32_t now){__BLOCK__}
};
int main(){
 Output out;Power p;p.servo_output_=&out;
 p.check(4999);assert(wzh::last==wzh::StartupProtectionNotice::kNone);
 out.mask=wzh::kOverheat;p.check(5000);assert(wzh::last==wzh::StartupProtectionNotice::kOverheat);
 out.mask=wzh::kFalling;p.check(6000);assert(wzh::last==wzh::StartupProtectionNotice::kMotionBlocked);
 out.mask=wzh::kBatteryPending;p.check(7000);assert(wzh::last==wzh::StartupProtectionNotice::kCheckingBattery);
 out.mask=0;p.battery_protection_.is_low=true;p.check(15000);assert(wzh::last==wzh::StartupProtectionNotice::kLowBattery);
 p.battery_protection_.is_low=false;p.check(16000);assert(wzh::last==wzh::StartupProtectionNotice::kNone);
 assert(p.startup_protection_monitoring);greeting=true;p.check(16001);assert(!p.startup_protection_monitoring);
}
'''.replace('__BLOCK__', block)
            with self.subTest(board=board), tempfile.TemporaryDirectory() as directory:
                cpp = Path(directory) / 'test.cc'; cpp.write_text(program)
                exe = Path(directory) / 'test'
                result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                         '-I', str(COMMON), str(cpp), '-o', str(exe)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(exe)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
