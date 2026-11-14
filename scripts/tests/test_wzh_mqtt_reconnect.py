"""Exercise the production MQTT timer against link loss and shutdown races."""
from pathlib import Path
import unittest

from test_wzh_action_queue import queue_function
import test_wzh_motion_output as output

ROOT = Path(__file__).resolve().parents[2]


class MqttReconnectTest(unittest.TestCase):
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
 std::mutex channel_mutex_;std::shared_ptr<Udp> udp_;std::function<void()> on_audio_channel_closed_;
 std::shared_ptr<std::atomic<bool>> alive_=std::make_shared<std::atomic<bool>>(true);
 void SuspendUnavailableMqtt();
 std::function<void()> on_disconnected_;void Bind(){
''' + callback + r'''
 }};
''' + suspend + r'''
int main(){Protocol p;auto& b=Board::GetInstance();auto& app=Application::GetInstance();p.Bind();
 int closes=0;p.on_audio_channel_closed_=[&]{++closes;};p.udp_=std::make_shared<Udp>();
 p.mqtt_->disconnected();assert(timers==1&&delay==60000000);app.Run();assert(p.mqtt_->disconnects==0&&!p.udp_&&closes==1);
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
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <cassert>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
namespace Lang {namespace Strings {constexpr auto SERVER_ERROR="error";}}
struct Client {bool connected=false,publish_ok=true;int publishes=0;
 bool IsConnected(){return connected;}bool Publish(const std::string&,const std::string&){++publishes;return publish_ok;}};
struct MqttProtocol {std::string publish_topic_="topic";std::unique_ptr<Client> mqtt_;std::mutex mqtt_mutex_;int errors=0;
 void SetError(const char*){++errors;}bool SendText(const std::string&);};
''' + send + r'''
int main(){MqttProtocol p;assert(!p.SendText("test"));p.mqtt_=std::make_unique<Client>();
 assert(!p.SendText("test")&&p.mqtt_->publishes==0);p.mqtt_->connected=true;
 assert(p.SendText("test")&&p.mqtt_->publishes==1);
 std::atomic<bool> locked{false},release{false};std::thread connector([&]{
  std::lock_guard<std::mutex> hold(p.mqtt_mutex_);locked=true;while(!release.load())std::this_thread::yield();});
 while(!locked.load())std::this_thread::yield();
 assert(!p.SendText("test")&&p.mqtt_->publishes==1);release=true;connector.join();
 p.mqtt_->publish_ok=false;
 assert(!p.SendText("test")&&p.errors==1);}
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
