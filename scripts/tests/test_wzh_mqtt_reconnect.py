"""Exercise the production MQTT timer against link loss and shutdown races."""
from pathlib import Path
import unittest

from test_wzh_action_queue import queue_function
import test_wzh_motion_output as output

ROOT = Path(__file__).resolve().parents[2]


class MqttReconnectTest(unittest.TestCase):
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
#include <functional>
#include <utility>
int stopped=0;void esp_timer_stop(void*){++stopped;}
struct Client {std::function<void()> connected;
 void OnConnected(std::function<void()> f){connected=std::move(f);}};
struct Protocol {Client client,*mqtt_=&client;std::atomic<bool> mqtt_connected_{false};
 void* reconnect_timer_=nullptr;std::function<void()> on_connected_;
 void Bind(){
''' + callback + r'''
 }};
int main(){Protocol p;int called=0;p.on_connected_=[&]{++called;};p.Bind();
 p.client.connected();assert(p.mqtt_connected_&&called==1&&stopped==1);}
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
        method = queue_function((ROOT / 'main/application.cc').read_text(),
                                'void Application::HandleNetworkConnectedEvent(')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <cassert>
#include <cstddef>
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
enum {kDeviceStateStarting,kDeviceStateWifiConfiguring,kDeviceStateIdle,kDeviceStateActivating};
int tasks=0;int xTaskCreate(void(*)(void*),const char*,int,void*,int,void**){++tasks;return 1;}
void vTaskDelete(void*){}
struct Display{int updates=0;void UpdateStatusBar(bool){++updates;}};
struct Board{bool online=false;Display display;static Board& GetInstance(){static Board b;return b;}
 bool IsNetworkAvailable(){return online;}Display* GetDisplay(){return &display;}};
struct Application{int state=kDeviceStateIdle;void* protocol_=nullptr;void* activation_task_handle_=nullptr;
 int GetDeviceState(){return state;}void SetDeviceState(int s){state=s;}void ActivationTask(){}
 void HandleNetworkConnectedEvent();};
''' + method + r'''
int main(){Application app;auto& board=Board::GetInstance();
 app.HandleNetworkConnectedEvent();assert(tasks==0&&app.state==kDeviceStateIdle&&board.display.updates==0);
 board.online=true;app.HandleNetworkConnectedEvent();
 assert(tasks==1&&app.state==kDeviceStateActivating&&board.display.updates==1);
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
 std::shared_ptr<std::atomic<bool>> alive_=std::make_shared<std::atomic<bool>>(true);
 void SetError(const char*){++errors;}bool IsMqttConnectedLocked();void ScheduleCellularRecheck();
 bool SendText(const std::string&);};
''' + connected + '\n' + recheck + '\n' + send + r'''
int main(){MqttProtocol p;assert(!p.SendText("test"));p.mqtt_=std::make_unique<Client>();
 assert(!p.SendText("test")&&p.mqtt_->publishes==0);p.mqtt_->connected=true;
 assert(p.SendText("test")&&p.mqtt_->publishes==1);
 std::atomic<bool> locked{false},release{false};std::thread connector([&]{
  std::lock_guard<std::mutex> hold(p.mqtt_mutex_);locked=true;while(!release.load())std::this_thread::yield();});
 while(!locked.load())std::this_thread::yield();
 assert(!p.SendText("test")&&p.mqtt_->publishes==1);release=true;connector.join();
 Board::GetInstance().cellular=true;p.mqtt_connected_=true;
 assert(!p.SendText("test")); // A Wi-Fi client must not be reused for 4G.
 p.mqtt_transport_cellular_=true;
 int queries=p.mqtt_->queries;assert(p.SendText("test")&&p.mqtt_->queries==queries);
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
struct Board {bool online=true,maintenance=false;static Board& GetInstance(){static Board b;return b;}
 bool IsNetworkAvailable()const{return online;}bool IsAudioMaintenanceActive(){return maintenance;}};
struct Application {int state=kDeviceStateIdle;std::function<void()> pending;
 static Application& GetInstance(){static Application a;return a;}int GetDeviceState(){return state;}
 void Schedule(std::function<void()>&& fn){pending=std::move(fn);}void Run(){auto fn=std::move(pending);pending={};fn();}};
struct Client {bool connected=false;bool IsConnected(){return connected;}};
struct MqttProtocol {void* event_group_handle_=nullptr;void* reconnect_timer_=nullptr;
 std::unique_ptr<Client> mqtt_;std::mutex mqtt_mutex_;std::atomic<bool> suspend_queued_{false};int suspends=0;
 std::shared_ptr<std::atomic<bool>> alive_=std::make_shared<std::atomic<bool>>(true);
 int connects=0;bool result=true;MqttProtocol();bool Start();void SuspendUnavailableMqtt(){++suspends;}
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
 assert(last_duration==5000000);b.online=true;assert(!fresh.Start());assert(last_duration==60000000);
}
''')


if __name__ == '__main__':
    unittest.main()
