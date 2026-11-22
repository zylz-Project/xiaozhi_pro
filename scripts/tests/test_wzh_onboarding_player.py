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
extern uint32_t clock_ms,feedback_hold_ms;extern bool wifi_online,cellular_mode,cellular_online,network_probing;
struct Board{uint32_t feedback_until=0;std::vector<int> feedback;std::vector<uint32_t> feedback_times;static Board& GetInstance(){static Board b;return b;}
 bool UsesCellularNetwork()const{return cellular_mode;}
 bool IsNetworkProbing()const{return network_probing;}
 bool IsNetworkAvailable()const{return cellular_mode?cellular_online:wifi_online;}
 bool TriggerOnboardingFeedback(uint8_t step){feedback.push_back(step);feedback_times.push_back(clock_ms);feedback_until=clock_ms+feedback_hold_ms;return true;}
 bool IsOnboardingFeedbackPending()const{return clock_ms<feedback_until;}
 bool IsAudioMaintenanceActive(){return false;}bool IsLocalPlaybackPending(){return false;}};
''')
            names = ('INTRO TOUCH MOTION CMD1 NET_INVITE WAKE AP PAGE CONNECTED ABILITY '
                     'INTERRUPT CLONE FAREWELL ADV_WIFI_WAITING').split()
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
extern uint32_t clock_ms,conversation_start,conversation_end,ap_ready_at,stop_ap_at,cellular_connect_at;extern bool stop_on_motion,press_double_click,disconnect_after_click,stop_on_ap_wait,simulate_actions,wifi_online,stop_on_ability,connect_during_grace,connecting;
constexpr int kDeviceStateIdle=0,kDeviceStateConnecting=2;
struct Audio {
 uint32_t until=0,generation=1;std::vector<std::string_view> played;std::vector<uint32_t> starts;
 unsigned resets=0;
 bool IsPlaybackIdle(){return clock_ms>=until;}
 bool IsAudioProcessorRunning(){return false;}
 uint32_t GetPlaybackGeneration(){return generation;}
 void ResetDecoder(uint32_t expected){if(expected==generation){++generation;++resets;until=clock_ms;}}
 void PlaySound(std::string_view sound,uint32_t){
  const bool online=sound=="CONNECTED"||sound=="ABILITY"||sound=="INTERRUPT"||sound=="CLONE"||sound=="FAREWELL";
  if(wzh::IsOnboardingVoiceMotionActive()!=online)std::abort();
  if(!wzh::IsOnboardingActive())std::abort();
  played.push_back(sound);starts.push_back(clock_ms);until=clock_ms+300;
  if(sound=="INTRO" && !wzh::OnboardingBlocksConversation())std::abort();
  if(sound=="PAGE" && !wzh::OnboardingBlocksConversation())std::abort();
  if(sound=="ABILITY" && stop_on_ability){stop_on_ability=false;connecting=false;wzh::StopOnboarding();++generation;until=clock_ms;return;}
  if(sound=="MOTION" && stop_on_motion){stop_on_motion=false;wzh::StopOnboarding();++generation;until=clock_ms;return;}
  if(sound=="TOUCH" && simulate_actions)wzh::NotifyOnboarding(wzh::OnboardingEvent::kTouch);
  if(sound=="MOTION" && simulate_actions)wzh::NotifyOnboarding(wzh::OnboardingEvent::kMotion);
  if(sound=="CMD1" && simulate_actions)wzh::NotifyOnboarding(wzh::OnboardingEvent::kCommand);
  // The child answers the tutorial: says the wake phrase, then opens the page.
  if(sound=="WAKE" && simulate_actions)wzh::NotifyOnboarding(wzh::OnboardingEvent::kWake);
  if(sound=="WAKE" && connect_during_grace)cellular_connect_at=until+9000;
  if(sound=="AP"){ap_ready_at=stop_on_ap_wait ? UINT32_MAX : until+15000;stop_ap_at=until+7000;}
  if(sound=="PAGE"){wifi_online=true;wzh::NotifyPortalRequest();conversation_start=until+100;conversation_end=until+2100;}
 }
};
struct Application{Audio audio;static Application& GetInstance(){static Application a;return a;}
 int GetDeviceState(){if(connecting)return kDeviceStateConnecting;
  return clock_ms>=conversation_start && clock_ms<conversation_end ? 1 : kDeviceStateIdle;}
 Audio& GetAudioService(){return audio;}
 void Schedule(std::function<void()> resume){resume();}};
''')
            (root / 'test.cc').write_text(r'''
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "nvs.h"
#include "application.h"
uint32_t clock_ms=0,conversation_start=0,conversation_end=0,ap_ready_at=0,stop_ap_at=0,feedback_hold_ms=0,cellular_connect_at=0;bool stop_on_motion=false,press_double_click=true,disconnect_after_click=false,stop_on_ap_wait=false,simulate_actions=true,connect_during_grace=false;
bool wifi_online=false,cellular_mode=false,cellular_online=false,connecting=false,online_test_click=false,online_saw_wait=false,stop_on_ability=false,network_probing=false;
bool stop_late_wait=false,late_click=false,resume_after_stop=false,fail_create=false;
bool inject_touch_window=false,touch_captured=false;
void(*worker)(void*)=nullptr;
int xTaskCreate(void(*fn)(void*),const char*,int,void*,int,void*){if(fail_create)return 0;worker=fn;return 1;}
uint32_t xTaskGetTickCount(){return clock_ms;}
int finished_intros=0,idle_wait_checks=0;
uint32_t wifi_connect_at=0,cellular_select_at=0;
bool check_page_activation_wait=false;
void vTaskDelay(uint32_t n){
 clock_ms+=n;assert(clock_ms<400000);
 if(cellular_connect_at&&clock_ms>=cellular_connect_at){cellular_online=true;cellular_connect_at=0;}
 if(wifi_connect_at&&clock_ms>=wifi_connect_at){wifi_online=true;wifi_connect_at=0;}
 if(cellular_select_at&&clock_ms>=cellular_select_at){
  cellular_mode=true;cellular_online=true;network_probing=false;cellular_select_at=0;
 }
 if(check_page_activation_wait&&clock_ms>=2000&&clock_ms<5000)
  assert(!wzh::IsOnboardingActive());
 if(late_click&&clock_ms>=240000){late_click=false;press_double_click=true;}
 const bool ready=cellular_mode?cellular_online:wifi_online;
 auto& audio=Application::GetInstance().audio;
 if(!connecting&&wzh::IsOnboardingIntroPending()&&!wzh::OnboardingBlocksConversation()&&
    (audio.played.empty()||audio.played.back()!="ADV_WIFI_WAITING")){
  assert(wzh::IsOnboardingWorkerRunning());
  assert(!wzh::IsOnboardingActive()&&!wzh::IsOnboardingVoiceMotionActive());
  assert(!wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kTouch));
  if(clock_ms<180000)++idle_wait_checks;
 }
 if(!connecting&&ready&&((online_test_click&&clock_ms>=90000)||(!online_test_click&&press_double_click))){
  connecting=true;
  if(wzh::DeferOnboardingConversation([](bool played){
    auto& audio=Application::GetInstance().audio;
    if(!disconnect_after_click){
     assert(played&&audio.IsPlaybackIdle()&&audio.played.back()=="FAREWELL");
     ++finished_intros;
    }else assert(!played);
    connecting=false;
  })){
   online_test_click=false;online_saw_wait=true;
   if(disconnect_after_click){wifi_online=false;wzh::NotifyOnboarding(wzh::OnboardingEvent::kNetworkLost);}
  }else{
   connecting=false;
  }
 }
 if(inject_touch_window&&wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kTouch)){
  inject_touch_window=false;touch_captured=true;
 }
 if(clock_ms>=240000&&stop_late_wait){stop_late_wait=false;wzh::StopOnboarding();}
 if(stop_on_ap_wait&&stop_ap_at&&clock_ms>=stop_ap_at){
  assert(!wzh::IsOnboardingActive());stop_on_ap_wait=false;wzh::StopOnboarding();
  if(resume_after_stop){resume_after_stop=false;wzh::ResumeOnboardingAfterAbort();}
 }
}
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
 assert(wzh::OnboardingBlocksConversation()); // Includes the power-on gap before the worker starts.
 wzh::OnboardingProgress interrupted_intro;
 for(int i=0;i<4;++i)assert(wzh::SaveInterrupted(interrupted_intro));
 assert(interrupted_intro.step==0 && interrupted_intro.skipped==0);
 store.clear();
 wzh::StartOnboarding();assert(worker&&wzh::IsOnboardingActive());worker(nullptr);
 auto& audio=Application::GetInstance().audio;assert(audio.played.size()==13&&finished_intros==1);
 for(size_t i=1;i<audio.starts.size();++i){
  auto gap=audio.starts[i]-audio.starts[i-1];
  if(i<=8)assert(gap>=6300);else assert(gap>=300&&gap<=400);
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
 assert(!wzh::IsOnboardingActive()&&!wzh::IsOnboardingWorkerRunning());assert(store[key(2,"setup_guide")][0]==1);
 wzh::started.store(false);worker=nullptr;wzh::StartOnboarding();assert(worker);worker(nullptr);
 assert(audio.played.size()==13&&!wzh::IsOnboardingActive());
 // No sensor events: each experience still fires after the six-second window.
 store.clear();wifi_online=false;audio.played.clear();audio.starts.clear();demonstrations.clear();times.clear();
 clock_ms=0;audio.until=0;simulate_actions=false;feedback_hold_ms=1200;wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert((demonstrations==std::vector<int>{1,2,3,5}));
 for(size_t i=0;i<demonstrations.size();++i)
  {assert(times[i]>=audio.starts[demonstrations[i]]+6300);
   assert(audio.starts[demonstrations[i]+1]>=times[i]+1200);}
 // A touch arriving inside the real wait window gets one immediate response.
 store.clear();wifi_online=false;audio.played.clear();audio.starts.clear();demonstrations.clear();times.clear();
 clock_ms=0;audio.until=0;feedback_hold_ms=0;inject_touch_window=true;touch_captured=false;
 stop_on_motion=true;wzh::started.store(false);wzh::stopping.store(false);worker=nullptr;
 wzh::StartOnboarding();worker(nullptr);assert(touch_captured);
 assert((demonstrations==std::vector<int>{1}));
 assert(times[0]>=audio.starts[1]+300&&times[0]<audio.starts[1]+6300);
 simulate_actions=true;feedback_hold_ms=0;
 // A power-off mid-prompt keeps that step for the next boot.
 store.clear();wifi_online=false;audio.played.clear();wzh::started.store(false);wzh::stopping.store(false);
 stop_on_motion=true;worker=nullptr;wzh::StartOnboarding();assert(worker);worker(nullptr);
 assert(audio.played.size()==3&&audio.played.back()=="MOTION");
 wzh::OnboardingProgress interrupted;assert(wzh::LoadOnboardingProgress(interrupted));
 assert(interrupted.step==2&&!interrupted.Done());
 wzh::started.store(false);wzh::stopping.store(false);worker=nullptr;
 wzh::StartOnboarding();assert(worker);worker(nullptr);
 assert(audio.played.size()==14&&audio.played[3]=="MOTION");
 wzh::OnboardingProgress resumed;assert(wzh::LoadOnboardingProgress(resumed)&&resumed.Done());
 // Power-off while waiting for a phone keeps the page step for next boot.
 store.clear();wifi_online=false;audio.played.clear();clock_ms=0;audio.until=0;stop_on_ap_wait=true;stop_ap_at=0;
 wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(audio.played.size()==7&&audio.played.back()=="AP");
 wzh::OnboardingProgress ap_wait;assert(wzh::LoadOnboardingProgress(ap_wait)&&ap_wait.step==7);
 ap_ready_at=0;wzh::started.store(false);wzh::stopping.store(false);worker=nullptr;
 wzh::StartOnboarding();worker(nullptr);assert(audio.played.size()==13&&audio.played[7]=="PAGE");
 // Failed reset while a background worker is still unwinding restarts it once.
 store.clear();wifi_online=false;audio.played.clear();clock_ms=0;audio.until=0;stop_on_ap_wait=true;stop_ap_at=0;
 resume_after_stop=true;wzh::started.store(false);wzh::stopping.store(false);worker=nullptr;
 wzh::StartOnboarding();worker(nullptr);assert(wzh::worker_running.load()&&wzh::started.load());
 ap_ready_at=0;worker(nullptr);assert(audio.played.size()==13&&audio.played[7]=="PAGE");
 // A failed task creation must allow a later retry in the same boot.
 store.clear();wifi_online=false;audio.played.clear();clock_ms=0;audio.until=0;wzh::started.store(false);
 wzh::stopping.store(false);fail_create=true;worker=nullptr;wzh::StartOnboarding();
 assert(!wzh::started.load()&&!wzh::worker_running.load());fail_create=false;
 wzh::StartOnboarding();assert(worker);worker(nullptr);assert(audio.played.size()==13);
 // Wi-Fi alone does not start the introduction or suppress idle behavior.
 store.clear();wifi_online=false;audio.played.clear();clock_ms=0;audio.until=0;press_double_click=false;stop_late_wait=true;
 idle_wait_checks=0;
 wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();assert(worker);worker(nullptr);
 assert(audio.played.size()==8&&audio.played.back()=="PAGE"&&idle_wait_checks>0);
 wzh::OnboardingProgress waiting;assert(wzh::LoadOnboardingProgress(waiting));
 assert(waiting.step==8&&waiting.flags==wzh::OnboardingProgress::kOfflineComplete);
 // Direct Wi-Fi can resume page narration without an AP client. If the app
 // is still activating, let the page wait until it can play.
 waiting.step=7;waiting.completed&=~(1U<<7);assert(wzh::SaveOnboardingProgress(waiting));
 wifi_online=false;wifi_connect_at=2000;ap_ready_at=UINT32_MAX;
 audio.played.clear();audio.starts.clear();clock_ms=0;audio.until=0;press_double_click=true;
 check_page_activation_wait=true;conversation_start=2000;conversation_end=5000;
 Board::GetInstance().feedback_until=0;
 wzh::started.store(false);wzh::stopping.store(false);worker=nullptr;
 wzh::StartOnboarding();assert(worker);worker(nullptr);
 check_page_activation_wait=false;assert(audio.starts.front()>=5000);
 assert(audio.played.size()==6&&audio.played.front()=="PAGE"&&audio.played.back()=="FAREWELL");
 // A page step saved before selecting 4G may be waiting for a phone,
 // waiting on audio, or already speaking when the transport changes.
 for(unsigned phase=0;phase<3;++phase){
  assert(wzh::SaveOnboardingProgress(waiting));
  wifi_online=false;cellular_mode=false;cellular_online=false;network_probing=phase==1;
  ap_ready_at=phase==0?UINT32_MAX:0;
  cellular_select_at=phase==2?100:500;
  audio.played.clear();audio.starts.clear();clock_ms=0;audio.until=0;audio.resets=0;
  conversation_start=conversation_end=0;Board::GetInstance().feedback_until=0;
  wzh::started.store(false);wzh::stopping.store(false);
  worker=nullptr;wzh::StartOnboarding();assert(worker);worker(nullptr);
  assert(audio.played.size()==(phase==2?6:5)&&audio.played.back()=="FAREWELL");
  assert(audio.played.front()==(phase==2?"PAGE":"CONNECTED"));
  assert(audio.resets==(phase==2?1U:0U));
  wzh::OnboardingProgress result;
  assert(wzh::LoadOnboardingProgress(result)&&result.Done());
 }
 cellular_mode=false;cellular_online=false;
 // A network loss cancels a pending introduction.
 store.clear();wifi_online=false;audio.played.clear();clock_ms=0;audio.until=0;press_double_click=true;
 disconnect_after_click=true;stop_late_wait=true;wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(audio.played.size()==8&&audio.played.back()=="PAGE");
 // A late double click continues without a reboot.
 store.clear();wifi_online=false;audio.played.clear();clock_ms=0;audio.until=0;press_double_click=false;
 disconnect_after_click=false;late_click=true;wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(audio.played.size()==13&&audio.played[8]=="CONNECTED");
 // Already connected: play all eight introductory steps, then wait for the
 // first double click before online speech.
 store.clear();wifi_online=true;online_test_click=true;online_saw_wait=false;
 audio.played.clear();audio.starts.clear();
 clock_ms=0;audio.until=0;wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(online_saw_wait&&audio.played.size()==13&&audio.played[6]=="AP"&&audio.played[7]=="PAGE"&&audio.played[8]=="CONNECTED");
 assert(audio.starts[8]>=90000);
 // Interrupted online narration immediately releases voice motion and is
 // retained for the next tutorial session.
 store.clear();wifi_online=false;audio.played.clear();
 clock_ms=0;audio.until=0;press_double_click=true;stop_on_ability=true;
 wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(audio.played.back()=="ABILITY"&&!wzh::IsOnboardingVoiceMotionActive());
 wzh::OnboardingProgress online_interrupted;
 assert(wzh::LoadOnboardingProgress(online_interrupted)&&online_interrupted.step==9);
 // Clearing both tutorial and legacy records (factory reset) enables it again.
 store.clear();wifi_online=false;audio.played.clear();clock_ms=0;audio.until=0;press_double_click=true;disconnect_after_click=false;
 wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();assert(worker);worker(nullptr);
 assert(audio.played.size()==13&&audio.played.front()=="INTRO");
 // Cellular skips Wi-Fi setup and waits for a double click.
 store.clear();wifi_online=false;cellular_mode=true;cellular_online=true;
 online_test_click=true;online_saw_wait=false;
 audio.played.clear();audio.starts.clear();clock_ms=0;audio.until=0;
 wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(online_saw_wait);
 assert((audio.played==std::vector<std::string_view>{"INTRO","TOUCH","MOTION","CMD1","WAKE",
   "CONNECTED","ABILITY","INTERRUPT","CLONE","FAREWELL"}));
 wzh::OnboardingProgress cellular_done;
 assert(wzh::LoadOnboardingProgress(cellular_done)&&cellular_done.Done());
 assert(!wzh::OnboardingBlocksConversation());
 // Offline 4G gives one neutral prompt, then waits without Wi-Fi setup.
 store.clear();cellular_online=false;online_test_click=false;stop_late_wait=true;
 audio.played.clear();audio.starts.clear();clock_ms=0;audio.until=0;
 wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert((audio.played==std::vector<std::string_view>{"INTRO","TOUCH","MOTION","CMD1","WAKE","ADV_WIFI_WAITING"}));
 wzh::OnboardingProgress waiting_cellular;
 assert(wzh::LoadOnboardingProgress(waiting_cellular)&&waiting_cellular.step==8&&!waiting_cellular.Done());
 // If local sound owns the lane when the optional cue is due, registering
 // and double-clicking can still start the remaining tutorial.
 audio.played.clear();clock_ms=0;audio.until=20000;cellular_connect_at=17000;
 press_double_click=true;online_test_click=false;online_saw_wait=false;
 Board::GetInstance().feedback_until=0;
 wzh::started.store(false);wzh::stopping.store(false);worker=nullptr;
 wzh::StartOnboarding();assert(worker);worker(nullptr);
 assert(online_saw_wait&&audio.played.size()==5&&audio.played.front()=="CONNECTED");
 assert(wzh::SaveOnboardingProgress(waiting_cellular));
 // Reboot with network restored and finish only the remaining online chapter.
 cellular_online=true;online_test_click=true;online_saw_wait=false;audio.played.clear();
 clock_ms=0;audio.until=0;wzh::started.store(false);wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 assert(online_saw_wait&&audio.played.front()=="CONNECTED");
 assert(wzh::LoadOnboardingProgress(waiting_cellular)&&waiting_cellular.Done());
 // Normal registration during the grace period must not play the offline cue.
 store.clear();cellular_online=false;connect_during_grace=true;online_test_click=true;
 audio.played.clear();clock_ms=0;audio.until=0;wzh::started.store(false);
 wzh::stopping.store(false);
 worker=nullptr;wzh::StartOnboarding();worker(nullptr);
 for(auto sound:audio.played)assert(sound!="ADV_WIFI_WAITING");
 assert(wzh::LoadOnboardingProgress(waiting_cellular)&&waiting_cellular.Done());
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
