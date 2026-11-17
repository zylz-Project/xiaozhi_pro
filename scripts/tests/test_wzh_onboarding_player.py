"""Exercise BLE onboarding wait/restart behavior using the real worker."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / 'main/boards/common/wzh'


class OnboardingPlayerTest(unittest.TestCase):
    def test_waits_release_worker_and_all_chapters_block_chat(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'freertos').mkdir(); (root / 'assets').mkdir()
            (root / 'freertos/FreeRTOS.h').write_text('''#pragma once
#include <cstdint>
using TickType_t=uint32_t;
#define pdMS_TO_TICKS(v) (v)
#define pdPASS 1
''')
            (root / 'freertos/task.h').write_text('''#pragma once
#include "FreeRTOS.h"
int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*);
TickType_t xTaskGetTickCount();void vTaskDelay(TickType_t);void vTaskDelete(void*);
''')
            (root / 'esp_log.h').write_text('''#pragma once
template<class... T>void log_stub(T...){ }
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
''')
            (root / 'wifi_manager.h').write_text('''#pragma once
extern bool wifi_online;
struct WifiManager{static WifiManager& GetInstance(){static WifiManager w;return w;}
 bool IsConnected(){return wifi_online;}};
''')
            (root / 'board.h').write_text('''#pragma once
#include <cstdint>
struct Board{static Board& GetInstance(){static Board b;return b;}
 bool TriggerOnboardingFeedback(uint8_t){return true;}
 bool IsOnboardingFeedbackPending()const{return false;}
 bool IsAudioMaintenanceActive(){return false;}bool IsLocalPlaybackPending(){return false;}};
''')
            names = ('INTRO TOUCH MOTION CMD1 NET_INVITE WAKE AP PAGE CONNECTED ABILITY '
                     'INTERRUPT CLONE FAREWELL').split()
            (root / 'assets/lang_config.h').write_text(
                '#pragma once\n#include <string_view>\nnamespace Lang { namespace Sounds {\n' +
                '\n'.join('constexpr std::string_view OGG_' + name + '="' + name + '";'
                          for name in names) + '\n}}\n')
            (root / 'application.h').write_text('''#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
extern uint32_t clock_ms;
constexpr int kDeviceStateIdle=0;
struct Audio{uint32_t until=0,generation=1;std::vector<std::string_view> played;
 bool IsPlaybackIdle(){return clock_ms>=until;}bool IsAudioProcessorRunning(){return false;}
 uint32_t GetPlaybackGeneration(){return generation;}
 void PlaySound(std::string_view sound,uint32_t){played.push_back(sound);until=clock_ms+100;}};
struct Application{Audio audio;static Application& GetInstance(){static Application a;return a;}
 int GetDeviceState(){return kDeviceStateIdle;}Audio& GetAudioService(){return audio;}};
''')
            (root / 'test.cc').write_text(r'''
#include <cassert>
#include <cstdint>
#include "wzh_onboarding_store.h"
uint32_t clock_ms=0;bool wifi_online=false,fail_create=false,fail_load=false,fail_save=false;
void(*worker)(void*)=nullptr;
int xTaskCreate(void(*fn)(void*),const char*,int,void*,int,void*){
 if(fail_create)return 0;
 worker=fn;return 1;}
uint32_t xTaskGetTickCount(){return clock_ms;}
void vTaskDelay(uint32_t ms){clock_ms+=ms;assert(clock_ms<500000);}
void vTaskDelete(void*){}
namespace wzh {
OnboardingProgress saved;
bool LoadOnboardingProgress(OnboardingProgress& p){if(fail_load)return false;p=saved;p.Resume();return true;}
bool SaveOnboardingProgress(const OnboardingProgress& p){if(fail_save)return false;saved=p;return true;}
}
#include "wzh_onboarding.cc"
void run(){assert(worker);auto fn=worker;worker=nullptr;fn(nullptr);}
int main(){
 using namespace wzh;
 StartOnboarding();assert(worker && OnboardingBlocksConversation());run();
 assert(saved.step==7 && !worker && !IsOnboardingActive() && OnboardingBlocksConversation());
 assert(Application::GetInstance().audio.played.size()==7);
 NotifyOnboarding(OnboardingEvent::kBleConnected);assert(worker);run();
 assert(saved.step==7 && !worker && !IsOnboardingActive() && OnboardingBlocksConversation());
 assert(Application::GetInstance().audio.played.size()==8);
 NotifyOnboarding(OnboardingEvent::kWifiRequest);assert(worker);run();
 assert(saved.step==8 && !worker && OnboardingBlocksConversation());
 NotifyOnboarding(OnboardingEvent::kNetworkConnected);
 NotifyOnboarding(OnboardingEvent::kServiceReady);assert(!worker);
 NotifyOnboarding(OnboardingEvent::kServiceLost);
 NotifyOnboarding(OnboardingEvent::kNetworkLost);
 PrepareOnboardingDoubleClick();assert(!worker);
 NotifyOnboarding(OnboardingEvent::kNetworkConnected);assert(!worker);
 NotifyOnboarding(OnboardingEvent::kServiceConfigured);assert(worker);run();
 assert(saved.Done() && !worker && !IsOnboardingActive() && !OnboardingBlocksConversation());
 assert(Application::GetInstance().audio.played.size()==13);
 // A failed task allocation keeps the guide pending and can be retried.
 saved=OnboardingProgress{};started.store(false);stopping.store(false);wifi_online=false;
 ble_connected.store(false);wifi_requested.store(false);wifi_narrated.store(false);
 service_verified.store(false);online_double_click.store(false);
 fail_create=true;worker=nullptr;StartOnboarding();assert(!worker && OnboardingBlocksConversation());
 fail_create=false;TryResumeOnboarding();assert(worker);run();assert(saved.step==7);
 // Failed persistence loading must not permanently block the product.
 StopOnboarding();started.store(false);stopping.store(false);fail_load=true;worker=nullptr;
 StartOnboarding();assert(worker);run();assert(!OnboardingBlocksConversation());
 // A failed NVS write must also release the conversation gate.
 fail_load=false;fail_save=true;saved=OnboardingProgress{};StartOnboarding();assert(worker);run();
 assert(!worker && !IsOnboardingActive() && !OnboardingBlocksConversation());
}
''')
            exe = root / 'test'
            result = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                                     '-DCONFIG_WZH_SYSTEM_PROMPTS=1', '-I', str(root),
                                     '-I', str(COMMON), str(root / 'test.cc'), '-o', str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
