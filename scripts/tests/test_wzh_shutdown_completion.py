"""Exercise production readiness and shutdown with controlled network and audio state."""
from pathlib import Path
import unittest

from test_wzh_action_queue import queue_function
import test_wzh_system_prompts as prompt_tests

ROOT = Path(__file__).resolve().parents[2]
STUBS = {name: '' for name in ['application.h', 'board.h', 'assets/lang_config.h',
                              'esp_log.h', 'freertos/FreeRTOS.h', 'freertos/task.h']}
HARNESS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#define CONFIG_WZH_SYSTEM_PROMPTS PROMPT_FEATURE
#define CONFIG_BOARD_TYPE_WZH_TAILREDPANDA 1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
constexpr int pdPASS=1,kDeviceStateIdle=0,kDeviceStateActivating=1;
enum class PowerSaveLevel { LOW_POWER };
struct Ota {
 bool HasServerTime(){return true;}
 std::string GetCurrentVersion(){return "1.0";}
};
struct Display {
 std::string notification;
 void ShowNotification(const char* message){notification=message;}
 void SetChatMessage(const char*,const char*){}
};
namespace SystemInfo{void PrintHeapStats(){}}
uint32_t clock_ms=10000,app_ready_ms=0;
bool inside_worker=false,drive_worker=true,drive_app=true,cleanup_done=false;
std::function<void()> advance_hook;
struct YieldWorker{};
void (*entry)(void*)=nullptr;
uint32_t esp_log_timestamp(){return clock_ms;}
int xTaskCreate(void(*f)(void*),const char*,int,void*,int,void*){entry=f;return pdPASS;}
void vTaskDelay(unsigned);
struct Audio {
 enum class PlaybackSource { kRegular,kShutdown };
 uint32_t generation=1,until=0,drained=0,last_start=0;
 unsigned off=0,ready=0,resets=0;
 bool stalled=false,cancel_every_play=false,wake=true,voice=true,shutdown=false;
 void settle(){if(!stalled&&until&&clock_ms>=until){until=0;++drained;}}
 uint32_t GetPlaybackGeneration(){return generation;}
 uint32_t GetPlaybackDrainedCount(){settle();return drained;}
 bool IsPlaybackIdle(){settle();return until==0;}
 bool IsAudioProcessorRunning(){return voice;}
 void EnableWakeWordDetection(bool value){wake=value;}
 void EnableVoiceProcessing(bool value){voice=value;}
 // A reset can also advance a global drain notification; it is not completion.
 void ResetDecoder(std::optional<uint32_t> expected={},PlaybackSource source=PlaybackSource::kRegular){
  if((expected&&*expected!=generation)||(shutdown&&source==PlaybackSource::kRegular))return;
  shutdown=source==PlaybackSource::kShutdown;++generation;++drained;++resets;until=0;
 }
 void PlaySound(std::string_view sound,std::optional<uint32_t> expected={},PlaybackSource source=PlaybackSource::kRegular){
  if((expected&&*expected!=generation)||shutdown!=(source==PlaybackSource::kShutdown))return;
  if(sound=="ready"){++ready;return;}
  assert(cleanup_done&&sound=="off"&&!wake&&!voice);
  ++off;last_start=clock_ms;until=clock_ms+3100;
  if(cancel_every_play)ResetDecoder({},PlaybackSource::kShutdown);
 }
};
using AudioService=Audio;
struct Protocol{bool ready=true;bool IsReady()const{return ready;}};
struct Application {
 Audio audio;std::deque<std::function<void()>> pending;
 Audio& audio_service_=audio;
 std::unique_ptr<Ota> ota_;
 std::shared_ptr<Protocol> protocol_=std::make_shared<Protocol>();
 std::atomic<uint32_t> protocol_generation_{0};
 std::atomic<bool> activation_protocol_ready_{false};
 bool activation_running_=false;
 bool activation_network_ready_pending_=false;
 int deferred_network_events=0;
 int state=kDeviceStateIdle;
 bool has_server_time_=false;
 static Application& GetInstance(){static Application app;return app;}
 Audio& GetAudioService(){return audio;}int GetDeviceState(){return state;}
 void SetDeviceState(int value){state=value;}
 void Schedule(std::function<void()> f){pending.push_back(std::move(f));}
 bool IsShuttingDown() const;
 void HandleActivationDoneEvent();
 bool IsConversationRecovering()const{return false;}
 void HandleNetworkConnectedEvent(){assert(!ota_&&state==kDeviceStateIdle);++deferred_network_events;}
};
struct Board {
 bool network_available=false;
 static Board& GetInstance(){static Board b;return b;}
 Display* GetDisplay(){static Display display;return &display;}
 void SetPowerSaveLevel(PowerSaveLevel){}
 bool UsesCellularNetwork(){return false;}
 bool IsAudioMaintenanceActive(){return true;}
 bool IsNetworkAvailable(){return network_available;}
 bool IsLocalPlaybackPending(){return false;}
};
namespace Lang {
namespace Strings{constexpr auto VERSION="Version ",SERVER_NOT_CONNECTED="Server unavailable";}
namespace Sounds{constexpr std::string_view OGG_POWER_ON="on",OGG_POWER_OFF="off",OGG_LOW_BATTERY="low",OGG_WIFICONFIG="wifi",OGG_EXCLAMATION="warning",OGG_SUCCESS="ready";}
}
#include "wzh_onboarding_store.h"
namespace wzh {bool PeekOnboardingProgress(OnboardingProgress&){return true;} bool IsOnboardingActive(){return false;}bool OnboardingBlocksConversation(){return false;}void StartOnboarding(){}void StopOnboarding(){}
void StopIdlePower(){}void StartIdlePower(std::function<void()>){}}
#include "PRODUCTION_SOURCE"
void run_application(){
 if(!drive_app||clock_ms<app_ready_ms)return;
 auto& pending=Application::GetInstance().pending;
 while(!pending.empty()){auto f=std::move(pending.front());pending.pop_front();f();}
}
void run_worker(){
 if(!entry)return;
 inside_worker=true;try{entry(nullptr);}catch(YieldWorker&){}inside_worker=false;
}
void vTaskDelay(unsigned ms){
 if(inside_worker)throw YieldWorker{};
 clock_ms+=ms;run_application();if(advance_hook)advance_hook();
 if(drive_worker)run_worker();
}
void begin(){
 wzh::StartSystemPrompts();wzh::BeginSystemPromptShutdown();
 Application::GetInstance().Schedule([](){
  Application::GetInstance().audio.ResetDecoder();cleanup_done=true;
 });
}
'''.replace('PRODUCTION_SOURCE', str(ROOT/'main/boards/common/wzh/wzh_system_prompts.cc'))


class WzhShutdownCompletionTest(unittest.TestCase):
    def test_activation_announcement_replaces_local_generation_and_keeps_digits_together(self):
        source=(ROOT/'main/application.cc').read_text()
        show_code=queue_function(source, 'void Application::ShowActivationCode(')
        self.compile_run(r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "wzh_prompt_audio_gate.h"
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 1
#define ESP_LOGI(...) ((void)0)
constexpr int kDeviceStateActivating=1;
namespace Lang {
namespace Strings{constexpr auto ACTIVATION="activate";}
namespace Sounds {
constexpr std::string_view OGG_ACTIVATION="intro",OGG_0="0",OGG_1="1",OGG_2="2",
    OGG_3="3",OGG_4="4",OGG_5="5",OGG_6="6",OGG_7="7",OGG_8="8",OGG_9="9";
}
}
struct Board {
    static Board& GetInstance(){static Board board;return board;}
    bool pending=false;
    bool IsLocalPlaybackPending(){return pending;}
};
struct Audio {
    unsigned generation=1,resets=0;
    bool idle=true,interrupt=false;
    std::vector<std::string_view> spoken;
    unsigned GetPlaybackGeneration(){return generation;}
    bool IsPlaybackIdle(){return idle;}
    void ResetDecoder(){++resets;++generation;idle=true;}
    void PlaySound(std::string_view sound,unsigned expected){
        assert(expected==generation);
        bool producer_blocked=false;
        std::thread producer([&] {
            std::unique_lock gate(wzh::PromptAudioGate(),std::try_to_lock);
            producer_blocked=!gate.owns_lock();
        });
        producer.join();assert(producer_blocked);
        spoken.push_back(sound);
        if(interrupt)++generation;
    }
};
struct Application {
    Audio audio_service_;
    bool shutdown=false;
    int state=kDeviceStateActivating;
    bool IsShuttingDown(){return shutdown;}
    int GetDeviceState(){return state;}
    void Alert(const char*,const char*,const char*,std::string_view sound){assert(sound.empty());}
    void ShowActivationCode(const std::string&,const std::string&);
};
'''+show_code+r'''
int main(){
    Application app;
    auto& board=Board::GetInstance();
    board.pending=true;
    app.ShowActivationCode("20x4","activation");
    assert(app.audio_service_.resets==1);
    assert((app.audio_service_.spoken==std::vector<std::string_view>{"intro","2","0","4"}));
    board.pending=false;app.audio_service_.idle=false;app.audio_service_.spoken.clear();
    app.audio_service_.interrupt=true;
    app.ShowActivationCode("20","activation");
    assert(app.audio_service_.resets==2&&app.audio_service_.spoken.size()==1);
    app.shutdown=true;
    app.ShowActivationCode("20","activation");
    assert(app.audio_service_.resets==2&&app.audio_service_.spoken.size()==1);
}
''')

    compile_run = prompt_tests.PromptWorkerTest.compile_run

    def test_activation_ready_prompt_requires_protocol_and_network_without_shutdown(self):
        source = (ROOT / 'main/application.cc').read_text()
        shutdown_active = queue_function(source, 'bool Application::IsShuttingDown() const')
        activation_done = queue_function(source, 'void Application::HandleActivationDoneEvent()')
        self.compile_run(HARNESS.replace('PROMPT_FEATURE', '1') + shutdown_active + activation_done + r'''
int main(){
 auto& app=Application::GetInstance();auto& audio=app.audio;
 auto& board=Board::GetInstance();
 const auto finish_activation=[&](){
  app.state=kDeviceStateActivating;app.ota_=std::make_unique<Ota>();
  app.HandleActivationDoneEvent();run_application();
  assert(app.state==kDeviceStateIdle&&!app.ota_&&app.has_server_time_);
 };

 board.network_available=true;
 finish_activation();assert(audio.ready==0);
 assert(board.GetDisplay()->notification==Lang::Strings::SERVER_NOT_CONNECTED);
 assert(app.deferred_network_events==0);
 app.activation_network_ready_pending_=true;
 finish_activation();assert(audio.ready==0&&app.deferred_network_events==1);
 assert(!app.activation_network_ready_pending_);
 finish_activation();assert(app.deferred_network_events==1);

 app.activation_protocol_ready_=true;board.network_available=false;
 finish_activation();assert(audio.ready==0);

 app.activation_protocol_ready_=true;board.network_available=true;
 finish_activation();assert(audio.ready==1);
 assert(board.GetDisplay()->notification=="Version 1.0");

 // The link can drop after successful activation but before the prompt callback.
 app.activation_protocol_ready_=true;
 app.ota_=std::make_unique<Ota>();app.HandleActivationDoneEvent();
 assert(!app.pending.empty());
 board.network_available=false;run_application();assert(audio.ready==1);
 board.network_available=true;

 // MQTT can fall out of readiness while the modem link is still up.
 app.ota_=std::make_unique<Ota>();app.HandleActivationDoneEvent();
 assert(!app.pending.empty());
 app.protocol_->ready=false;run_application();assert(audio.ready==1);
 finish_activation();assert(audio.ready==1);
 app.protocol_->ready=true;

 // Activation can queue the ready prompt just before the power task starts shutdown.
 app.activation_protocol_ready_=true;
 app.ota_=std::make_unique<Ota>();app.HandleActivationDoneEvent();
 assert(!app.pending.empty());
 begin();run_application();assert(audio.ready==1);

 // Activation can also finish while the goodbye is playing or its codec tail is draining.
 bool late_completion=false;
 advance_hook=[&](){
  if(late_completion||audio.off==0||clock_ms<audio.last_start+3110)return;
  late_completion=true;app.state=kDeviceStateActivating;
  app.ota_=std::make_unique<Ota>();app.HandleActivationDoneEvent();run_application();
  assert(!app.ota_&&app.state==kDeviceStateActivating);
 };
 assert(wzh::PlayShutdownPromptAndWait([](){}));
 assert(late_completion&&audio.off==1&&audio.ready==1);
}
''', STUBS)

    def test_delayed_cleanup_cancellation_and_codec_tail_wait_for_complete_sentence(self):
        self.compile_run(HARNESS.replace('PROMPT_FEATURE', '1') + r'''
int main(){
 auto& audio=Application::GetInstance().audio;
 app_ready_ms=clock_ms+2800;begin();const auto start=clock_ms;unsigned checks=0;
 assert(wzh::PlayShutdownPromptAndWait([&](){++checks;}));
 assert(audio.off==1&&clock_ms>=audio.last_start+3180&&clock_ms-start>=5980&&checks>=5);
 // A mid-sentence reset empties the queue and advances its drain counter.
 // The entire replacement sentence must finish before power may be released.
 unsigned off_before=audio.off;const auto interrupt_at=clock_ms+1500;bool cancelled=false;
 advance_hook=[&](){if(!cancelled&&clock_ms>=interrupt_at&&audio.until){
  cancelled=true;audio.ResetDecoder({},Audio::PlaybackSource::kShutdown);}};
 assert(wzh::PlayShutdownPromptAndWait([&](){++checks;}));
 assert(cancelled&&audio.off==off_before+2&&clock_ms>=audio.last_start+3180);
 // Also re-check after the codec-tail delay, where a late cancellation may land.
 off_before=audio.off;cancelled=false;
 advance_hook=[&](){if(!cancelled&&audio.off>off_before&&clock_ms>=audio.last_start+3130){
  cancelled=true;audio.ResetDecoder({},Audio::PlaybackSource::kShutdown);}};
 assert(wzh::PlayShutdownPromptAndWait([&](){++checks;}));
 assert(cancelled&&audio.off==off_before+2&&clock_ms>=audio.last_start+3180);
}
''', STUBS)

    def test_stalls_time_out_without_late_playback_and_battery_checks_remain_live(self):
        self.compile_run(HARNESS.replace('PROMPT_FEATURE', '1') + r'''
struct CriticalPowerCut{};
int main(){
 auto& app=Application::GetInstance();auto& audio=app.audio;begin();
 drive_app=false;auto start=clock_ms;unsigned checks=0;
 assert(!wzh::PlayShutdownPromptAndWait([&](){++checks;}));
 assert(clock_ms-start==8000&&checks>=7&&audio.off==0);
 drive_app=true;run_application();run_worker();assert(audio.off==0);
 drive_worker=false;start=clock_ms;
 assert(!wzh::PlayShutdownPromptAndWait([&](){++checks;}));
 assert(clock_ms-start==8000&&audio.off==0);
 drive_worker=true;run_worker();assert(audio.off==0);
 audio.stalled=true;start=clock_ms;
 assert(!wzh::PlayShutdownPromptAndWait([&](){++checks;}));
 assert(clock_ms-start==8000&&audio.off==1);
 audio.stalled=false;audio.cancel_every_play=true;auto off=audio.off;start=clock_ms;
 assert(!wzh::PlayShutdownPromptAndWait([&](){++checks;}));
 assert(audio.off==off+2&&clock_ms-start<8000);
 audio.cancel_every_play=false;start=clock_ms;bool cut=false;
 try{wzh::PlayShutdownPromptAndWait([](){throw CriticalPowerCut{};});}
 catch(CriticalPowerCut&){cut=true;}
 assert(cut&&clock_ms-start==1000); // Critical cutoff never waits for speech.
}
''', STUBS)

    def test_legacy_prompt_pack_also_uses_complete_playback_instead_of_700_ms(self):
        self.compile_run(HARNESS.replace('PROMPT_FEATURE', '0') + r'''
int main(){
 begin();auto& audio=Application::GetInstance().audio;const auto start=clock_ms;
 assert(wzh::PlayShutdownPromptAndWait([](){}));
 assert(audio.off==1&&clock_ms-start>=3180);
}
''', STUBS)

    def test_all_product_shutdown_flags_gate_real_board_input_admission(self):
        for board, stem in [('wzh_tailRedPanda', 'panda'), ('wzh_crawlPanda', 'panda'),
                            ('wzh_dinosaur', 'dinosaur')]:
            with self.subTest(board=board):
                folder = ROOT/'main/boards'/board
                getter = queue_function((folder/(stem+'_power.h')).read_text(),
                                        'bool IsShuttingDown() const')
                admission = queue_function((folder/(board+'.cc')).read_text(),
                                           'bool IsAudioMaintenanceActive() override')
                program = r'''
#include <atomic>
#include <cassert>
struct Power{std::atomic<bool> shutting_down_{false};GETTER};
struct Web{bool active=false;bool maintenance_active(){return active;}};
struct Store{bool writing=false;bool IsWriting(){return writing;}};
struct Audio{Store store;Store& audio_store(){return store;}};
namespace wzh{bool voice_busy=false;bool VoiceManagementBusy(){return voice_busy;}}
struct Base{virtual bool IsAudioMaintenanceActive(){return false;}};
struct Board:Base{Power power_;Web web_console_;Audio local_audio_;ADMISSION};
int main(){Board b;assert(!b.IsAudioMaintenanceActive());
 b.power_.shutting_down_=true;assert(b.IsAudioMaintenanceActive());
 b.power_.shutting_down_=false;b.web_console_.active=true;assert(b.IsAudioMaintenanceActive());
 b.web_console_.active=false;b.local_audio_.store.writing=true;assert(b.IsAudioMaintenanceActive());
 b.local_audio_.store.writing=false;wzh::voice_busy=true;assert(b.IsAudioMaintenanceActive());}
'''.replace('GETTER', getter).replace('ADMISSION', admission)
                self.compile_run(program)

    def test_real_power_sequences_check_battery_while_parking_and_cut_after_speech(self):
        for board, stem, cls in [('wzh_tailRedPanda', 'panda', 'PandaPower'),
                                 ('wzh_crawlPanda', 'panda', 'PandaPower'),
                                 ('wzh_dinosaur', 'dinosaur', 'DinosaurPower')]:
            with self.subTest(board=board):
                source = (ROOT/'main/boards'/board/(stem+'_power.cc')).read_text()
                shutdown = queue_function(source, 'void '+cls+'::Shutdown()')
                program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
constexpr unsigned SERVO_POWER_GPIO=4,POWER_CTRL_GPIO=7;
uint32_t clock_ms=0,critical_at=0;unsigned battery_checks=0,speech_calls=0;
bool servo_off=false;struct PowerCut{};
uint32_t esp_log_timestamp(){return clock_ms;}
void vTaskDelay(unsigned ms){clock_ms+=ms;}
void gpio_set_level(unsigned pin,int level){assert(level==0);
 if(pin==SERVO_POWER_GPIO)servo_off=true;else if(pin==POWER_CTRL_GPIO)throw PowerCut{};}
struct Output{void Watchdog(){}void SetFault(int,bool){}};
namespace wzh{
 constexpr int kShutdown=1;void BeginSystemPromptShutdown(){}
 bool PlayShutdownPromptAndWait(const std::function<void()>&check){
  assert(servo_off&&battery_checks==2&&clock_ms==2400);++speech_calls;
  for(unsigned i=0;i<3;++i){vTaskDelay(1000);check();}vTaskDelay(100);return true;
 }
}
struct Application{static Application& GetInstance(){static Application a;return a;}
 void StopInteraction(){}};
struct POWER_CLASS{
 std::atomic<bool> shutting_down_{false},shutdown_hook_ready_{false};
 std::atomic<Output*> servo_output_{nullptr};std::function<void()>shutdown_hook_;
 void Shutdown();void ReadBattery(uint32_t now){++battery_checks;
  if(critical_at&&now>=critical_at){gpio_set_level(SERVO_POWER_GPIO,0);gpio_set_level(POWER_CTRL_GPIO,0);}}
};
SHUTDOWN_METHOD
int main(){
 POWER_CLASS normal;try{normal.Shutdown();}catch(PowerCut&){}
 assert(normal.shutting_down_&&clock_ms==5500&&speech_calls==1&&battery_checks==5);
 clock_ms=0;critical_at=1000;battery_checks=speech_calls=0;servo_off=false;
 POWER_CLASS critical;try{critical.Shutdown();}catch(PowerCut&){}
 assert(clock_ms==1000&&speech_calls==0&&servo_off);
}
'''.replace('POWER_CLASS', cls).replace('SHUTDOWN_METHOD', shutdown)
                self.compile_run(program)


if __name__ == '__main__':
    unittest.main()
