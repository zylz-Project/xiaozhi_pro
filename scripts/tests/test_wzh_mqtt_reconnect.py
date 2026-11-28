"""Exercise the production MQTT timer against link loss and shutdown races."""
from pathlib import Path
import unittest

from test_wzh_action_queue import queue_function
import test_wzh_motion_output as output

ROOT = Path(__file__).resolve().parents[2]


class MqttReconnectTest(unittest.TestCase):
    def test_short_link_loss_resumes_once_and_cancellation_expires(self):
        source = (ROOT / 'main/application.cc').read_text()
        methods = '\n'.join(queue_function(source, name) for name in (
            'bool Application::IsConversationRecovering()',
            'void Application::SuspendConversationForNetworkLoss(',
            'void Application::ServiceConversationNetworkRecovery(',
        ))
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#define CONFIG_USE_VOICE_BARGE_IN 1
#define ESP_LOGI(...) ((void)0)
enum State{kDeviceStateIdle,kDeviceStateConnecting,kDeviceStateListening,kDeviceStateSpeaking};
int64_t now=100; int64_t esp_timer_get_time(){return now;}
struct Board{
 bool online=false, maintenance=false;
 static Board& GetInstance(){static Board b;return b;}
 bool IsNetworkAvailable(){return online;} bool IsAudioMaintenanceActive(){return maintenance;}
};
struct Protocol {
 bool ready=false, worker=false;
 bool IsReady(){return ready;}
 bool DeferUntilReconnectFinishes(std::function<void()>){return worker;}
};
struct Audio {
 bool live=true;int resets=0;
 void EnableVoiceProcessing(bool value){live=value;} void ResetDecoder(){++resets;}
};
struct Application{
 State state=kDeviceStateSpeaking;
 bool shutting=false, pending_listening_start_=true, play_popup_on_listening_=true;
 bool entry_prompt_before_listening_=true;
 std::atomic<bool> interaction_stopped_{false}, voice_enrollment_active_{false},
  external_voice_preview_active_{false}, voice_enrollment_reconnecting_{false},
  barge_in_reconnecting_{false};
 std::atomic<uint32_t> interaction_generation_{3},network_resume_generation_{0};
 std::atomic<int64_t> network_resume_deadline_us_{0};
 Audio audio_service_;Protocol protocol;Protocol* protocol_=&protocol;
 int attempts=0;
 State GetDeviceState(){return state;}bool SetDeviceState(State value){state=value;return true;}
 bool IsShuttingDown(){return shutting;}
 void StopInteraction(){++interaction_generation_;interaction_stopped_=true;}
 bool ReconnectAfterVoiceInterruption(uint32_t token,bool close,bool reopen,bool listen){
  assert(token==interaction_generation_&&!close&&reopen&&listen);++attempts;return true;
 }
 bool IsConversationRecovering()const;
 void SuspendConversationForNetworkLoss(int64_t);
 void ServiceConversationNetworkRecovery();
};
''' + methods + r'''
int main(){
 auto& board=Board::GetInstance();Application app;
 app.SuspendConversationForNetworkLoss(20000000);
 assert(app.IsConversationRecovering()&&app.state==kDeviceStateIdle&&!app.audio_service_.live);
 app.ServiceConversationNetworkRecovery();assert(app.attempts==0);
 board.online=true;app.ServiceConversationNetworkRecovery();assert(app.attempts==0);
 app.protocol.ready=true;app.protocol.worker=true;
 app.ServiceConversationNetworkRecovery();assert(app.attempts==0&&app.IsConversationRecovering());
 app.protocol.worker=false;app.ServiceConversationNetworkRecovery();
 assert(app.attempts==1&&app.state==kDeviceStateConnecting&&!app.IsConversationRecovering());
 app.ServiceConversationNetworkRecovery();assert(app.attempts==1);
 Application cancelled;cancelled.protocol.ready=true;
 cancelled.SuspendConversationForNetworkLoss(20000000);cancelled.StopInteraction();
 cancelled.ServiceConversationNetworkRecovery();assert(cancelled.attempts==0);
 Application stale;stale.protocol.ready=true;
 stale.SuspendConversationForNetworkLoss(20000000);now+=20000000;
 stale.ServiceConversationNetworkRecovery();assert(stale.attempts==0);
 Application record;record.voice_enrollment_active_=true;
 record.SuspendConversationForNetworkLoss(20000000);
 assert(record.state==kDeviceStateIdle&&!record.IsConversationRecovering());
}
''')

    def test_conversation_disconnect_recovery_is_bounded_and_respects_stop(self):
        source = (ROOT / 'main/application.cc').read_text()
        recovery = queue_function(source, 'if (transport_disconnected && !interaction_stopped_.load()')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#define ESP_LOGW(...) ((void)0)
#define TAG "test"
enum {kDeviceStateIdle,kDeviceStateListening,kDeviceStateSpeaking};
struct Board {bool online=true;bool IsNetworkAvailable(){return online;}};
struct Application {std::atomic<bool> interaction_stopped_{false},barge_in_reconnecting_{false};
 uint32_t disconnect_recovery_generation_=0;bool disconnect_recovery_attempted_=false;
 int state=kDeviceStateSpeaking,attempts=0;bool started=true;
 int GetDeviceState(){return state;}void SetDeviceState(int s){state=s;}
 bool ReconnectAfterVoiceInterruption(uint32_t,bool,bool,bool){++attempts;return started;}
 void Run(Board& board,bool transport_disconnected,uint32_t generation){
''' + recovery + r'''
 state=kDeviceStateIdle;}};
int main(){Board b;Application p;p.Run(b,true,1);
 assert(p.attempts==1&&p.state==kDeviceStateListening&&p.barge_in_reconnecting_);
 p.barge_in_reconnecting_=false;p.state=kDeviceStateSpeaking;p.Run(b,true,1);
 assert(p.attempts==1&&p.state==kDeviceStateIdle);
 p.state=kDeviceStateSpeaking;p.Run(b,false,2);assert(p.attempts==1);
 p.state=kDeviceStateSpeaking;p.interaction_stopped_=true;p.Run(b,true,2);assert(p.attempts==1);
 p.interaction_stopped_=false;p.state=kDeviceStateSpeaking;b.online=false;p.Run(b,true,2);
 assert(p.attempts==1);b.online=true;p.state=kDeviceStateSpeaking;p.Run(b,true,2);
 assert(p.attempts==2&&p.state==kDeviceStateListening);}
''')

    def test_mqtt_connect_failure_rechecks_only_cellular(self):
        source = (ROOT / 'main/protocols/mqtt_protocol.cc').read_text()
        failure = queue_function(source,
            'if (auto connected = mqtt_->Connect(broker_address, broker_port, client_id, username, password);')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <string>
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
namespace Lang {namespace Strings {constexpr auto SERVER_NOT_CONNECTED="offline";}}
struct Error {std::string ToString()const{return "timeout";}};
struct Result {bool ok;explicit operator bool()const{return ok;}Error error()const{return {};}};
struct Client {bool ok=false;int disconnects=0;
 Result Connect(std::string,int,std::string,std::string,std::string){return {ok};}
 void Disconnect(){++disconnects;}};
struct Protocol {Client client,*mqtt_=&client;std::atomic<bool> mqtt_connected_{true};
 bool mqtt_transport_cellular_=false;int rechecks=0,errors=0;
 void ScheduleCellularRecheck(){++rechecks;}void SetError(const char*){++errors;}
 bool Run(bool report_error){std::string broker_address,client_id,username,password;int broker_port=1;
''' + failure + r'''
 return true;}};
int main(){Protocol p;assert(!p.Run(true)&&p.errors==1&&p.rechecks==0);
 p.mqtt_transport_cellular_=true;assert(!p.Run(false)&&p.rechecks==1&&p.errors==1);
 p.client.ok=true;assert(p.Run(true)&&p.rechecks==1&&p.client.disconnects==2);}
''')

    def test_udp_connect_failure_rechecks_only_cellular(self):
        source = (ROOT / 'main/protocols/mqtt_protocol.cc').read_text()
        failure = queue_function(source,
                                 'if (auto connected = udp->Connect(udp_server_, udp_port_); !connected)')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <cassert>
#include <string>
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
namespace Lang {namespace Strings {constexpr auto SERVER_NOT_CONNECTED="offline";}}
struct Board {bool cellular=false;bool UsesCellularNetwork()const{return cellular;}};
struct NetError {std::string ToString()const{return "failed";}};
struct NetResult {bool ok=false;explicit operator bool()const{return ok;}NetError error()const{return {};}};
struct Udp {bool success=false;NetResult Connect(const std::string&,int){return NetResult{success};}};
struct Protocol {Udp client,*udp=&client;std::string udp_server_;int udp_port_=1;
 int errors=0,rechecks=0;void SetError(const char*){++errors;}void ScheduleCellularRecheck(){++rechecks;}
 bool Run(Board& board){
''' + failure + r'''
 return true;}};
int main(){Board b;Protocol p;assert(!p.Run(b)&&p.errors==0&&p.rechecks==0);
 b.cellular=true;assert(!p.Run(b)&&p.errors==1&&p.rechecks==1);
 p.client.success=true;assert(p.Run(b)&&p.errors==1&&p.rechecks==1);}
''')

    def test_stale_socket_is_destroyed_without_channel_lock(self):
        source = (ROOT / 'main/protocols/mqtt_protocol.cc').read_text()
        discard = queue_function(source, 'void MqttProtocol::DiscardStaleAudioChannel(')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <cassert>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#define ESP_LOGI(...) ((void)0)
#define TAG "test"
struct Udp {std::mutex* lock;bool* unlocked;int* destroyed;
 Udp(std::mutex* l,bool* u,int* d):lock(l),unlocked(u),destroyed(d){}
 ~Udp(){std::thread t([&]{if(lock->try_lock()){*unlocked=true;lock->unlock();}});t.join();++*destroyed;}};
struct MqttProtocol {std::mutex channel_mutex_;std::shared_ptr<Udp> udp_;
 void DiscardStaleAudioChannel();};
''' + discard + r'''
int main(){MqttProtocol p;bool unlocked=false;int destroyed=0;
 p.udp_=std::make_shared<Udp>(&p.channel_mutex_,&unlocked,&destroyed);
 p.DiscardStaleAudioChannel();assert(!p.udp_&&unlocked&&destroyed==1);
 p.DiscardStaleAudioChannel();assert(destroyed==1);}
''')

    def test_connected_event_marks_mqtt_ready(self):
        source = (ROOT / 'main/protocols/mqtt_protocol.cc').read_text()
        callback = queue_function(source, 'mqtt_->OnConnected(') + ');'
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <utility>
int stopped=0;void esp_timer_stop(void*){++stopped;}
struct Board {bool online=true,cellular=false;static Board& GetInstance(){static Board b;return b;}
 bool IsNetworkAvailable()const{return online;}bool UsesCellularNetwork()const{return cellular;}};
struct Client {std::function<void()> connected;
 void OnConnected(std::function<void()> f){connected=std::move(f);}};
struct Protocol {Client client,*mqtt_=&client;std::atomic<bool> mqtt_connected_{false};
 std::atomic<uint32_t> link_generation_{0};uint32_t mqtt_link_generation_=0;
 bool mqtt_transport_cellular_=false;
 void* reconnect_timer_=nullptr;std::function<void()> on_connected_;
 void Bind(){
''' + callback + r'''
 }};
int main(){Protocol p;int called=0;p.on_connected_=[&]{++called;};p.Bind();
 Board::GetInstance().online=false;p.client.connected();assert(!p.mqtt_connected_&&stopped==0);
 Board::GetInstance().online=true;++p.link_generation_;p.client.connected();
 assert(!p.mqtt_connected_&&called==0&&stopped==0);
 p.mqtt_link_generation_=1;p.Bind();
 Board::GetInstance().cellular=true;p.client.connected();
 assert(!p.mqtt_connected_&&called==0&&stopped==0);
 p.mqtt_transport_cellular_=true;p.Bind();p.client.connected();
 assert(p.mqtt_connected_&&called==1&&stopped==1);}
''')

    def test_failed_cellular_uplink_closes_without_goodbye(self):
        source = (ROOT / 'main/application.cc').read_text()
        close = queue_function(source, 'if (request->close_first && protocol->IsAudioChannelOpened())')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <cassert>
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 1
struct Board {bool cellular=true;static Board& GetInstance(){static Board b;return b;}
 bool UsesCellularNetwork()const{return cellular;}};
struct Protocol {bool open=true,goodbye=true;int closes=0;
 bool IsAudioChannelOpened(){return open;}
 void CloseAudioChannel(bool send_goodbye=true){goodbye=send_goodbye;open=false;++closes;}};
struct Request {bool close_first=true;};
void close_for_recovery(Request* request,Protocol* protocol){
''' + close + r'''
}
int main(){Request req;Protocol p;close_for_recovery(&req,&p);
 assert(p.closes==1&&!p.goodbye);
 p.open=true;Board::GetInstance().cellular=false;close_for_recovery(&req,&p);
 assert(p.closes==2&&p.goodbye);
 p.open=true;req.close_first=false;close_for_recovery(&req,&p);assert(p.closes==2);
}
''')

    def test_delayed_goodbye_does_not_close_replacement_channel(self):
        source = (ROOT / 'main/protocols/mqtt_protocol.cc').read_text()
        callback = queue_function(source, 'Application::GetInstance().Schedule([this, alive, channel]()') + ');'
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
struct Udp {};
struct Application {std::function<void()> pending;
 static Application& GetInstance(){static Application a;return a;}
 void Schedule(std::function<void()> f){pending=std::move(f);}
 void Run(){auto f=std::move(pending);pending={};f();}};
struct Protocol {std::mutex channel_mutex_;std::shared_ptr<Udp> udp_;
 std::function<void()> on_audio_channel_closed_;
 std::shared_ptr<std::atomic<bool>> alive_=std::make_shared<std::atomic<bool>>(true);
 void Goodbye(){std::weak_ptr<Udp> channel=udp_;auto alive=alive_;
''' + callback + r'''
 }};
int main(){Protocol p;auto& app=Application::GetInstance();int closes=0;
 p.on_audio_channel_closed_=[&]{++closes;};
 p.udp_=std::make_shared<Udp>();p.Goodbye();p.udp_=std::make_shared<Udp>();
 app.Run();assert(p.udp_ && closes==0);
 p.Goodbye();app.Run();assert(!p.udp_ && closes==1);
 p.udp_=std::make_shared<Udp>();p.Goodbye();*p.alive_=false;
 app.Run();assert(p.udp_ && closes==1);
}
''')

    def test_stale_connected_event_does_not_start_activation(self):
        source = (ROOT / 'main/application.cc').read_text()
        method = queue_function(source, 'void Application::HandleNetworkConnectedEvent(')
        connected_callback = queue_function(source, 'protocol_->OnConnected(') + ');'
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
constexpr int pdPASS=1;
enum {kDeviceStateStarting,kDeviceStateWifiConfiguring,kDeviceStateIdle,kDeviceStateActivating};
constexpr int MAIN_EVENT_ACTIVATION_DONE=1;
int tasks=0;bool allocation_ok=true;
int xTaskCreate(void(*)(void*),const char*,int,void*,int,void**){++tasks;return allocation_ok?pdPASS:0;}
void xEventGroupSetBits(void*,int){}
void vTaskDelete(void*){}
struct Display{int updates=0;void UpdateStatusBar(bool){++updates;}};
struct Board{bool online=false;Display display;static Board& GetInstance(){static Board b;return b;}
 bool IsNetworkAvailable(){return online;}Display* GetDisplay(){return &display;}};
struct Protocol{int ready=0;std::function<void()> connected;
 void OnNetworkReady(){++ready;}
 void OnConnected(std::function<void()> fn){connected=std::move(fn);}};
struct Application{int state=kDeviceStateIdle;Protocol* protocol_=nullptr;bool activation_running_=false;
 void* event_group_=nullptr;bool activation_network_ready_pending_=false;
 std::atomic<uint32_t> protocol_generation_{1};std::function<void()> scheduled;
 bool recovering=false;int attempts=0,dismissals=0;
 bool shutting_down=false;bool IsShuttingDown()const{return shutting_down;}
 bool IsConversationRecovering()const{return recovering;}
 void ServiceConversationNetworkRecovery(){if(recovering)++attempts;}
 void DismissAlert(){++dismissals;}
 void Schedule(std::function<void()> work){assert(!scheduled);scheduled=std::move(work);}
 void Run(){auto work=std::exchange(scheduled, {});assert(work);work();}
 int GetDeviceState(){return state;}void SetDeviceState(int s){state=s;}void ActivationTask(){}
 void BindConnected(uint32_t protocol_generation){
''' + connected_callback + r'''
 }
 void HandleNetworkConnectedEvent();};
''' + method + r'''
int main(){Application app;auto& board=Board::GetInstance();
 app.HandleNetworkConnectedEvent();assert(tasks==0&&app.state==kDeviceStateIdle&&board.display.updates==0);
 board.online=true;app.shutting_down=true;app.HandleNetworkConnectedEvent();
 assert(tasks==0&&app.state==kDeviceStateIdle&&board.display.updates==0);
 app.shutting_down=false;app.HandleNetworkConnectedEvent();
 assert(tasks==1&&app.state==kDeviceStateActivating&&board.display.updates==1);
 Protocol p;app.protocol_=&p;app.HandleNetworkConnectedEvent();
 assert(p.ready==0&&app.activation_network_ready_pending_);
 app.state=kDeviceStateIdle;app.HandleNetworkConnectedEvent();assert(p.ready==1&&tasks==1);
 assert(!app.scheduled);
 app.recovering=true;app.HandleNetworkConnectedEvent();
 assert(p.ready==2&&app.scheduled&&app.attempts==0);
 app.Run();assert(app.attempts==1);
 app.BindConnected(1);p.connected();
 assert(app.scheduled&&app.attempts==1);
 app.Run();assert(app.attempts==2&&app.dismissals==1);
 p.connected();++app.protocol_generation_;
 app.Run();assert(app.attempts==2&&app.dismissals==1);
 app.protocol_=nullptr;app.activation_running_=false;allocation_ok=false;app.HandleNetworkConnectedEvent();
 assert(tasks==2&&app.state==kDeviceStateIdle&&!app.activation_running_);
}
''')

    def test_transport_internal_retry_is_stopped_after_network_loss(self):
        source = (ROOT / 'main/protocols/mqtt_protocol.cc').read_text()
        callback = queue_function(source, 'mqtt_->OnDisconnected(') + ');'
        suspend = queue_function(source, 'void MqttProtocol::SuspendUnavailableMqtt(').replace('MqttProtocol::', 'Protocol::')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#define ESP_LOGI(...) ((void)0)
#define TAG "test"
constexpr int MQTT_RECONNECT_INTERVAL_MS=60000,MQTT_DEFERRED_RETRY_MS=5000;
int timers=0,stops=0;int64_t delay=0;
int esp_timer_start_once(void*,int64_t d){++timers;delay=d;return 0;}
int esp_timer_stop(void*){++stops;return 0;}
struct Board{bool online=true,maintenance=false;static Board& GetInstance(){static Board b;return b;}
 bool IsNetworkAvailable()const{return online;}bool IsAudioMaintenanceActive(){return maintenance;}};
struct Application{std::function<void()> pending;static Application& GetInstance(){static Application a;return a;}
 void Schedule(std::function<void()>&& f){pending=std::move(f);}void Run(){auto f=std::move(pending);pending={};f();}};
struct Client{std::function<void()> disconnected;int disconnects=0;
 void OnDisconnected(std::function<void()> f){disconnected=std::move(f);}void Disconnect(){++disconnects;}};
struct Udp{};
struct Protocol{std::unique_ptr<Client> mqtt_=std::make_unique<Client>();void* reconnect_timer_=nullptr;std::mutex mqtt_mutex_;
 std::atomic<bool> mqtt_connected_{true};
 std::mutex channel_mutex_;std::shared_ptr<Udp> udp_;std::function<void()> on_audio_channel_closed_;
 std::shared_ptr<std::atomic<bool>> alive_=std::make_shared<std::atomic<bool>>(true);
 void SuspendUnavailableMqtt();
 std::function<void()> on_disconnected_;void Bind(){
''' + callback + r'''
 }};
''' + suspend + r'''
int main(){Protocol p;auto& b=Board::GetInstance();auto& app=Application::GetInstance();p.Bind();
 int closes=0;p.on_audio_channel_closed_=[&]{++closes;};p.udp_=std::make_shared<Udp>();
 p.mqtt_->disconnected();assert(!p.mqtt_connected_&&timers==1&&delay==60000000);app.Run();assert(p.mqtt_->disconnects==0&&!p.udp_&&closes==1);
 p.udp_=std::make_shared<Udp>();p.mqtt_->disconnected();p.udp_=std::make_shared<Udp>();app.Run();
 assert(p.udp_&&closes==1); // An old disconnect cannot close the replacement channel.
 b.online=false;p.mqtt_->disconnected();app.Run();assert(p.mqtt_->disconnects==1&&stops==1&&delay==5000000);
 assert(!p.udp_&&closes==2);
 std::atomic<bool> locked{false},release{false};std::thread connect([&]{
  std::lock_guard<std::mutex> hold(p.mqtt_mutex_);locked=true;while(!release.load())std::this_thread::yield();});
 while(!locked.load())std::this_thread::yield();
 p.mqtt_->disconnected();app.Run();assert(p.mqtt_->disconnects==1&&delay==5000000);
 release=true;connect.join();p.SuspendUnavailableMqtt();assert(p.mqtt_->disconnects==2);
 b.online=true;b.maintenance=true;p.mqtt_->disconnected();app.Run();assert(p.mqtt_->disconnects==3);
 b.maintenance=false;p.mqtt_->disconnected();*p.alive_=false;app.Run();assert(p.mqtt_->disconnects==3);
}
''')

    def test_send_text_handles_unavailable_mqtt_client(self):
        source = (ROOT / 'main/protocols/mqtt_protocol.cc').read_text()
        send = queue_function(source, 'bool MqttProtocol::SendText(')
        connected = queue_function(source, 'bool MqttProtocol::IsMqttConnectedLocked(')
        recheck = queue_function(source, 'void MqttProtocol::ScheduleCellularRecheck(')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <cassert>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)publish_started_us)
#define TAG "test"
namespace Lang {namespace Strings {constexpr auto SERVER_ERROR="error";}}
int64_t esp_timer_get_time(){return 0;}
struct Board {bool cellular=false,online=true;int rechecks=0;static Board& GetInstance(){static Board b;return b;}
 bool UsesCellularNetwork()const{return cellular;}bool IsNetworkAvailable()const{return online;}
 size_t GetProtocolPayloadLimit()const{return 0;}
 void RequestNetworkRecheck(){++rechecks;}};
struct Application {std::function<void()> pending;static Application& GetInstance(){static Application a;return a;}
 void Schedule(std::function<void()> f){pending=std::move(f);}
 void Run(){auto f=std::move(pending);pending={};f();}};
struct Client {bool connected=false,publish_ok=true;int publishes=0,queries=0;
 bool IsConnected(){++queries;return connected;}bool Publish(const std::string&,const std::string&){++publishes;return publish_ok;}};
struct MqttProtocol {std::string publish_topic_="topic";std::unique_ptr<Client> mqtt_;std::mutex mqtt_mutex_;int errors=0;
 std::atomic<bool> mqtt_connected_{false};bool mqtt_transport_cellular_=false;
 std::atomic<uint32_t> link_generation_{0};uint32_t mqtt_link_generation_=0;
 std::shared_ptr<std::atomic<bool>> alive_=std::make_shared<std::atomic<bool>>(true);
 void SetError(const char*){++errors;}bool IsMqttConnectedLocked();void ScheduleCellularRecheck();
 bool SendText(const std::string&);};
''' + connected + '\n' + recheck + '\n' + send + r'''
int main(){MqttProtocol p;assert(!p.SendText("test"));p.mqtt_=std::make_unique<Client>();
 assert(!p.SendText("test")&&p.mqtt_->publishes==0);p.mqtt_->connected=true;
 assert(p.SendText("test")&&p.mqtt_->publishes==1);
 Board::GetInstance().online=false;assert(!p.SendText("test"));
 Board::GetInstance().online=true;++p.link_generation_;
 assert(!p.SendText("test")&&p.mqtt_->publishes==1); // Old TCP socket after a new IP event.
 p.mqtt_link_generation_=1;
 std::atomic<bool> locked{false},release{false};std::thread connector([&]{
  std::lock_guard<std::mutex> hold(p.mqtt_mutex_);locked=true;while(!release.load())std::this_thread::yield();});
 while(!locked.load())std::this_thread::yield();
 assert(!p.SendText("test")&&p.mqtt_->publishes==1);release=true;connector.join();
 Board::GetInstance().cellular=true;p.mqtt_connected_=true;
 assert(!p.SendText("test")); // A Wi-Fi client must not be reused for 4G.
 p.mqtt_transport_cellular_=true;
 int queries=p.mqtt_->queries;assert(p.SendText("test")&&p.mqtt_->queries==queries);
 ++p.link_generation_;assert(!p.SendText("test")&&p.mqtt_->queries==queries);
 p.mqtt_link_generation_=p.link_generation_.load();
 Board::GetInstance().online=false;assert(!p.SendText("test")&&p.mqtt_->queries==queries);
 Board::GetInstance().online=true;
 p.mqtt_->publish_ok=false;
 assert(!p.SendText("test")&&p.errors==1&&!p.mqtt_connected_&&Board::GetInstance().rechecks==0);
 Application::GetInstance().Run();assert(Board::GetInstance().rechecks==1);
 int publishes=p.mqtt_->publishes;assert(!p.SendText("test")&&p.mqtt_->publishes==publishes);
 p.mqtt_connected_=true;assert(!p.SendText("test"));
 Board::GetInstance().cellular=false;Application::GetInstance().Run();
 assert(Board::GetInstance().rechecks==1);p.mqtt_->connected=true;p.mqtt_->publish_ok=true;
 assert(!p.SendText("test"));p.mqtt_transport_cellular_=false;
 assert(p.SendText("test")&&p.mqtt_->queries==queries+1);
 Board::GetInstance().cellular=true;p.mqtt_transport_cellular_=true;p.mqtt_connected_=true;
 p.mqtt_->publish_ok=false;assert(!p.SendText("test"));*p.alive_=false;
 Application::GetInstance().Run();assert(Board::GetInstance().rechecks==1);}
''')

    def test_both_transports_reject_offline_or_maintenance_before_connecting(self):
        for filename, signature in (
            ('mqtt_protocol.cc', 'bool MqttProtocol::StartMqttClient(bool report_error)'),
            ('websocket_protocol.cc', 'bool WebsocketProtocol::OpenAudioChannel()'),
        ):
            source = (ROOT / 'main/protocols' / filename).read_text()
            prefix = source[source.index(signature):]
            prefix = prefix[:prefix.index('    Settings settings(')] + '    return true;\n}'
            if filename == 'mqtt_protocol.cc':
                # The real guard precedes disposal of the old MQTT client.
                prefix = prefix[:prefix.index('    if (mqtt_ != nullptr)')] + '    return true;\n}'
            prefix = prefix.replace('MqttProtocol::', 'Transport::').replace('WebsocketProtocol::', 'Transport::')
            with self.subTest(transport=filename):
                output.WzhMotionOutputTest.run_cpp(self, r'''
#include <cassert>
#include <mutex>
#define ESP_LOGI(...) ((void)0)
#define TAG "test"
namespace Lang {namespace Strings {constexpr auto SERVER_NOT_CONNECTED="offline";}}
struct Board {bool online=true,maintenance=false;static Board& GetInstance(){static Board b;return b;}
 bool IsNetworkAvailable()const{return online;}bool IsAudioMaintenanceActive(){return maintenance;}};
struct Transport {int errors=0;std::mutex mqtt_mutex_;void SetError(const char*){++errors;}
 bool StartMqttClient(bool);bool OpenAudioChannel();};
''' + prefix + r'''
int main(){Transport t;auto& b=Board::GetInstance();
 b.online=false;
''' + (' assert(!t.StartMqttClient(false)&&t.errors==0);assert(!t.StartMqttClient(true)&&t.errors==1);\n'
       if filename == 'mqtt_protocol.cc' else
       ' assert(!t.OpenAudioChannel()&&t.errors==1);\n') + r'''
 b.online=true;b.maintenance=true;
''' + (' assert(!t.StartMqttClient(true)&&t.errors==1);\n'
       if filename == 'mqtt_protocol.cc' else
       ' assert(!t.OpenAudioChannel()&&t.errors==1);\n') + r'''
 b.maintenance=false;
''' + (' assert(t.StartMqttClient(true));\n' if filename == 'mqtt_protocol.cc'
       else ' assert(t.OpenAudioChannel());\n') + r'''
}
''')

    def test_retry_waits_for_network_and_maintenance_then_rechecks_scheduled_work(self):
        source = (ROOT / 'main/protocols/mqtt_protocol.cc').read_text()
        constructor = queue_function(source, 'MqttProtocol::MqttProtocol()')
        start = queue_function(source, 'bool MqttProtocol::Start()')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#define ESP_LOGI(...) ((void)0)
#define TAG "test"
constexpr int kDeviceStateIdle=0, MQTT_RECONNECT_INTERVAL_MS=60000, MQTT_DEFERRED_RETRY_MS=5000;
struct TimerArgs {void(*callback)(void*)=nullptr;void* arg=nullptr;};
using esp_timer_create_args_t=TimerArgs;
TimerArgs timer;int retries=0;int64_t last_duration=0;
int esp_timer_create(const TimerArgs* args,void** handle){timer=*args;*handle=&timer;return 0;}
int esp_timer_start_once(void*,int64_t duration){last_duration=duration;++retries;return 0;}
void* xEventGroupCreate(){return nullptr;}
struct Board {bool online=true,maintenance=false,cellular=true;static Board& GetInstance(){static Board b;return b;}
 bool IsNetworkAvailable()const{return online;}bool IsAudioMaintenanceActive(){return maintenance;}
 bool UsesCellularNetwork()const{return cellular;}};
struct Application {int state=kDeviceStateIdle;std::function<void()> pending;
 static Application& GetInstance(){static Application a;return a;}int GetDeviceState(){return state;}
 void Schedule(std::function<void()>&& fn){pending=std::move(fn);}void Run(){auto fn=std::move(pending);pending={};fn();}};
struct Client {bool connected=false;bool IsConnected(){return connected;}};
struct MqttProtocol {void* event_group_handle_=nullptr;void* reconnect_timer_=nullptr;
 std::unique_ptr<Client> mqtt_;std::mutex mqtt_mutex_;std::atomic<bool> suspend_queued_{false};int suspends=0;
 bool reconnect_running_=false;
 std::shared_ptr<std::atomic<bool>> alive_=std::make_shared<std::atomic<bool>>(true);
 int connects=0;bool result=true;MqttProtocol();bool Start();void SuspendUnavailableMqtt(){++suspends;}
 void StartReconnectTask(){if(!StartMqttClient(false))esp_timer_start_once(reconnect_timer_,60000000);}
 bool IsMqttConnectedLocked(){return mqtt_&&mqtt_->IsConnected();}
 bool StartMqttClient(bool){++connects;return result;}};
''' + constructor + '\n' + start + r'''
int main(){MqttProtocol p;auto& b=Board::GetInstance();auto& app=Application::GetInstance();
 b.online=false;timer.callback(timer.arg);assert(retries==1&&last_duration==5000000&&app.pending&&p.connects==0);
 timer.callback(timer.arg);assert(retries==2&&p.suspend_queued_);app.Run();assert(p.suspends==1&&!p.suspend_queued_);
 b.online=true;b.maintenance=true;timer.callback(timer.arg);assert(retries==3&&app.pending);app.Run();assert(p.suspends==2);
 b.maintenance=false;app.state=1;timer.callback(timer.arg);assert(retries==4&&!app.pending);
 app.state=0;timer.callback(timer.arg);assert(app.pending);b.online=false;app.Run();
 assert(retries==5&&p.connects==0);
 b.online=true;timer.callback(timer.arg);b.maintenance=true;app.Run();
 assert(retries==6&&p.connects==0);
 b.maintenance=false;p.result=false;timer.callback(timer.arg);app.Run();
 assert(retries==7&&last_duration==60000000&&p.connects==1);
 p.result=true;timer.callback(timer.arg);app.Run();assert(retries==7&&p.connects==2);
 p.mqtt_=std::make_unique<Client>();timer.callback(timer.arg);p.mqtt_->connected=true;app.Run();
 assert(p.connects==2&&retries==7);
 timer.callback(timer.arg);*p.alive_=false;app.Run();assert(p.connects==2);
 MqttProtocol fresh;fresh.result=false;b.online=false;assert(!fresh.Start());
 assert(last_duration==5000000);b.online=true;assert(!fresh.Start());assert(last_duration==5000000);
 b.cellular=false;assert(!fresh.Start());assert(last_duration==60000000);
}
''')

    def test_network_ready_rearms_retry_and_connects_off_application_task(self):
        source = (ROOT / 'main/protocols/mqtt_protocol.cc').read_text()
        methods = '\n'.join(queue_function(source, signature) for signature in (
            'void MqttProtocol::OnNetworkLost()', 'void MqttProtocol::OnNetworkReady()',
            'bool MqttProtocol::DeferUntilReconnectFinishes(',
            'void MqttProtocol::StartReconnectTask()'))
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <utility>
#define ESP_LOGW(...) ((void)0)
#define TAG "test"
constexpr int kDeviceStateIdle=0,pdPASS=1,kLinkResumeRetryMs=250;
constexpr int MQTT_DEFERRED_RETRY_MS=5000,MQTT_RECONNECT_INTERVAL_MS=60000;
int64_t delay=60000000;
int esp_timer_stop(void*){delay=0;return 0;}
int esp_timer_start_once(void*,int64_t value){if(delay)return 1;delay=value;return 0;}
void(*task)(void*)=nullptr;void* payload=nullptr;bool allocation_ok=true;
int xTaskCreate(void(*fn)(void*),const char*,int,void* data,int,void*){
 if(!allocation_ok)return 0;
 assert(!task);task=fn;payload=data;return pdPASS;}
void vTaskDelete(void*){}
void run_task(){const auto fn=task;task=nullptr;fn(payload);}
struct Board{bool online=true,maintenance=false,cellular=false;static Board& GetInstance(){static Board b;return b;}
 bool IsNetworkAvailable()const{return online;}bool IsAudioMaintenanceActive()const{return maintenance;}
 bool UsesCellularNetwork()const{return cellular;}};
struct Application{int state=kDeviceStateIdle;static Application& GetInstance(){static Application a;return a;}
 std::function<void()> pending;int GetDeviceState()const{return state;}
 void Schedule(std::function<void()> fn){pending=std::move(fn);}
 void Run(){auto fn=std::exchange(pending,{});assert(fn);fn();}};
struct MqttProtocol:std::enable_shared_from_this<MqttProtocol>{
 bool reconnect_running_=false;std::atomic<uint32_t> link_generation_{0};
 std::atomic<bool> mqtt_connected_{true};
 std::function<void()> reconnect_completion_;
 void* reconnect_timer_=nullptr;int connects=0;bool succeed=false,lose_link=false;
 void OnNetworkLost();void OnNetworkReady();void StartReconnectTask();
 bool DeferUntilReconnectFinishes(std::function<void()>);
 bool StartMqttClient(){++connects;if(lose_link)OnNetworkLost();return succeed;}};
''' + methods + r'''
int main(){auto p=std::make_shared<MqttProtocol>();auto& b=Board::GetInstance();
 auto& app=Application::GetInstance();int resumed=0;
 p->OnNetworkLost();assert(p->link_generation_==1&&delay==60000000&&!p->mqtt_connected_);
 b.cellular=true;p->OnNetworkReady();assert(delay==250000);
 b.cellular=false;p->OnNetworkReady();assert(delay==250000);b.cellular=true;
 delay=0;p->StartReconnectTask();assert(task&&p->connects==0);
 p->StartReconnectTask();assert(delay==0&&p->connects==0);
 assert(p->DeferUntilReconnectFinishes([&]{++resumed;}));
 b.online=false;run_task();assert(!resumed&&p->reconnect_running_);app.Run();
 assert(delay==5000000&&p->connects==0&&resumed==1&&!p->reconnect_running_);
 assert(!p->DeferUntilReconnectFinishes([&]{++resumed;}));
 b.online=true;delay=0;p->StartReconnectTask();run_task();app.Run();
 assert(delay==60000000&&p->connects==1);
 p->lose_link=true;delay=0;p->StartReconnectTask();run_task();app.Run();
 assert(delay==250000&&p->connects==2);
 delay=0;allocation_ok=false;p->StartReconnectTask();assert(!task&&delay==5000000);
 delay=0;allocation_ok=true;p->succeed=true;p->lose_link=false;p->StartReconnectTask();
 std::weak_ptr<MqttProtocol> owner=p;p.reset();assert(!owner.expired());
 run_task();assert(!owner.expired());app.Run();assert(owner.expired()&&delay==0);
}
''')

    def test_radio_switch_waits_for_previous_mqtt_worker_to_release_connection(self):
        method = queue_function((ROOT / 'main/application.cc').read_text(),
                                'void Application::ResetProtocol(')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 1
#define ESP_LOGW(...) ((void)0)
constexpr int pdPASS=1;
constexpr int kDeviceStateIdle=0;
int destroyed=0,closed=0;
std::function<void()> cleanup;
int xTaskCreate(void (*run)(void*),const char*,int,void* arg,int,void*){
 assert(!cleanup);cleanup=[run,arg]{run(arg);};return pdPASS;
}
void vTaskDelete(void*){}
struct Protocol{
 bool running=true;std::function<void()> completion;
 ~Protocol(){++destroyed;}
 bool DeferUntilReconnectFinishes(std::function<void()> next){
  if(!running)return false;
  completion=std::move(next);return true;}
 bool IsAudioChannelOpened(){return false;}
 void CloseAudioChannel(bool goodbye=true){assert(!goodbye);++closed;}
 void CloseTransport(){CloseAudioChannel(false);}
};
struct Enrollment{void Cancel(){}};
struct Board{
 static Board& GetInstance(){static Board b;return b;}
 void SetVoiceEnrollmentRecording(bool){} Enrollment* GetVoiceEnrollment(){return nullptr;}
};
struct Audio{void EnableVoiceProcessing(bool){}};
struct Application{
 std::shared_ptr<Protocol> protocol_=std::make_shared<Protocol>();
 std::deque<std::function<void()>> pending;
 std::atomic<bool> cellular_server_connected_{true},barge_in_reconnecting_{false},
  voice_enrollment_active_{false},voice_enrollment_reconnecting_{false},conversation_configured_{true};
 std::atomic<unsigned> protocol_generation_{0},interaction_generation_{0},network_resume_generation_{0};
 Audio audio_service_;void SetDeviceState(int){}
 void Schedule(std::function<void()> next){pending.push_back(std::move(next));}
 void Run(){auto next=std::move(pending.front());pending.pop_front();next();}
 void ResetProtocol(std::function<void()>);
};
''' + method + r'''
int main(){
 Application app;bool switched=false;
 app.interaction_generation_=4;app.network_resume_generation_=4;
 auto worker=app.protocol_;
 app.ResetProtocol([&]{assert(!app.protocol_&&destroyed==1);switched=true;});
 app.Run();assert(app.protocol_&&!switched&&app.pending.empty()&&worker->completion);
 worker->running=false;auto finished=std::move(worker->completion);finished();worker.reset();
 assert(app.protocol_&&!switched&&destroyed==0);
 app.Run();assert(!app.protocol_&&!switched&&destroyed==0&&cleanup&&app.pending.empty());
 auto teardown=std::exchange(cleanup,{});teardown();
 assert(!switched&&destroyed==0&&closed==1);
 app.Run();assert(switched&&!app.cellular_server_connected_&&!app.conversation_configured_&&app.protocol_generation_==1);
 assert(destroyed==1);
 assert(app.interaction_generation_==5&&app.network_resume_generation_==5);
}
''')

    def test_cellular_audio_send_keeps_one_packet_in_flight_and_drops_old_generation(self):
        source = (ROOT / 'main/application.cc').read_text()
        methods = '\n'.join(queue_function(source, name) for name in (
            'void Application::QueueCellularAudioSend(', 'void Application::RunCellularAudioSend('))
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#define ESP_LOGW(...) ((void)0)
constexpr int pdPASS=1,pdTRUE=1,portMAX_DELAY=1,MAIN_EVENT_SEND_AUDIO=1;
using TaskHandle_t=void*;
std::function<void()> worker;
bool notification=false;
int send_events=0;
struct WaitForMoreWork{};
int xTaskCreate(void (*run)(void*),const char*,int,void* arg,int,TaskHandle_t* task){
 worker=[run,arg]{run(arg);};*task=arg;return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t){notification=true;}
void ulTaskNotifyTake(int,int){if(!std::exchange(notification,false))throw WaitForMoreWork{};}
void xEventGroupSetBits(int,int){++send_events;}
struct Board{
 bool cellular=true,online=true;
 static Board& GetInstance(){static Board b;return b;}
 bool UsesCellularNetwork(){return cellular;}
 bool IsNetworkAvailable(){return online;}
};
struct AudioStreamPacket{};
struct Protocol{
 int sends=0;bool send_ok=true;
 bool SendAudio(std::unique_ptr<AudioStreamPacket> packet){assert(packet);++sends;return send_ok;}
};
struct Audio{
 int queued=0;
 std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue(){
  if(!queued)return {};
  --queued;return std::make_unique<AudioStreamPacket>();
 }
};
struct Application{
 struct CellularAudioSend {
  std::shared_ptr<Protocol> protocol;
  std::unique_ptr<AudioStreamPacket> packet;
  uint32_t protocol_generation,interaction_generation;
 };
 std::shared_ptr<Protocol> protocol_=std::make_shared<Protocol>();
 std::atomic<uint32_t> protocol_generation_{1},interaction_generation_{1};
 std::atomic<bool> interaction_stopped_{false},voice_enrollment_active_{false};
 std::atomic<int> external_voice_preview_status_{0};
 Audio audio_service_;
 int event_group_=0,failures=0;
 bool cellular_audio_in_flight_=false;
 TaskHandle_t cellular_audio_task_=nullptr;
 std::mutex cellular_audio_mutex_;
 CellularAudioSend cellular_audio_send_{};
 std::function<void()> completion;
 void QueueCellularAudioSend();
 void RunCellularAudioSend();
 void HandleCellularAudioSendFailure(){++failures;}
 void Schedule(std::function<void()> callback){assert(!completion);completion=std::move(callback);}
};
''' + methods + r'''
int main(){
 Application app;
 auto work=[] {try{worker();}catch(const WaitForMoreWork&){};};
 auto finish=[&] {auto run=std::exchange(app.completion,{});assert(run);run();};
 app.audio_service_.queued=2;
 app.QueueCellularAudioSend();app.QueueCellularAudioSend();
 assert(notification&&app.audio_service_.queued==1&&app.protocol_->sends==0);
 work();assert(app.protocol_->sends==1&&app.cellular_audio_in_flight_);finish();
 assert(send_events==1&&!app.cellular_audio_in_flight_);
 app.QueueCellularAudioSend();++app.protocol_generation_;
 work();finish();assert(app.protocol_->sends==1&&app.failures==0&&send_events==2);
 app.protocol_->send_ok=false;app.audio_service_.queued=1;
 app.QueueCellularAudioSend();work();finish();
 assert(app.protocol_->sends==2&&app.failures==1);
}
''')

    def test_persistent_portal_limits_idle_wifi_power_saving(self):
        source = (ROOT / 'project_components/esp-wifi-connect/wifi_station.cc').read_text()
        method = queue_function(source, 'void WifiStation::SetPowerSaveLevel(')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <cassert>
#define ESP_LOGI(...) ((void)0)
#define ESP_ERROR_CHECK(status) assert((status)==0)
enum class WifiPowerSaveLevel{LOW_POWER,BALANCED,PERFORMANCE};
enum wifi_ps_type_t{WIFI_PS_MAX_MODEM,WIFI_PS_MIN_MODEM,WIFI_PS_NONE};
wifi_ps_type_t applied;
int esp_wifi_set_ps(wifi_ps_type_t mode){applied=mode;return 0;}
struct WifiStation{bool persistent_ap_=false;void SetPowerSaveLevel(WifiPowerSaveLevel);};
''' + method + r'''
int main(){WifiStation station;
 station.SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);assert(applied==WIFI_PS_MAX_MODEM);
 station.persistent_ap_=true;
 station.SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);assert(applied==WIFI_PS_MIN_MODEM);
 station.SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);assert(applied==WIFI_PS_NONE);
}
''')


if __name__ == '__main__':
    unittest.main()
