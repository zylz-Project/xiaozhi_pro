"""Drive the production tutorial worker through completion and a reboot."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / 'main/boards/common/wzh'


@unittest.skipUnless(shutil.which('g++'), 'g++ required')
class OnboardingPlayerTest(unittest.TestCase):
    def test_complete_tutorial_does_not_replay_after_reboot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'freertos').mkdir()
            (root / 'assets').mkdir()
            (root / 'freertos/FreeRTOS.h').write_text(r'''
#pragma once
#include <cstdint>
using TickType_t=uint32_t;
#define pdMS_TO_TICKS(v) (v)
#define pdPASS 1
''')
            (root / 'freertos/task.h').write_text(r'''
#pragma once
#include "FreeRTOS.h"
int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*);
TickType_t xTaskGetTickCount();
void vTaskDelay(TickType_t);void vTaskDelete(void*);
''')
            (root / 'esp_log.h').write_text(r'''
#pragma once
template<class... T>void log_stub(T...){ }
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
''')
            (root / 'esp_wifi.h').write_text(r'''
#pragma once
#include <cstdint>
struct wifi_sta_list_t { int num=0; };
extern uint32_t clock_ms,ap_ready_at;
inline int esp_wifi_ap_get_sta_list(wifi_sta_list_t* list){list->num=clock_ms>=ap_ready_at;return 0;}
''')
            (root / 'nvs.h').write_text(r'''
#pragma once
#include <cstddef>
#include <cstdint>
using nvs_handle_t=int;
constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1;
constexpr int NVS_READONLY=0,NVS_READWRITE=1;
int nvs_open(const char*,int,nvs_handle_t*);int nvs_get_u8(int,const char*,uint8_t*);
int nvs_set_u8(int,const char*,uint8_t);int nvs_get_blob(int,const char*,void*,size_t*);
int nvs_set_blob(int,const char*,const void*,size_t);int nvs_commit(int);void nvs_close(int);
''')
            (root / 'wifi_manager.h').write_text(r'''
#pragma once
extern bool wifi_online;
struct WifiManager{static WifiManager& GetInstance(){static WifiManager w;return w;}
 bool IsConnected(){return wifi_online;}};
''')
            (root / 'board.h').write_text(r'''
#pragma once
#include <vector>
#include <cstdint>
extern uint32_t clock_ms,feedback_hold_ms;
struct Board{uint32_t feedback_until=0;std::vector<int> feedback;std::vector<uint32_t> feedback_times;static Board& GetInstance(){static Board b;return b;}
 bool TriggerOnboardingFeedback(uint8_t step){feedback.push_back(step);feedback_times.push_back(clock_ms);feedback_until=clock_ms+feedback_hold_ms;return true;}
 bool IsOnboardingFeedbackPending()const{return clock_ms<feedback_until;}
 bool IsAudioMaintenanceActive(){return false;}bool IsLocalPlaybackPending(){return false;}};
''')
            names = ('INTRO TOUCH MOTION CMD1 NET_INVITE WAKE AP PAGE CONNECTED ABILITY '
                     'INTERRUPT CLONE FAREWELL').split()
            (root / 'assets/lang_config.h').write_text(
                '#pragma once\n#include <string_view>\nnamespace Lang { namespace Sounds {\n' +
                '\n'.join('constexpr std::string_view OGG_' + name + '="' + name + '";'
                          for name in names) + '\n}}\n')
            (root / 'application.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>
#include "wzh_onboarding.h"
extern uint32_t clock_ms,conversation_start,conversation_end,ap_ready_at,stop_ap_at;extern bool stop_on_motion,send_service,disconnect_after_service,stop_on_ap_wait,simulate_actions,wifi_online,stop_on_ability;
constexpr int kDeviceStateIdle=0;
struct Audio {
 uint32_t until=0,generation=1;std::vector<std::string_view> played;std::vector<uint32_t> starts;
 bool IsPlaybackIdle(){return clock_ms>=until;}
 bool IsAudioProcessorRunning(){return false;}
 uint32_t GetPlaybackGeneration(){return generation;}
 void PlaySound(std::string_view sound,uint32_t){
  const bool online=sound=="CONNECTED"||sound=="ABILITY"||sound=="INTERRUPT"||sound=="CLONE"||sound=="FAREWELL";
  if(wzh::IsOnboardingVoiceMotionActive()!=online)std::abort();
  played.push_back(sound);starts.push_back(clock_ms);until=clock_ms+300;
  if(sound=="INTRO" && !wzh::OnboardingBlocksConversation())std::abort();
  if(sound=="PAGE" && !wzh::OnboardingBlocksConversation())std::abort();
  if(sound=="ABILITY" && stop_on_ability){stop_on_ability=false;wzh::StopOnboarding();++generation;until=clock_ms;return;}
  if(sound=="MOTION" && stop_on_motion){stop_on_motion=false;wzh::StopOnboarding();++generation;until=clock_ms;return;}
  if(sound=="TOUCH" && simulate_actions)wzh::NotifyOnboarding(wzh::OnboardingEvent::kTouch);
  if(sound=="MOTION" && simulate_actions)wzh::NotifyOnboarding(wzh::OnboardingEvent::kMotion);
  if(sound=="CMD1" && simulate_actions)wzh::NotifyOnboarding(wzh::OnboardingEvent::kCommand);
  // The child answers the tutorial: says the wake phrase, then opens the page.
  if(sound=="WAKE" && simulate_actions)wzh::NotifyOnboarding(wzh::OnboardingEvent::kWake);
  if(sound=="AP"){ap_ready_at=stop_on_ap_wait ? UINT32_MAX : until+15000;stop_ap_at=until+7000;}
  if(sound=="PAGE"){wifi_online=true;wzh::NotifyPortalRequest();conversation_start=until+100;conversation_end=until+2100;}
  if(sound=="PAGE" && send_service){
   wzh::NotifyOnboarding(wzh::OnboardingEvent::kServiceReady);
   if(disconnect_after_service)wzh::NotifyOnboarding(wzh::OnboardingEvent::kNetworkLost);
  }
 }
};
struct Application{Audio audio;static Application& GetInstance(){static Application a;return a;}
 int GetDeviceState(){return clock_ms>=conversation_start && clock_ms<conversation_end ? 1 : kDeviceStateIdle;} Audio& GetAudioService(){return audio;}};
''')
            (root / 'test.cc').write_text(r'''
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "nvs.h"
#include "application.h"
uint32_t clock_ms=0,conversation_start=0,conversation_end=0,ap_ready_at=0,stop_ap_at=0,feedback_hold_ms=0;bool stop_on_motion=false,send_service=true,disconnect_after_service=false,stop_on_ap_wait=false,simulate_actions=true;
bool wifi_online=false,online_test_click=false,online_saw_wait=false,stop_on_ability=false;
bool stop_late_wait=false,late_service=false,resume_after_stop=false,fail_create=false;
bool inject_touch_window=false,touch_captured=false;
void(*worker)(void*)=nullptr;
int xTaskCreate(void(*fn)(void*),const char*,int,void*,int,void*){if(fail_create)return 0;worker=fn;return 1;}
uint32_t xTaskGetTickCount(){return clock_ms;}void vTaskDelay(uint32_t n){clock_ms+=n;assert(clock_ms<400000);if(online_test_click && clock_ms>90000){wzh::PrepareOnboardingDoubleClick();if(wzh::ConsumeOnboardingConnectionProbe()){online_test_click=false;online_saw_wait=true;wzh::NotifyOnboarding(wzh::OnboardingEvent::kServiceReady);}}else if(!online_test_click && wifi_online){wzh::PrepareOnboardingDoubleClick();wzh::ConsumeOnboardingConnectionProbe();}if(inject_touch_window&&wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kTouch)){inject_touch_window=false;touch_captured=true;}if(clock_ms>=240000&&stop_late_wait){stop_late_wait=false;wzh::StopOnboarding();}if(clock_ms>=240000&&late_service){late_service=false;wzh::NotifyOnboarding(wzh::OnboardingEvent::kServiceReady);}if(stop_on_ap_wait&&stop_ap_at&&clock_ms>=stop_ap_at){assert(!wzh::IsOnboardingActive());stop_on_ap_wait=false;wzh::StopOnboarding();if(resume_after_stop){resume_after_stop=false;wzh::ResumeOnboardingAfterAbort();}}}
void vTaskDelete(void*){}
std::map<std::string,std::vector<uint8_t>> store;
std::string key(int h,const char* n){return std::to_string(h)+":"+n;}
int nvs_open(const char* n,int mode,int* h){*h=std::strcmp(n,"onboarding")==0?1:2;
 if(mode==NVS_READONLY){bool exists=false;for(auto& e:store)if(e.first.rfind(std::to_string(*h)+":",0)==0)exists=true;
 if(!exists)return ESP_ERR_NVS_NOT_FOUND;}return ESP_OK;}
int nvs_get_u8(int h,const char* n,uint8_t* v){auto it=store.find(key(h,n));if(it==store.end())return ESP_ERR_NVS_NOT_FOUND;*v=it->second[0];return ESP_OK;}
int nvs_set_u8(int h,const char* n,uint8_t v){store[key(h,n)]={v};return ESP_OK;}
int nvs_get_blob(int h,const char* n,void* v,size_t* len){auto it=store.find(key(h,n));if(it==store.end())return ESP_ERR_NVS_NOT_FOUND;assert(*len>=it->second.size());*len=it->second.size();std::memcpy(v,it->second.data(),*len);return ESP_OK;}
int nvs_set_blob(int h,const char* n,const void* v,size_t len){auto p=static_cast<const uint8_t*>(v);store[key(h,n)]={p,p+len};return ESP_OK;}
int nvs_commit(int){return ESP_OK;}void nvs_close(int){}
#include "wzh_onboarding_store.cc"
#include "wzh_onboarding.cc"
int main(){
 wzh::StartOnboarding();assert(worker&&wzh::IsOnboardingActive());worker(nullptr);
 auto& audio=Application::GetInstance().audio;assert(audio.played.size()==13);
 for(size_t i=1;i<audio.starts.size();++i){
  auto gap=audio.starts[i]-audio.starts[i-1];
  if(i<=8)assert(gap>=6300);else assert(gap>=2800&&gap<3000);
 }
 assert(!wzh::IsOnboardingVoiceMotionActive());
 assert(audio.starts[7]-audio.starts[6]>=15300); // PAGE waits for a real AP client.
 auto& demonstrations=Board::GetInstance().feedback;
 assert((demonstrations==std::vector<int>{1,2,3,5}));
 auto& times=Board::GetInstance().feedback_times;
 for(size_t i=0;i<demonstrations.size();++i)
  assert(times[i]<audio.starts[demonstrations[i]]+6300); // Real interaction is immediate.
 wzh::feedback_window.store(2);
 assert(wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kTouch));
 assert(!wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kMotion));
 wzh::feedback_window.store(0);assert(!wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kTouch));
 assert(!wzh::IsOnboardingActive());assert(store[key(2,"setup_guide")][0]==1);
 wzh::started.store(false);worker=nullptr;wzh::StartOnboarding();assert(worker);worker(nullptr);
 assert(audio.played.size()==13&&!wzh::IsOnboardingActive());
 // No sensor events: each experience still fires after the six-second window.
 store.clear();wifi_online=false;wzh::online_double_click.store(false);audio.played.clear();audio.starts.clear();demonstrations.clear();times.clear();
 clock_ms=0;audio.until=0;simulate_actions=false;feedback_hold_ms=1200;wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert((demonstrations==std::vector<int>{1,2,3,5}));
 for(size_t i=0;i<demonstrations.size();++i)
  {assert(times[i]>=audio.starts[demonstrations[i]]+6300);
   assert(audio.starts[demonstrations[i]+1]>=times[i]+1200);}
 // A touch arriving inside the real wait window gets one immediate response.
 store.clear();wifi_online=false;wzh::online_double_click.store(false);audio.played.clear();audio.starts.clear();demonstrations.clear();times.clear();
 clock_ms=0;audio.until=0;feedback_hold_ms=0;inject_touch_window=true;touch_captured=false;
 stop_on_motion=true;wzh::started.store(false);wzh::stopping.store(false);worker=nullptr;
 wzh::StartOnboarding();worker(nullptr);assert(touch_captured);
 assert((demonstrations==std::vector<int>{1}));
 assert(times[0]>=audio.starts[1]+300&&times[0]<audio.starts[1]+6300);
 simulate_actions=true;feedback_hold_ms=0;
 // A power-off mid-prompt keeps that step for the next boot.
 store.clear();wifi_online=false;wzh::online_double_click.store(false);audio.played.clear();wzh::started.store(false);wzh::stopping.store(false);
 stop_on_motion=true;worker=nullptr;wzh::StartOnboarding();assert(worker);worker(nullptr);
 assert(audio.played.size()==3&&audio.played.back()=="MOTION");
 wzh::OnboardingProgress interrupted;assert(wzh::LoadOnboardingProgress(interrupted));
 assert(interrupted.step==2&&!interrupted.Done());
 wzh::started.store(false);wzh::stopping.store(false);worker=nullptr;
 wzh::StartOnboarding();assert(worker);worker(nullptr);
 assert(audio.played.size()==14&&audio.played[3]=="MOTION");
 wzh::OnboardingProgress resumed;assert(wzh::LoadOnboardingProgress(resumed)&&resumed.Done());
 // Power-off while waiting for a phone keeps the page step for next boot.
 store.clear();wifi_online=false;wzh::online_double_click.store(false);audio.played.clear();clock_ms=0;audio.until=0;stop_on_ap_wait=true;stop_ap_at=0;
 wzh::started.store(false);wzh::stopping.store(false);wzh::service_verified.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(audio.played.size()==7&&audio.played.back()=="AP");
 wzh::OnboardingProgress ap_wait;assert(wzh::LoadOnboardingProgress(ap_wait)&&ap_wait.step==7);
 ap_ready_at=0;wzh::started.store(false);wzh::stopping.store(false);worker=nullptr;
 wzh::StartOnboarding();worker(nullptr);assert(audio.played.size()==13&&audio.played[7]=="PAGE");
 // Failed reset while a background worker is still unwinding restarts it once.
 store.clear();wifi_online=false;wzh::online_double_click.store(false);audio.played.clear();clock_ms=0;audio.until=0;stop_on_ap_wait=true;stop_ap_at=0;
 resume_after_stop=true;wzh::started.store(false);wzh::stopping.store(false);worker=nullptr;
 wzh::StartOnboarding();worker(nullptr);assert(wzh::worker_running.load()&&wzh::started.load());
 ap_ready_at=0;worker(nullptr);assert(audio.played.size()==13&&audio.played[7]=="PAGE");
 // A failed task creation must allow a later retry in the same boot.
 store.clear();wifi_online=false;wzh::online_double_click.store(false);audio.played.clear();clock_ms=0;audio.until=0;wzh::started.store(false);
 wzh::stopping.store(false);fail_create=true;worker=nullptr;wzh::StartOnboarding();
 assert(!wzh::started.load()&&!wzh::worker_running.load());fail_create=false;
 wzh::StartOnboarding();assert(worker);worker(nullptr);assert(audio.played.size()==13);
 // Wi-Fi alone does not claim that the conversation service is ready.
 store.clear();wifi_online=false;wzh::online_double_click.store(false);audio.played.clear();clock_ms=0;audio.until=0;send_service=false;stop_late_wait=true;
 wzh::started.store(false);wzh::stopping.store(false);wzh::service_verified.store(false);
 worker=nullptr;wzh::StartOnboarding();assert(worker);worker(nullptr);
 assert(audio.played.size()==8&&audio.played.back()=="PAGE");
 wzh::OnboardingProgress waiting;assert(wzh::LoadOnboardingProgress(waiting));
 assert(waiting.step==8&&waiting.flags==wzh::OnboardingProgress::kOfflineComplete);
 // A service that opened before a network loss does not validate a new connection.
 store.clear();wifi_online=false;wzh::online_double_click.store(false);audio.played.clear();clock_ms=0;audio.until=0;send_service=true;
 disconnect_after_service=true;stop_late_wait=true;wzh::started.store(false);wzh::stopping.store(false);
 wzh::service_verified.store(false);worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(audio.played.size()==8&&audio.played.back()=="PAGE");
 // Service ready after the initial timeout continues without a reboot.
 store.clear();wifi_online=false;wzh::online_double_click.store(false);audio.played.clear();clock_ms=0;audio.until=0;send_service=false;
 disconnect_after_service=false;late_service=true;wzh::started.store(false);wzh::stopping.store(false);
 wzh::service_verified.store(false);worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(audio.played.size()==13&&audio.played[8]=="CONNECTED");
 // Already connected: play all eight introductory steps, then wait for the
 // first double click and a successful service check before online speech.
 store.clear();wifi_online=true;online_test_click=true;online_saw_wait=false;
 wzh::online_double_click.store(false);audio.played.clear();audio.starts.clear();
 clock_ms=0;audio.until=0;wzh::started.store(false);wzh::stopping.store(false);
 wzh::service_verified.store(false);worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(online_saw_wait&&audio.played.size()==13&&audio.played[6]=="AP"&&audio.played[7]=="PAGE"&&audio.played[8]=="CONNECTED");
 assert(audio.starts[8]>=90000);
 // Interrupted online narration immediately releases voice motion and is
 // retained for the next tutorial session.
 store.clear();wifi_online=false;wzh::online_double_click.store(false);audio.played.clear();
 clock_ms=0;audio.until=0;send_service=true;stop_on_ability=true;
 wzh::started.store(false);wzh::stopping.store(false);wzh::service_verified.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(audio.played.back()=="ABILITY"&&!wzh::IsOnboardingVoiceMotionActive());
 wzh::OnboardingProgress online_interrupted;
 assert(wzh::LoadOnboardingProgress(online_interrupted)&&online_interrupted.step==9);
 // Clearing both tutorial and legacy records (factory reset) enables it again.
 store.clear();wifi_online=false;wzh::online_double_click.store(false);audio.played.clear();clock_ms=0;audio.until=0;send_service=true;disconnect_after_service=false;
 wzh::started.store(false);wzh::stopping.store(false);wzh::service_verified.store(false);
 worker=nullptr;wzh::StartOnboarding();assert(worker);worker(nullptr);
 assert(audio.played.size()==13&&audio.played.front()=="INTRO");
}
''')
            binary = root / 'test'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-DCONFIG_WZH_SYSTEM_PROMPTS=1', '-I', str(root), '-I', str(COMMON),
                            str(root / 'test.cc'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            (root / 'disabled.cc').write_text(r'''
#include <cassert>
#include "wzh_onboarding.cc"
int main(){wzh::StartOnboarding();assert(!wzh::IsOnboardingActive());wzh::StopOnboarding();}
''')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-DCONFIG_WZH_SYSTEM_PROMPTS=0', '-I', str(root), '-I', str(COMMON),
                            str(root / 'disabled.cc'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
