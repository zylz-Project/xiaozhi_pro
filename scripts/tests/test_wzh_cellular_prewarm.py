"""Exercise dinosaur preconnect scheduling without a modem."""
from pathlib import Path
import unittest
from test_wzh_action_queue import queue_function
import test_wzh_motion_output as output

ROOT = Path(__file__).resolve().parents[2]

class CellularPrewarmTest(unittest.TestCase):
    def test_prewarmed_channel_gets_cellular_buffer_before_early_return(self):
        source = (ROOT / 'main/application.cc').read_text()
        start = source.index('            if (protocol_generation !=', source.index('protocol_->OnAudioChannelOpened('))
        end = source.index('            board.SetPowerSaveLevel', start)
        fragment = source[start:end]
        header = (ROOT / 'main/audio/audio_service.h').read_text()
        constants = '\n'.join(line for line in header.splitlines() if 'static constexpr uint32_t k' in line and 'Playout' in line)
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 1
struct AudioService{
''' + constants + r'''
 uint32_t target=0,wait=0,recovery=0;
 void SetDownlinkJitter(uint32_t t,uint32_t w,uint32_t r=0){target=t;wait=w;recovery=r?r:t;}
};
struct Board{bool cellular=true;int opened=0;
 bool UsesCellularNetwork(){return cellular;}bool IsNetworkAvailable(){return true;}
 void OnConversationChannelOpened(){++opened;}};
struct Application{std::atomic<uint32_t>protocol_generation_{1};
 std::atomic<bool>cellular_server_connected_{false},conversation_transport_disconnected_{true},cellular_prewarming_{true};
 AudioService audio_service_;
 void Opened(Board& board,uint32_t protocol_generation){
''' + fragment + r'''
 }
};
int main(){Application a;Board b;
 a.Opened(b,0);assert(a.audio_service_.target==0);
 a.Opened(b,1);assert(a.audio_service_.target==360&&a.audio_service_.wait==480&&
                      a.audio_service_.recovery==480&&b.opened==0);
 a.cellular_prewarming_=false;b.cellular=false;a.Opened(b,1);
 assert(a.audio_service_.target==120&&a.audio_service_.wait==80&&
        a.audio_service_.recovery==120&&b.opened==1);
}
''')

    def test_close_retires_prewarm_only_for_current_cellular_session(self):
        source = (ROOT / 'main/application.cc').read_text()
        start = source.index('            // A delayed close from the previous session')
        end = source.index('            if (!interaction_stopped_.load()', start)
        fragment = source[start:end]
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 1
struct Board {bool cellular=true;bool UsesCellularNetwork()const{return cellular;}};
struct Protocol {bool open=false;bool IsAudioChannelOpened()const{return open;}};
struct Application {std::atomic<uint32_t> interaction_generation_{1};Protocol p,*protocol_=&p;
 bool cellular_prewarm_retired_=false;
 void Closed(Board& board,uint32_t generation){
''' + fragment + r'''
 }};
int main(){Application a;Board b;
 a.Closed(b,0);assert(!a.cellular_prewarm_retired_);
 a.p.open=true;a.Closed(b,1);assert(!a.cellular_prewarm_retired_);
 a.p.open=false;b.cellular=false;a.Closed(b,1);assert(!a.cellular_prewarm_retired_);
 b.cellular=true;a.Closed(b,1);assert(a.cellular_prewarm_retired_);}
''')

    def test_failed_user_connection_defers_background_retry(self):
        source = (ROOT / 'main/application.cc').read_text()
        method = queue_function(source, 'void Application::ContinueOpenAudioChannel(')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 1
enum {kDeviceStateConnecting=1,kDeviceStateIdle=2};using ListeningMode=int;
enum class PowerSaveLevel {PERFORMANCE};
int64_t esp_timer_get_time(){return 5000000;}
struct Board {bool cellular=true;static Board& GetInstance(){static Board b;return b;}
 void SetPowerSaveLevel(PowerSaveLevel){}bool UsesCellularNetwork(){return cellular;}};
struct Protocol {bool opened=false,success=false;
 bool IsAudioChannelOpened(){return opened;}bool OpenAudioChannel(){opened=success;return success;}
 bool DeferUntilReconnectFinishes(std::function<void()>){return false;}
 void CloseAudioChannel(){opened=false;}};
struct Application {Protocol p,*protocol_=&p;int state=kDeviceStateConnecting;int64_t cellular_prewarm_next_us_=0;
 std::atomic<bool> interaction_stopped_{false};bool entry_prompt_before_listening_=true,play_popup_on_listening_=true;
 std::atomic<uint32_t> interaction_generation_{0};bool IsShuttingDown()const{return false;}
 int GetDeviceState()const{return state;}bool SetDeviceState(int s){state=s;return true;}
 void ConsumePrewarmedCellularAudio(){}void SetListeningMode(ListeningMode){}
 void ContinueOpenAudioChannel(ListeningMode);};
''' + method + r'''
int main(){Application a;a.ContinueOpenAudioChannel(0);
 assert(a.state==kDeviceStateIdle&&a.cellular_prewarm_next_us_==35000000);
 Application wifi;Board::GetInstance().cellular=false;wifi.ContinueOpenAudioChannel(0);
 assert(wifi.state==kDeviceStateIdle&&wifi.cellular_prewarm_next_us_==0);}
''')

    def test_background_preconnect_retry_wakeup_and_transport_gate(self):
        source = (ROOT / 'main/application.cc').read_text()
        method = '\n'.join(queue_function(source, name) for name in (
            'void Application::QueueCellularInteraction(',
            'void Application::ReplayCellularInteraction(',
            'void Application::MaybePrewarmCellularAudio('))
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define TAG "test"
using EventBits_t=unsigned;
constexpr int kDeviceStateIdle=0,pdPASS=1;
int64_t now_us=1000000;int64_t esp_timer_get_time(){return now_us;}
std::function<void()> worker;
int xTaskCreate(void(*fn)(void*),const char*,int,void* arg,int,void*){
 assert(!worker);worker=[=]{fn(arg);};return pdPASS;}
std::function<void()> before_task_delete;
void vTaskDelete(void*){if(before_task_delete)before_task_delete();}
unsigned dispatched=0;void xEventGroupSetBits(void*,unsigned bits){dispatched|=bits;}
struct Board{bool cellular=true,online=true,maintenance=false;
 static Board& GetInstance(){static Board b;return b;}
 bool UsesCellularNetwork(){return cellular;}bool IsNetworkAvailable(){return online;}
 bool IsAudioMaintenanceActive(){return maintenance;}};
struct Protocol{bool open=false,success=true;int closes=0,opens=0;std::function<void()> on_open;
 bool IsAudioChannelOpened(){return open;}void CloseAudioChannel(bool){++closes;open=false;}
 bool OpenAudioChannel(){++opens;open=success;if(on_open)on_open();return open;}};
struct Audio{bool enabled=false;void EnableWakeWordDetection(bool x){enabled=x;}};
struct Application{
 std::shared_ptr<Protocol> protocol_=std::make_shared<Protocol>();
 std::atomic<bool> cellular_prewarming_{false},cellular_prewarmed_{false};
 std::atomic<bool> voice_enrollment_active_{false},voice_enrollment_reconnecting_{false};
 std::atomic<bool> barge_in_reconnecting_{false},external_voice_preview_active_{false};
 std::atomic<uint32_t> protocol_generation_{1},interaction_generation_{1};
 int64_t cellular_prewarm_next_us_=0;bool cellular_prewarm_retired_=false;unsigned cellular_prewarm_pending_events_=0;
 uint32_t cellular_pending_interaction_generation_=0,cellular_pending_protocol_generation_=0;
 std::string cellular_prewarm_wake_word_,wake;void* event_group_=nullptr;Audio audio_service_;
 std::function<void()> scheduled;int state=kDeviceStateIdle;
 int GetDeviceState(){return state;}void Schedule(std::function<void()> f){scheduled=std::move(f);}
 void BeginWakeWordInvoke(const std::string& s){wake=s;}
 void QueueCellularInteraction(EventBits_t);void ReplayCellularInteraction();
 void MaybePrewarmCellularAudio();
};
''' + method + r'''
void run(Application& a){auto f=std::move(worker);worker={};f();auto done=std::move(a.scheduled);done();}
int main(){Application a;auto& b=Board::GetInstance();
 b.cellular=false;a.MaybePrewarmCellularAudio();assert(!worker);
 b.cellular=true;b.online=false;a.MaybePrewarmCellularAudio();assert(!worker);
 b.online=true;a.MaybePrewarmCellularAudio();assert(worker && a.cellular_prewarming_);
 // Self-deletion bypasses destructors. Only the app and its completion should own the channel.
 before_task_delete=[&]{assert(a.protocol_.use_count()==2);};
 a.cellular_prewarm_wake_word_="wake";run(a);assert(a.protocol_->open && a.cellular_prewarmed_ && a.wake=="wake");
 before_task_delete={};
 a.protocol_->open=false;a.MaybePrewarmCellularAudio();assert(!worker);
 now_us+=31000000;a.protocol_->success=false;a.MaybePrewarmCellularAudio();assert(worker);
 a.QueueCellularInteraction(4);run(a);assert(!a.cellular_prewarmed_ && dispatched==4);
 now_us+=31000000;a.protocol_->success=true;a.MaybePrewarmCellularAudio();assert(worker);
 a.cellular_prewarm_wake_word_="stale";a.interaction_generation_++;run(a);
 assert(!a.cellular_prewarmed_ && a.wake=="wake" && a.audio_service_.enabled);
 now_us+=31000000;a.protocol_->on_open=[&]{a.interaction_generation_++;};
 a.MaybePrewarmCellularAudio();assert(worker);run(a);
 assert(!a.cellular_prewarmed_ && !a.protocol_->open);
 // A double-click remains visible even if the network drops during preconnect.
 a.protocol_->on_open={};now_us+=31000000;dispatched=0;
 a.MaybePrewarmCellularAudio();assert(worker);a.QueueCellularInteraction(4);b.online=false;
 run(a);assert(dispatched==4);b.online=true;
 // A later explicit stop invalidates the queued request.
 now_us+=31000000;dispatched=0;a.MaybePrewarmCellularAudio();assert(worker);
 a.QueueCellularInteraction(4);a.interaction_generation_++;run(a);assert(dispatched==0);
 // After a completed session there is no repeated background socket churn.
 now_us+=31000000;a.cellular_prewarm_retired_=true;a.MaybePrewarmCellularAudio();assert(!worker);
}
''')

    def test_double_click_waits_for_recovery_or_reports_offline(self):
        source = (ROOT / 'main/application.cc').read_text()
        methods = '\n'.join(queue_function(source, name) for name in (
            'void Application::QueueCellularInteraction(',
            'void Application::ReplayCellularInteraction(',
            'void Application::HandleToggleConversationEvent('))
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 1
#define ESP_LOGW(...) ((void)0)
using EventBits_t=unsigned;
constexpr unsigned MAIN_EVENT_TOGGLE_CONVERSATION=1;
enum State{kDeviceStateIdle,kDeviceStateConnecting,kDeviceStateListening,kDeviceStateSpeaking};
using ListeningMode=int;
unsigned pending_bits=0;void xEventGroupSetBits(void*,unsigned x){pending_bits|=x;}
struct Display{int notifications=0;void ShowNotification(const char*){++notifications;}};
struct Board{bool cellular=true,online=true,maintenance=false;Display display;
 static Board& GetInstance(){static Board b;return b;}
 bool UsesCellularNetwork(){return cellular;}bool IsNetworkAvailable(){return online;}
 bool IsAudioMaintenanceActive(){return maintenance;}Display* GetDisplay(){return &display;}};
struct Protocol{};
struct Application{std::atomic<bool> cellular_prewarming_{false},barge_in_reconnecting_{false};
 std::atomic<bool> voice_enrollment_reconnecting_{false},interaction_stopped_{false};
 std::atomic<uint32_t> interaction_generation_{1},protocol_generation_{1};
 uint32_t cellular_pending_interaction_generation_=0,cellular_pending_protocol_generation_=0;
 unsigned cellular_prewarm_pending_events_=0;void* event_group_=nullptr;
 bool play_popup_on_listening_=false,entry_prompt_before_listening_=false;Protocol* protocol_=nullptr;
 State state=kDeviceStateIdle;std::vector<std::function<void()>> tasks;
 State GetDeviceState(){return state;}bool SetDeviceState(State s){state=s;return true;}
 void StopInteraction(bool){interaction_generation_++;state=kDeviceStateIdle;}
 ListeningMode GetDefaultListeningMode(){return 0;}
 void ContinueOpenAudioChannel(ListeningMode){}
 void Schedule(std::function<void()> f){tasks.push_back(std::move(f));}
 void QueueCellularInteraction(EventBits_t);void ReplayCellularInteraction();
 void HandleToggleConversationEvent();};
''' + methods + r'''
int main(){Application a;Protocol protocol;a.protocol_=&protocol;auto& b=Board::GetInstance();
 a.barge_in_reconnecting_=true;a.HandleToggleConversationEvent();
 assert(a.state==kDeviceStateIdle&&b.display.notifications==1&&a.cellular_prewarm_pending_events_==1);
 a.barge_in_reconnecting_=false;a.ReplayCellularInteraction();assert(pending_bits==1);
 pending_bits=0;a.HandleToggleConversationEvent();assert(a.state==kDeviceStateConnecting&&a.tasks.size()==1);
 a.state=kDeviceStateIdle;b.online=false;a.HandleToggleConversationEvent();
 assert(a.state==kDeviceStateIdle&&b.display.notifications==2);
 b.online=true;a.cellular_prewarming_=true;a.HandleToggleConversationEvent();
 assert(a.cellular_prewarm_pending_events_==1&&b.display.notifications==3);
 a.interaction_generation_++;a.cellular_prewarming_=false;a.ReplayCellularInteraction();assert(pending_bits==0);
}
''')
