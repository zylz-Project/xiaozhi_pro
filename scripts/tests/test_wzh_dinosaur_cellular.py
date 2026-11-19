"""Host checks for dinosaur ML307 configuration and network lifecycle."""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DINO = ROOT / 'main/boards/wzh_dinosaur'


class DinosaurCellularTest(unittest.TestCase):
    def test_dinosaur_uses_ml307_at_and_keeps_its_board_family(self):
        config = json.loads((DINO / 'config.json').read_text())
        settings = config['builds'][0]['sdkconfig_append']
        self.assertIn('CONFIG_ESP_MODEM_USE_PPP_MODE=n', settings)
        self.assertIn('CONFIG_LWIP_PPP_SUPPORT=n', settings)
        self.assertFalse(any(s.startswith('CONFIG_WZH_DINOSAUR_ML307=') for s in settings))
        self.assertEqual(config['type'], 'wzh-dinosaur')
        board_config = (DINO / 'config.h').read_text()
        self.assertRegex(board_config, r'#define DINOSAUR_ENABLE_4G [01]')
        self.assertNotIn('DINOSAUR_4G_USE_PPP', board_config)
        board_source = (DINO / 'wzh_dinosaur.cc').read_text()
        self.assertIn('cellular_.GetNetwork() : WifiBoard::GetNetwork()', board_source)
        self.assertIn('CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC=y', settings)
        self.assertIn('CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=y', settings)
        kconfig = (ROOT / 'main/Kconfig.projbuild').read_text()
        self.assertIn('depends on BOARD_TYPE_WZH_CRAWLPANDA || BOARD_TYPE_WZH_TAILREDPANDA', kconfig)
        self.assertRegex(kconfig, r'if BOARD_TYPE_WZH_DINOSAUR\s+'
                                  r'config WZH_DINOSAUR_AT_DIAGNOSTIC\s+bool\s+default y')
        self.assertRegex(kconfig, r'config BOARD_TYPE_WZH_DINOSAUR\s+bool "WZH Dinosaur"\s+'
                                  r'depends on IDF_TARGET_ESP32S3\s+'
                                  r'select ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT')
        self.assertIn('config WZH_DINOSAUR_ML307_DTR_GPIO', kconfig)
        self.assertNotIn('WZH_DINOSAUR_4G_PPP', kconfig)
        self.assertNotIn('WZH_DINOSAUR_PPP', kconfig)

    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_pdp_address_validation(self):
        from test_wzh_action_queue import queue_function
        source = (DINO / 'dinosaur_cellular.cc').read_text()
        method = queue_function(source, 'bool ValidPdpAddress(')
        program = r'''
#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <string>
''' + method + r'''
int main(){assert(ValidPdpAddress("10.72.165.124"));
 assert(ValidPdpAddress("2001:db8::1"));
 assert(!ValidPdpAddress("0.0.0.0"));assert(!ValidPdpAddress("::"));
 assert(!ValidPdpAddress(""));assert(!ValidPdpAddress("10.0.0.999"));
 assert(!ValidPdpAddress("not-an-address"));}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cc').write_text(program)
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     str(path / 'test.cc'), '-o', str(path / 'test')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / 'test')], check=True, timeout=5)

    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_voice_network_waits_for_selected_transport(self):
        from test_wzh_action_queue import queue_function
        source = (ROOT / 'main/boards/common/wzh/voice_tts/voice_http.cc').read_text()
        method = queue_function(source, 'bool VoiceHttpNetworkAvailable()')
        program = r'''
#include <cassert>
struct Board {bool available=false,system=true,cellular=false;
 static Board& GetInstance(){static Board b;return b;}
 bool IsNetworkAvailable(){return available;}
 bool HasSystemNetwork(){return system;}
 bool UsesCellularNetwork(){return cellular;}};
''' + method + r'''
int main(){auto& board=Board::GetInstance();
 assert(!VoiceHttpNetworkAvailable());
 board.available=true;assert(VoiceHttpNetworkAvailable());
 board.cellular=true;board.system=false;assert(VoiceHttpNetworkAvailable());
 board.available=false;assert(!VoiceHttpNetworkAvailable());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cc').write_text(program)
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     str(path / 'test.cc'), '-o', str(path / 'test')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / 'test')], check=True, timeout=5)

    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_wifi_events_are_replayed_once_after_selection(self):
        from test_wzh_action_queue import queue_function
        source = (DINO / 'wzh_dinosaur.cc').read_text()
        callback = queue_function(source, 'void SetNetworkEventCallback(').replace(' override {', ' {', 1)
        select = queue_function(source, 'void SelectNetwork(bool detected)')
        switch = queue_function(source, 'bool SelectNetworkTransport(bool cellular)')
        switch = switch.replace(' override {', ' {', 1)
        start = queue_function(source, 'void StartNetwork()').replace(' override {', ' {', 1)
        deadline = queue_function(source, 'void StartBootNetworkDeadline()').replace('WzhDinosaur', 'Dinosaur')
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#define DINOSAUR_ENABLE_4G 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define TAG "test"
constexpr int ESP_OK=0;
struct Timer {void(*callback)(void*);void* arg;};using esp_timer_handle_t=Timer*;
struct esp_timer_create_args_t {void(*callback)(void*)=nullptr;void* arg=nullptr;const char* name=nullptr;};
Timer* last_timer=nullptr;int64_t timer_delay=0;
int esp_timer_create(esp_timer_create_args_t* a,Timer** t){*t=last_timer=new Timer{a->callback,a->arg};return ESP_OK;}
int esp_timer_start_once(Timer*,int64_t delay){timer_delay=delay;return ESP_OK;}
void esp_timer_delete(Timer* t){delete t;}
enum class NetworkEvent {Connected,Disconnected,WifiConfigModeEnter,Connecting,ModemErrorInitFailed,ModemErrorNoSim,ModemErrorRegDenied};
enum {kDeviceStateIdle,kDeviceStateActivating,kDeviceStateUpgrading};
using NetworkEventCallback=std::function<void(NetworkEvent,const std::string&)>;
struct WifiBoard { NetworkEventCallback callback;
 void SetNetworkEventCallback(NetworkEventCallback cb){callback=std::move(cb);}
 void StartNetwork(){} };
struct WifiManager { bool connected=false;static WifiManager& GetInstance(){static WifiManager w;return w;}
 bool IsConnected(){return connected;}std::string GetSsid(){return "home";}
 int stopped=0,started=0;void StopStation(){++stopped;}void StartStation(){++started;} };
int esp_wifi_disconnects=0;void esp_wifi_disconnect(){++esp_wifi_disconnects;};
struct Application {std::vector<std::function<void()>> pending;
 int state=kDeviceStateIdle,stops=0,resets=0;
 static Application& GetInstance(){static Application a;return a;}
 int GetDeviceState(){return state;}
 void StopInteraction(){Schedule([this]{++stops;});}
 void ResetProtocol(){Schedule([this]{++resets;});}
 void Schedule(std::function<void()> f){pending.push_back(std::move(f));}
 void Run(){auto work=std::move(pending);pending.clear();for(auto& f:work)f();}};
namespace wzh { enum class OnboardingEvent{kNetworkLost}; void NotifyOnboarding(OnboardingEvent){} }
struct Power {bool IsShuttingDown(){return false;}};
struct Console {void Start(){} void Stop(){}};
struct Controller {void SetConversationActive(bool){}};
struct Settings { Settings(const std::string&, bool = false) {}
 std::string GetString(const std::string&, const std::string& d = "") { return d; }
 void SetString(const std::string&, const std::string&) {} };
std::string saved_preference;
struct Dinosaur:WifiBoard {
 enum class NetworkChoice:uint8_t{kProbing,kWifi,kCellular};
 std::atomic<NetworkChoice> network_choice_{NetworkChoice::kProbing};
 std::atomic<bool> boot_selection_pending_{true};esp_timer_handle_t boot_network_timer_=nullptr;
 std::atomic<bool> cellular_probe_requested_{false};
 std::atomic<bool> wifi_setup_deferred_{true},wifi_connected_reported_{false};
 std::atomic<bool> cellular_connected_reported_{false};
 NetworkEventCallback cloud_network_callback_;Power power_;Console web_console_;Controller controller_;
 struct CellularStub { bool paused=false,absent=false,ready=false;std::function<void(bool)> detected;
  NetworkEventCallback events;
  void SetPaused(bool p) { paused=p; } bool Absent() const { return absent; }
  bool IsReady() const { return ready; }
  bool Start(NetworkEventCallback cb, std::function<void(bool)> cb2) {
   events=std::move(cb);detected=std::move(cb2);return true; }
  void Detect(bool ok){absent=!ok;detected(ok);}
  void Emit(NetworkEvent e,const std::string& data=""){events(e,data);} };
 CellularStub cellular_;
 bool CellularPaused() const { return cellular_.paused; }
 bool UsesCellularNetwork() const { return network_choice_.load()==NetworkChoice::kCellular; }
 bool IsNetworkProbing() const { return network_choice_.load()==NetworkChoice::kProbing; }
 bool HasModemTransport() const { return !IsNetworkProbing()&&!cellular_.Absent(); }
 static void SaveTransportPreference(const char*) {}
 static std::string LoadTransportPreference() { return saved_preference; }
 int setup=0;void CompleteDeferredWifiSetup(){++setup;}
''' + callback + '\n' + select + '\n' + switch + '\n' + deadline + '\n' + start + r'''
};
int main(){
 auto& wifi=WifiManager::GetInstance();auto& app=Application::GetInstance();
 Dinosaur wifi_board;int connected=0,disconnected=0;
 wifi_board.SetNetworkEventCallback([&](NetworkEvent e,const std::string&){
   if(e==NetworkEvent::Connected)++connected;
   if(e==NetworkEvent::Disconnected)++disconnected;});
 wifi.connected=true;wifi_board.callback(NetworkEvent::Connected,"home");assert(connected==0);
 wifi_board.SelectNetwork(false);app.Run();assert(connected==1&&wifi_board.setup==1);
 wifi_board.callback(NetworkEvent::Connected,"home");assert(connected==1);
 wifi.connected=false;wifi_board.callback(NetworkEvent::Disconnected,"");assert(disconnected==1);
 wifi.connected=true;wifi_board.callback(NetworkEvent::Connected,"home");assert(connected==2);
 Dinosaur cell;int cell_events=0;
 cell.SetNetworkEventCallback([&](NetworkEvent,const std::string&){++cell_events;});
 cell.callback(NetworkEvent::Connected,"home");cell.SelectNetwork(true);app.Run();
 cell.callback(NetworkEvent::Disconnected,"");assert(cell_events==0&&cell.setup==1);
 // Runtime transport switch contract: probing blocks, same-transport is a
 // no-op success, and the modem is paused only while Wi-Fi is selected.
 Dinosaur probing;assert(!probing.SelectNetworkTransport(true));
 Dinosaur settled;settled.SelectNetwork(false);app.Run();
 assert(!settled.UsesCellularNetwork());
 assert(settled.SelectNetworkTransport(false));
 assert(settled.SelectNetworkTransport(true)&&settled.IsNetworkProbing());
 app.Run();assert(settled.UsesCellularNetwork()&&!settled.CellularPaused()&&app.stops==1&&app.resets==1);
 // Selecting 4G also disconnects any stale STA association left over from a
 // station restart so it cannot hijack the cloud link.
 assert(wifi.stopped>=1&&esp_wifi_disconnects==1);
 assert(settled.SelectNetworkTransport(false)&&settled.IsNetworkProbing());
 app.Run();assert(!settled.UsesCellularNetwork()&&settled.CellularPaused()&&app.stops==2&&app.resets==2);
 Dinosaur absent;absent.SelectNetwork(false);app.Run();absent.cellular_.absent=true;
 assert(!absent.SelectNetworkTransport(true));absent.cellular_.Detect(true);
 app.Run();assert(absent.IsNetworkProbing());app.Run();assert(absent.UsesCellularNetwork());
 Dinosaur cancelled;cancelled.SelectNetwork(false);app.Run();cancelled.cellular_.absent=true;
 assert(!cancelled.SelectNetworkTransport(true));assert(cancelled.SelectNetworkTransport(false));
 cancelled.cellular_.Detect(true);app.Run();assert(!cancelled.UsesCellularNetwork()&&!cancelled.IsNetworkProbing());
 Dinosaur concurrent;int replayed=0;
 concurrent.SetNetworkEventCallback([&](NetworkEvent e,const std::string&){replayed+=e==NetworkEvent::Connected;});
 concurrent.SelectNetwork(false);concurrent.callback(NetworkEvent::Connected,"home");app.Run();assert(replayed==1);
 // A modem can finish registration while the boot probe is still running.
 // Its Connected event is suppressed during kProbing and must be replayed
 // exactly once when the board commits to 4G.
 Dinosaur boot;int boot_connected=0;
 boot.SetNetworkEventCallback([&](NetworkEvent e,const std::string&){boot_connected+=e==NetworkEvent::Connected;});
 boot.StartNetwork();assert(timer_delay==5000000);boot.cellular_.Detect(true);app.Run();
 assert(boot.IsNetworkProbing());boot.cellular_.ready=true;
 boot.cellular_.Emit(NetworkEvent::Connected,"4G");app.Run();assert(boot_connected==1);
 boot.cellular_.Emit(NetworkEvent::Connected,"4G");assert(boot_connected==1);
 boot.cellular_.Emit(NetworkEvent::Disconnected,"");assert(boot_connected==1);
 boot.cellular_.ready=true;boot.cellular_.Emit(NetworkEvent::Connected,"4G");assert(boot_connected==2);
 // A detected modem that never registers must yield to Wi-Fi at the deadline.
 Dinosaur timeout;timeout.StartNetwork();timeout.cellular_.Detect(true);app.Run();
 assert(timeout.IsNetworkProbing());last_timer->callback(last_timer->arg);app.Run();app.Run();
 assert(!timeout.UsesCellularNetwork()&&!timeout.IsNetworkProbing()&&timeout.CellularPaused());
 // Late registration and the old deadline cannot switch the chosen network.
 timeout.cellular_.ready=true;timeout.cellular_.Emit(NetworkEvent::Connected,"4G");
 assert(!timeout.UsesCellularNetwork());
 assert(timeout.SelectNetworkTransport(true));last_timer->callback(last_timer->arg);app.Run();app.Run();
 assert(timeout.UsesCellularNetwork());
 saved_preference="wifi";Dinosaur preferred_wifi;preferred_wifi.StartNetwork();app.Run();
 assert(!preferred_wifi.IsNetworkProbing()&&!preferred_wifi.UsesCellularNetwork());
 saved_preference="cellular";Dinosaur preferred_cell;preferred_cell.StartNetwork();
 last_timer->callback(last_timer->arg);app.Run();app.Run();
 assert(!preferred_cell.UsesCellularNetwork()&&saved_preference=="cellular");
 saved_preference="";
 // A runtime switch to an already-registered modem also replays Connected,
 // while leaving 4G clears the report latch so a later switch can replay it.
 Dinosaur runtime;int runtime_connected=0;
 runtime.SetNetworkEventCallback([&](NetworkEvent e,const std::string&){runtime_connected+=e==NetworkEvent::Connected;});
 WifiManager::GetInstance().connected=false;
 runtime.SelectNetwork(false);app.Run();runtime.cellular_.ready=true;
 assert(runtime.SelectNetworkTransport(true));app.Run();assert(runtime_connected==1);
 assert(runtime.SelectNetworkTransport(false));app.Run();assert(runtime_connected==1);
 assert(runtime.SelectNetworkTransport(true));app.Run();assert(runtime_connected==2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cc').write_text(program)
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     str(path / 'test.cc'), '-o', str(path / 'test')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / 'test')], check=True, timeout=5)

    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_pins_retries_and_disconnect_reconnect(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            for name in ('driver', 'esp_adc', 'freertos'):
                (p / name).mkdir()
            (p / 'sdkconfig.h').write_text('#define CONFIG_WZH_DINOSAUR_ML307 1\n')
            (p / 'driver/gpio.h').write_text('''#pragma once
using gpio_num_t=int;
#define GPIO_NUM_NC -1
''' + '\n'.join(f'#define GPIO_NUM_{i} {i}' for i in range(49)) + '''
#define GPIO_IS_VALID_GPIO(n) ((n)>=0 && (n)<=48 && !((n)>=22 && (n)<=25))
#define GPIO_IS_VALID_OUTPUT_GPIO(n) (GPIO_IS_VALID_GPIO(n) && (n)!=46)
''')
            for name in ('driver/ledc.h', 'driver/spi_master.h', 'esp_adc/adc_oneshot.h'):
                (p / name).write_text('#pragma once\n')
            (p / 'freertos/FreeRTOS.h').write_text('''#pragma once
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
using TickType_t = unsigned;
#define pdMS_TO_TICKS(n) (n)
''')
            (p / 'freertos/task.h').write_text('''#pragma once
int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*);
void vTaskDelay(int); void vTaskDelete(void*);
inline void taskYIELD(){}
''')
            (p / 'freertos/event_groups.h').write_text('''#pragma once
#include "FreeRTOS.h"
#include <functional>
using EventBits_t = unsigned;
#define BIT0 (1u << 0)
#define BIT1 (1u << 1)
#define BIT2 (1u << 2)
#define BIT3 (1u << 3)
#define BIT4 (1u << 4)
#define BIT5 (1u << 5)
struct EventGroupState { EventBits_t bits = 0; };
using EventGroupHandle_t = EventGroupState*;
extern std::function<void()> event_group_wait_hook;
inline EventGroupHandle_t xEventGroupCreate() { return new EventGroupState(); }
inline EventBits_t xEventGroupClearBits(EventGroupHandle_t h, EventBits_t bits) {
    EventBits_t old = h->bits; h->bits &= ~bits; return old;
}
inline EventBits_t xEventGroupSetBits(EventGroupHandle_t h, EventBits_t bits) {
    h->bits |= bits; return h->bits;
}
inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t h, EventBits_t bits,
                                       int clear, int, TickType_t) {
    if (event_group_wait_hook) { auto hook = event_group_wait_hook; event_group_wait_hook = {}; hook(); }
    EventBits_t got = h->bits & bits; if (clear) h->bits &= ~bits; return got;
}
inline void vEventGroupDelete(EventGroupHandle_t h) { delete h; }
''')
            (p / 'esp_log.h').write_text('''#pragma once
template<class... T> void log_stub(T...) {}
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
#define ESP_LOGD(...) log_stub(__VA_ARGS__)
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
''')
            (p / 'http.h').write_text('''#pragma once
#include <cstddef>
#include <string>
#include "network_error.h"
class Http {
public:
 virtual ~Http()=default;
 virtual void SetTimeout(int)=0;
 virtual void SetHeader(const std::string&,const std::string&)=0;
 virtual void SetContent(std::string&&)=0;
 virtual void SetKeepAlive(bool)=0;
 virtual NetworkResult<> Open(const std::string&,const std::string&)=0;
 virtual void Close()=0;
 virtual NetworkResult<int> Read(char*,size_t)=0;
 virtual NetworkResult<int> Write(const char*,size_t)=0;
 virtual NetworkResult<int> GetStatusCode()=0;
 virtual std::string GetResponseHeader(const std::string&) const=0;
 virtual size_t GetBodyLength()=0;
 virtual std::string ReadAll()=0;
protected:
 NetworkResult<> Fail(NetworkError e){last_error_=e;return std::unexpected(e);}
 template<class T> NetworkResult<T> FailValue(NetworkError e){last_error_=e;return std::unexpected(e);}
 NetworkError last_error_{};
};
''')
            (p / 'board.h').write_text('''#pragma once
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include "mqtt.h"
#include "http.h"
enum class NetworkEvent {ModemDetecting,ModemErrorInitFailed,Connecting,Connected,Disconnected,ModemErrorNoSim,ModemErrorRegDenied};
using NetworkEventCallback=std::function<void(NetworkEvent,const std::string&)>;
class Tcp {
public:
 virtual ~Tcp()=default;
 virtual NetworkResult<> Connect(const std::string&,int)=0;
 virtual void Disconnect()=0;
 virtual int Send(const std::string&)=0;
 virtual void OnStream(std::function<void(const std::string&)> cb){stream_callback_=std::move(cb);}
 virtual void OnDisconnected(std::function<void()> cb){disconnect_callback_=std::move(cb);}
 bool connected() const{return connected_;}
protected:
 std::function<void(const std::string&)> stream_callback_;
 std::function<void()> disconnect_callback_;
 bool connected_=false;
 NetworkResult<> Fail(NetworkError e){last_error_=e;return std::unexpected(e);}
 NetworkError last_error_{};
};
struct NetworkInterface;
class WebSocket {
public:
 WebSocket(NetworkInterface*,int) {}
};
class Udp {
public:
 virtual ~Udp()=default;
 virtual NetworkResult<> Connect(const std::string&,int)=0;
 virtual void Disconnect()=0;
 virtual int Send(const std::string&)=0;
 virtual void OnMessage(std::function<void(const std::string&)> cb){message_callback_=std::move(cb);}
 bool connected() const {return connected_;}
protected:
 std::function<void(const std::string&)> message_callback_;
 bool connected_=false;
 NetworkResult<> Fail(NetworkError e){last_error_=e;return std::unexpected(e);}
 NetworkError last_error_{};
};
struct NetworkInterface {
 virtual ~NetworkInterface()=default;
 virtual std::unique_ptr<Http> CreateHttp(int=-1)=0;
 virtual std::unique_ptr<Tcp> CreateTcp(int=-1)=0;
 virtual std::unique_ptr<Tcp> CreateSsl(int=-1)=0;
 virtual std::unique_ptr<Udp> CreateUdp(int=-1)=0;
 virtual std::unique_ptr<Mqtt> CreateMqtt(int=-1)=0;
 virtual std::unique_ptr<WebSocket> CreateWebSocket(int=-1)=0;
};
''')
            (p / 'network_interface.h').write_text('#pragma once\n#include "board.h"\n')
            (p / 'mqtt.h').write_text('''#pragma once
#include <functional>
#include <string>
#include <utility>
#include "network_error.h"
class Mqtt {
public:
 virtual ~Mqtt()=default;
 void SetKeepAlive(int seconds){keep_alive_seconds_=seconds;}
 virtual NetworkResult<> Connect(const std::string,int,const std::string,const std::string,const std::string)=0;
 virtual void Disconnect()=0;
 virtual bool Publish(const std::string,const std::string,int=0)=0;
 virtual bool Subscribe(const std::string,int=0)=0;
 virtual bool Unsubscribe(const std::string)=0;
 virtual bool IsConnected()=0;
 virtual void OnConnected(std::function<void()> cb){on_connected_callback_=std::move(cb);}
 virtual void OnDisconnected(std::function<void()> cb){on_disconnected_callback_=std::move(cb);}
 virtual void OnMessage(std::function<void(const std::string&,const std::string&)> cb){on_message_callback_=std::move(cb);}
 virtual void OnError(std::function<void(const std::string&)> cb){on_error_callback_=std::move(cb);}
protected:
 int keep_alive_seconds_=120;
 std::function<void(const std::string&,const std::string&)> on_message_callback_;
 std::function<void()> on_connected_callback_;
 std::function<void()> on_disconnected_callback_;
 std::function<void(const std::string&)> on_error_callback_;
 NetworkResult<> Fail(NetworkError e){last_error_=e;return std::unexpected(e);}
 NetworkError last_error_{};
};
''')
            (p / 'at_uart.h').write_text('''#pragma once
#include <cstring>
#include <functional>
#include <list>
#include <string>
#include <vector>
#include "at_error.h"
struct AtArgumentValue {int int_value=0;std::string string_value;enum class Type {Int,String};Type type=Type::Int;};
using Urc=std::function<void(const std::string&,const std::vector<AtArgumentValue>&)>;
using UrcCallback=Urc;
extern std::function<void()> event_group_wait_hook;
struct AtUart {
 std::list<Urc> callbacks;
 static int pdp_queries,fail_pdp,at_queries,pdp_dials;static bool query_ok,drop_during_query,require_dial,defer_mqttpub;
 static std::string address;static std::vector<std::string> sent_commands;static bool mipstate_active;static bool hold_mipopen;
 static std::string last_data_command,last_data_payload;static std::function<void()> deferred_mqttpub;
 auto RegisterUrcCallback(Urc cb){return callbacks.insert(callbacks.end(),std::move(cb));}
 void UnregisterUrcCallback(std::list<Urc>::iterator it){callbacks.erase(it);}
 int GetCmeErrorCode() const{return 0;}
 int GetBaudRate() const{return 921600;}
 void Emit(const std::string& command,std::vector<AtArgumentValue> args){
  for(auto& cb:callbacks)cb(command,args);
 }
 AtResult SendCommand(const std::string& cmd,size_t=3000,bool=true){
  sent_commands.push_back(cmd);
  if(cmd=="AT")++at_queries;
  if(cmd=="AT+MIPCALL?"){++pdp_queries;if(!query_ok)return std::unexpected(AtError{AtErrc::Timeout});
    if(require_dial)return {};
    if(pdp_queries>fail_pdp)Emit("MIPCALL",{{1,""},{1,""},{0,address}});
    if(drop_during_query)Emit("MIPCALL",{{1,""},{0,""}});
  }else if(cmd=="AT+MIPCALL=1,1"){++pdp_dials;require_dial=false;}
  else if(cmd=="AT+CSQ"){Emit("CSQ",{{23,""}});}
  else if(cmd.rfind("AT+MIPSTATE=",0)==0){int id=std::stoi(cmd.substr(12));AtArgumentValue state;state.type=AtArgumentValue::Type::String;state.string_value=mipstate_active?"CONNECTED":"INITIAL";Emit("MIPSTATE",{{id,""},{0,""},{0,""},{0,""},state});}
  else if(cmd.rfind("AT+MIPCLOSE=",0)==0){int id=std::stoi(cmd.substr(12));Emit("MIPCLOSE",{{id,""}});}
  else if(cmd.rfind("AT+MIPOPEN=",0)==0){if(!hold_mipopen){int id=std::stoi(cmd.substr(11));Emit("MIPOPEN",{{id,""},{0,""}});}}
  else if(cmd.rfind("AT+MIPSEND=",0)==0){auto comma=cmd.find(',',11);auto next=cmd.find(',',comma+1);int id=std::stoi(cmd.substr(11,comma-11));int len=std::stoi(cmd.substr(comma+1,next-comma-1));Emit("MIPSEND",{{id,""},{len,""}});}
  else if(cmd.rfind("AT+MQTTDISC=",0)==0){int id=std::stoi(cmd.substr(12));AtArgumentValue t;t.type=AtArgumentValue::Type::String;t.string_value="conn";Emit("MQTTURC",{t,{id,""},{2,""}});}
  else if(cmd.rfind("AT+MQTTCONN=",0)==0){int eq=cmd.find('=');int comma=cmd.find(',',eq);int id=std::stoi(cmd.substr(eq+1,comma-eq-1));AtArgumentValue t;t.type=AtArgumentValue::Type::String;t.string_value="conn";Emit("MQTTURC",{t,{id,""},{2,""}});Emit("MQTTURC",{t,{id,""},{0,""}});}
  return {};
 }
 AtResult SendCommandWithData(const std::string& cmd,size_t timeout,bool add_crlf,const char* data,size_t length){
  last_data_command=cmd;last_data_payload.assign(data?data:"",data?length:0);
  if(cmd.rfind("AT+MQTTPUB=",0)==0){
   int eq=cmd.find('=');int c1=cmd.find(',',eq);int id=std::stoi(cmd.substr(eq+1,c1-eq-1));
   int q1=cmd.find('"',c1),q2=cmd.find('"',q1+1);std::string topic=cmd.substr(q1+1,q2-q1-1);
   int qos=std::stoi(cmd.substr(q2+2));
   int c5=cmd.rfind(',');int payload_len=std::stoi(cmd.substr(c5+1));
   size_t remaining=2+topic.size()+payload_len+(qos>0?2:0),length_bytes=1;
   for(size_t n=remaining;n>=128;n/=128)++length_bytes;
   auto emit_publish=[this,id,remaining,length_bytes]{
    Emit("MQTTPUB",{{id,""},{1,""},{static_cast<int>(1+length_bytes+remaining),""}});};
   if(defer_mqttpub){
    defer_mqttpub=false;deferred_mqttpub=emit_publish;
    event_group_wait_hook=[](){auto hook=std::move(deferred_mqttpub);deferred_mqttpub={};if(hook)hook();};
   }else emit_publish();
  }
  return SendCommand(cmd,timeout,add_crlf);
 }
 std::string DecodeHex(const std::string& data){std::string out;for(size_t i=0;i+1<data.size();i+=2){auto v=[](char c){return c>='0'&&c<='9'?c-'0':(c>='A'&&c<='F'?c-'A'+10:c-'a'+10);};out.push_back(static_cast<char>((v(data[i])<<4)|v(data[i+1])));}return out;}
 void DecodeHexAppend(std::string& out,const char* data,size_t length){out+=DecodeHex(std::string(data,length));}
 std::string EncodeHex(const std::string& data){std::string out;EncodeHexAppend(out,data.data(),data.size());return out;}
 void EncodeHexAppend(std::string& out,const char* data,size_t length){static const char hex[]="0123456789ABCDEF";for(size_t i=0;i<length;++i){out.push_back(hex[(data[i]&0xF0)>>4]);out.push_back(hex[data[i]&0x0F]);}}
};
''')
            (p / 'at_modem.h').write_text('''#pragma once
#include "board.h"
#include "at_uart.h"
#include "driver/gpio.h"
#include <memory>
enum class NetworkStatus {Ready,ErrorInsertPin,ErrorRegistrationDenied,ErrorTimeout,Error};
struct AtModem: NetworkInterface {
 static int detects,waits,tx,rx,baud,timeout,sleep_disable_calls;static bool always_absent,registration_error,no_uart;static AtModem* current;std::function<void(bool)> callback;
 std::shared_ptr<AtUart> uart=std::make_shared<AtUart>();
 std::shared_ptr<AtUart> GetAtUart(){return no_uart?nullptr:uart;}
 static AtValue<std::unique_ptr<AtModem>> Detect(gpio_num_t tx_pin,gpio_num_t rx_pin,gpio_num_t,int rate,int timeout_ms) {
  tx=tx_pin;rx=rx_pin;baud=rate;timeout=timeout_ms;
  if(++detects==1||always_absent)return std::unexpected(AtError{AtErrc::Timeout});auto m=std::make_unique<AtModem>();current=m.get();return m;}
 void OnNetworkStateChanged(std::function<void(bool)> f){callback=f;}
 bool SetSleepMode(bool enable){assert(!enable);++sleep_disable_calls;return true;}
 NetworkStatus WaitForNetworkReady(int) {++waits;if(registration_error && waits==1)return NetworkStatus::Error;return waits<=2?NetworkStatus::ErrorInsertPin:NetworkStatus::Ready;}
 std::unique_ptr<Http> CreateHttp(int) override{return {};}
 std::unique_ptr<Tcp> CreateTcp(int) override{return {};}
 std::unique_ptr<Tcp> CreateSsl(int) override{return {};}
 std::unique_ptr<Udp> CreateUdp(int) override{return {};}
 std::unique_ptr<Mqtt> CreateMqtt(int) override{return {};}
 std::unique_ptr<WebSocket> CreateWebSocket(int) override{return {};}
};
''')
            (p / 'test.cc').write_text('''
#include <cassert>
#include <vector>
#include "at_modem.h"
int AtUart::pdp_queries=0;int AtUart::fail_pdp=0;int AtUart::at_queries=0;int AtUart::pdp_dials=0;bool AtUart::query_ok=true;bool AtUart::drop_during_query=false;bool AtUart::require_dial=false;bool AtUart::defer_mqttpub=false;std::string AtUart::address="10.72.165.124";std::vector<std::string> AtUart::sent_commands;bool AtUart::mipstate_active=false;bool AtUart::hold_mipopen=false;std::string AtUart::last_data_command,AtUart::last_data_payload;std::function<void()> AtUart::deferred_mqttpub;
bool AtModem::always_absent=false;bool AtModem::registration_error=false;bool AtModem::no_uart=false;int AtModem::detects=0;int AtModem::waits=0;int AtModem::tx=-1;int AtModem::rx=-1;int AtModem::baud=0;int AtModem::timeout=0;int AtModem::sleep_disable_calls=0;AtModem* AtModem::current=nullptr;
void(*task)(void*)=nullptr;void* task_arg=nullptr;int online_delays=0,fast_retries=0;bool fail_task=false,stop_on_no_ip=false,drop_pdp=false;std::function<void()> recheck_action;std::function<void()> event_group_wait_hook;
int xTaskCreate(void(*f)(void*),const char*,int,void* p,int,void*){if(fail_task)return 0;task=f;task_arg=p;return 1;}
struct Stop{};
void vTaskDelay(int ms){if(stop_on_no_ip&&ms==10000&&AtUart::pdp_queries>=1)throw Stop{};if(ms==500&&!AtModem::current)++fast_retries;if(ms==1000&&AtModem::current&&AtModem::waits>=3){if(++online_delays==1){if(recheck_action)recheck_action();else if(drop_pdp){AtUart::require_dial=true;for(auto& cb:AtModem::current->uart->callbacks)cb("MIPCALL",{{1,""},{0,""}});}else AtModem::current->callback(false);}else throw Stop{};}}
void vTaskDelete(void*){}
#include "dinosaur_cellular.cc"
#include "dinosaur_ml307_network.cc"
int main(){
 DinosaurCellular cell;std::vector<NetworkEvent> events;
 auto cb=[&](NetworkEvent e,const std::string&){events.push_back(e);};
#if DINOSAUR_ML307_TX_GPIO == 12 && DINOSAUR_ML307_RX_GPIO == 11 && CONFIG_WZH_DINOSAUR_ML307_DTR_GPIO == -1
 fail_task=true;assert(!cell.Start(cb)&&!task);fail_task=false;
 assert(cell.Start(cb));assert(task);assert(!cell.IsReady()&&cell.GetNetwork()==nullptr);
 int replacement_events=0;
 assert(cell.Start([&](NetworkEvent e,const std::string& data){++replacement_events;cb(e,data);}));
 try{task(task_arg);}catch(const Stop&){}
 assert(AtModem::detects==2&&AtModem::waits==4&&cell.IsReady()&&cell.GetNetwork()!=nullptr);
 assert(replacement_events>0);
 assert(AtModem::sleep_disable_calls==4);
 assert(fast_retries==1);
 assert(AtModem::tx==12&&AtModem::rx==11&&AtModem::baud==921600&&AtModem::timeout==2000);
 int connected=0,disconnected=0,connecting=0,nosim=0;
 for(auto e:events){connected+=e==NetworkEvent::Connected;disconnected+=e==NetworkEvent::Disconnected;connecting+=e==NetworkEvent::Connecting;nosim+=e==NetworkEvent::ModemErrorNoSim;}
 assert(connected==2&&disconnected==1&&connecting==2&&nosim==1);
 // Missing hardware selects Wi-Fi after two bounded probes without a modem error.
 AtModem::current=nullptr;AtModem::always_absent=true;AtModem::detects=0;
 DinosaurCellular absent;events.clear();int choice=-1;
 assert(absent.Start(cb,[&](bool detected){choice=detected?1:0;}));
 task(task_arg);assert(choice==0&&AtModem::detects==2);
 for(auto e:events)assert(e!=NetworkEvent::ModemErrorInitFailed);
 // A detected module without a UART must not announce success or start a retry loop.
 AtModem::always_absent=false;AtModem::detects=1;AtModem::no_uart=true;
 DinosaurCellular missing_uart;choice=-1;events.clear();
 assert(missing_uart.Start(cb,[&](bool detected){choice=detected?1:0;}));
 task(task_arg);assert(choice==0&&missing_uart.Absent());AtModem::no_uart=false;
 // A responding modem is selected before registration completes.
 AtModem::always_absent=false;AtModem::detects=1;AtModem::waits=0;online_delays=0;
 DinosaurCellular present;choice=-1;
 assert(present.Start(cb,[&](bool detected){choice=detected?1:0;}));
 try{task(task_arg);}catch(const Stop&){}
 assert(choice==1&&present.IsReady());
 // A successful registration is insufficient without a valid PDP address.
 AtModem::detects=1;AtModem::waits=0;online_delays=0;AtUart::pdp_queries=0;
 AtUart::address="0.0.0.0";stop_on_no_ip=true;DinosaurCellular no_ip;events.clear();choice=-1;
 assert(no_ip.Start(cb,[&](bool detected){choice=detected?1:0;}));
 try{task(task_arg);}catch(const Stop&){}
 assert(choice==1&&!no_ip.IsReady()&&AtUart::pdp_queries>=3);
 for(auto e:events)assert(e!=NetworkEvent::Connected);
 // A transient missing PDP response is retried before publishing readiness.
 AtModem::detects=1;AtModem::waits=0;online_delays=0;AtUart::pdp_queries=0;
 AtUart::address="10.72.165.124";AtUart::fail_pdp=2;stop_on_no_ip=false;
 DinosaurCellular delayed_ip;events.clear();
 assert(delayed_ip.Start(cb));try{task(task_arg);}catch(const Stop&){}
 assert(delayed_ip.IsReady()&&AtUart::pdp_queries>=3);AtUart::fail_pdp=0;
 // An explicit PDP deactivation raises a disconnect and triggers recovery.
 AtModem::detects=1;AtModem::waits=0;online_delays=0;AtUart::pdp_queries=0;AtUart::pdp_dials=0;
 AtUart::require_dial=false;drop_pdp=true;
 DinosaurCellular lost_pdp;events.clear();
 assert(lost_pdp.Start(cb));try{task(task_arg);}catch(const Stop&){}
 connected=disconnected=connecting=0;for(auto e:events){connected+=e==NetworkEvent::Connected;disconnected+=e==NetworkEvent::Disconnected;connecting+=e==NetworkEvent::Connecting;}
 assert(connected==2&&disconnected==1&&connecting==2&&lost_pdp.IsReady()&&AtUart::pdp_dials==1);
 drop_pdp=false;AtUart::require_dial=false;
 // A cloud/audio write failure only requests a PDP confirmation. A healthy
 // registration stays ready and must not emit Disconnected.
 AtModem::detects=1;AtModem::waits=3;online_delays=0;AtUart::pdp_queries=0;events.clear();
 DinosaurCellular recheck;recheck_action=[&]{recheck.RequestRecheck();};
 assert(recheck.Start(cb));try{task(task_arg);}catch(const Stop&){}
 connected=disconnected=0;for(auto e:events){connected+=e==NetworkEvent::Connected;disconnected+=e==NetworkEvent::Disconnected;}
 assert(connected==1&&disconnected==0&&recheck.IsReady()&&AtUart::pdp_queries>=2);recheck_action={};
 // Only a failed PDP confirmation demotes the still-registered modem.
 AtModem::detects=1;AtModem::waits=3;online_delays=0;AtUart::pdp_queries=0;
 AtUart::address="10.72.165.124";stop_on_no_ip=true;events.clear();
 DinosaurCellular recheck_failed;
 recheck_action=[&]{AtUart::address="0.0.0.0";recheck_failed.RequestRecheck();};
 assert(recheck_failed.Start(cb));try{task(task_arg);}catch(const Stop&){}
 connected=disconnected=0;for(auto e:events){connected+=e==NetworkEvent::Connected;disconnected+=e==NetworkEvent::Disconnected;}
 assert(connected==1&&disconnected==1&&!recheck_failed.IsReady());
 recheck_action={};AtUart::address="10.72.165.124";stop_on_no_ip=false;
 // A generic registration error performs one bounded AT responsiveness probe.
 AtModem::detects=1;AtModem::waits=0;online_delays=0;AtUart::at_queries=0;
 AtModem::registration_error=true;DinosaurCellular diagnostic;
 assert(diagnostic.Start(cb));try{task(task_arg);}catch(const Stop&){}
 assert(AtUart::at_queries==1&&diagnostic.IsReady());AtModem::registration_error=false;
 // A PDP loss arriving while the address is being checked must not report ready.
 AtModem::detects=1;AtModem::waits=2;online_delays=0;AtUart::pdp_queries=0;
 AtUart::drop_during_query=true;stop_on_no_ip=true;DinosaurCellular racing_loss;events.clear();
 assert(racing_loss.Start(cb));try{task(task_arg);}catch(const Stop&){}
 assert(!racing_loss.IsReady());for(auto e:events)assert(e!=NetworkEvent::Connected);
 AtUart::drop_during_query=false;stop_on_no_ip=false;
 // The board adapter replaces only UDP with the ML307 inline HEX form. The
 // send is a single AT command (no '>' prompt / binary payload phase), and
 // three-argument disconn plus FIFO overflow never issue AT from the URC task.
 AtModem::detects=1;AtModem::waits=3;online_delays=1;events.clear();
 DinosaurCellular transport;
 assert(transport.Start(cb));try{task(task_arg);}catch(const Stop&){}
 assert(transport.IsReady());auto* network=transport.GetNetwork();assert(network);
 AtUart::sent_commands.clear();AtUart::mipstate_active=false;
 auto udp=network->CreateUdp(2);assert(udp);
 std::vector<std::string> datagrams;
 udp->OnMessage([&](const std::string& data){datagrams.push_back(data);});
 assert(udp->Connect("192.0.2.10",9000));
 for(const auto& sent:AtUart::sent_commands)assert(sent.rfind("AT+MIPSTATE",0)!=0);
 assert(!udp->Connect("bad,host",9000));
 auto was_sent=[](const std::string& command){
   for(const auto& sent:AtUart::sent_commands)if(sent==command)return true;return false;};
 assert(was_sent("AT+MIPCFG=\\\"encoding\\\",2,1,1"));
 assert(was_sent("AT+MIPCFG=\\\"ssl\\\",2,0,0"));
 assert(was_sent("AT+MIPOPEN=2,\\\"UDP\\\",\\\"192.0.2.10\\\",9000,,0"));
 assert(udp->Send(std::string("\\x01\\x33\\x45\\x37",4))==4);
 assert(AtUart::sent_commands.back()=="AT+MIPSEND=2,4,\\\"01334537\\\"\\r\\n");
 auto arg_int=[](int value){AtArgumentValue a;a.type=AtArgumentValue::Type::Int;a.int_value=value;return a;};
 auto arg_string=[](const std::string& value){AtArgumentValue a;a.type=AtArgumentValue::Type::String;a.string_value=value;return a;};
 AtModem::current->uart->Emit("MIPURC",{arg_string("rudp"),arg_int(2),arg_int(2),arg_string("AABB")});
 assert(datagrams.size()==1&&datagrams[0]==std::string("\\xAA\\xBB",2));
 size_t commands_before=AtUart::sent_commands.size();
 AtModem::current->uart->Emit("MIPURC",{arg_string("disconn"),arg_int(2),arg_int(1)});
 assert(AtUart::sent_commands.size()==commands_before&&udp->Send("x")==-1);
 AtUart::mipstate_active=true;assert(udp->Connect("192.0.2.10",9000));
 assert(was_sent("AT+MIPCLOSE=2"));
 assert(udp->Send(std::string(731,'x'))==-1);
 commands_before=AtUart::sent_commands.size();
 AtModem::current->uart->Emit("FIFO_OVERFLOW",{});
 assert(AtUart::sent_commands.size()==commands_before&&udp->Send("x")==-1);
 // TCP/TLS uses the same quoted inline HEX form; the upstream driver omits
 // the quotes and times out before the modem can acknowledge the send.
 auto tcp=network->CreateTcp(3);assert(tcp);
 std::vector<std::string> stream;
 tcp->OnStream([&](const std::string& data){stream.push_back(data);});
 assert(tcp->Connect("192.0.2.10",443));
 assert(was_sent("AT+MIPCFG=\\\"encoding\\\",3,1,1"));
 assert(was_sent("AT+MIPCFG=\\\"ssl\\\",3,0,0"));
 assert(was_sent("AT+MIPOPEN=3,\\\"TCP\\\",\\\"192.0.2.10\\\",443,15,0"));
 assert(tcp->Send("abc")==3);
 assert(AtUart::sent_commands.back()=="AT+MIPSEND=3,3,\\\"616263\\\"\\r\\n");
 AtModem::current->uart->Emit("MIPURC",{arg_string("rtcp"),arg_int(3),arg_int(3),arg_string("414243")});
 assert(stream.size()==1&&stream[0]=="ABC");
 auto ssl=network->CreateSsl(4);assert(ssl);
 assert(ssl->Connect("192.0.2.10",443));
 assert(was_sent("AT+MSSLCFG=\\\"auth\\\",0,0"));
 assert(was_sent("AT+MIPCFG=\\\"ssl\\\",4,1,0"));
 assert(was_sent("AT+MIPOPEN=4,\\\"TCP\\\",\\\"192.0.2.10\\\",443,15,0"));
 // A command timeout can be followed by a late +MIPOPEN success. The
 // adapter must remember that modem-side socket so teardown closes it.
 {
  AtUart::hold_mipopen=true;
  {
   auto late_udp=network->CreateUdp(1);assert(late_udp);
   assert(!late_udp->Connect("192.0.2.10",9000));
   AtUart::hold_mipopen=false;AtUart::sent_commands.clear();
   AtModem::current->uart->Emit("MIPOPEN",{arg_int(1),arg_int(0)});
  }
  assert(was_sent("AT+MIPCLOSE=1"));
  AtUart::hold_mipopen=true;
  {
   auto late_tcp=network->CreateTcp(0);assert(late_tcp);
   assert(!late_tcp->Connect("192.0.2.10",443));
   AtUart::hold_mipopen=false;AtUart::sent_commands.clear();
   AtModem::current->uart->Emit("MIPOPEN",{arg_int(0),arg_int(0)});
  }
  assert(was_sent("AT+MIPCLOSE=0"));
 }
 // A MQTTDISC URC that arrives around MQTTCONN must not fail the new session.
 auto mqtt=network->CreateMqtt(5);assert(mqtt);
 int mqtt_connected=0;mqtt->OnConnected([&]{++mqtt_connected;});
 assert(!mqtt->Connect("broker,example",1883,"client","user","pass"));
 assert(mqtt->Connect("broker.example",1883,"client","user","pass"));
 assert(mqtt->IsConnected()&&mqtt_connected==1);
 assert(!mqtt->Publish("bad,topic","abc"));
 assert(!mqtt->Publish("topic",std::string(1025,'x')));
 assert(mqtt->Publish("topic",std::string(1024,'x')));
 assert(AtUart::last_data_command=="AT+MQTTPUB=5,\\\"topic\\\",0,0,0,1024");
 assert(AtUart::last_data_payload==std::string(1024,'x'));
 assert(mqtt->Publish("status","abc"));
 assert(AtUart::last_data_command=="AT+MQTTPUB=5,\\\"status\\\",0,0,0,3");
 assert(AtUart::last_data_payload=="abc");
 // Older firmware can put +MQTTPUB after the final OK. Publish() must wait a
 // short bounded interval before declaring the packet length invalid.
 AtUart::defer_mqttpub=true;
 assert(mqtt->Publish("late","abc"));
 assert(AtUart::last_data_command=="AT+MQTTPUB=5,\\\"late\\\",0,0,0,3");
 assert(mqtt->IsConnected());
 // Incoming URCs are not MQTTPUB and may be larger than 1024 bytes. Two
 // uneven fragments must be reassembled exactly once.
 std::vector<std::string> mqtt_messages;
 mqtt->OnMessage([&](const std::string& topic,const std::string& payload){mqtt_messages.push_back(topic+":"+payload);});
 AtModem::current->uart->Emit("MQTTURC",{arg_string("publish"),arg_int(5),arg_int(11),
   arg_string("status"),arg_int(5),arg_int(2),arg_string("4142")});
 assert(mqtt_messages.empty());
 AtModem::current->uart->Emit("MQTTURC",{arg_string("publish"),arg_int(5),arg_int(11),
   arg_string("status"),arg_int(5),arg_int(3),arg_string("434445")});
 assert(mqtt_messages.size()==1&&mqtt_messages[0]=="status:ABCDE");
 std::string large(1500,'a'),large_hex;
 AtModem::current->uart->EncodeHexAppend(large_hex,large.data(),large.size());
 AtModem::current->uart->Emit("MQTTURC",{arg_string("publish"),arg_int(5),arg_int(12),
   arg_string("status"),arg_int(1500),arg_int(1500),arg_string(large_hex)});
 assert(mqtt_messages.size()==2&&mqtt_messages[1]=="status:"+large);
 // A malformed fragment must not be delivered or poison the next message.
 AtModem::current->uart->Emit("MQTTURC",{arg_string("publish"),arg_int(5),arg_int(13),
   arg_string("status"),arg_int(4),arg_int(2),arg_string("4142")});
 AtModem::current->uart->Emit("MQTTURC",{arg_string("publish"),arg_int(5),arg_int(13),
   arg_string("status"),arg_int(4),arg_int(2),arg_string("414")});
 assert(mqtt_messages.size()==2);
#else
 assert(!cell.Start(cb));assert(!task&&events.size()==1&&events[0]==NetworkEvent::ModemErrorInitFailed);
#endif
}
''')
            for tx, rx, dtr in [(-1, -1, -1), (11, 11, -1), (17, 11, -1), (12, 11, 11), (12, 11, -1)]:
                binary = p / 'test'
                (p / 'mqtt_impl.cc').write_text('#include "dinosaur_ml307_mqtt.cc"\n')
                (p / 'http_impl.cc').write_text('#include "dinosaur_ml307_http.cc"\n')
                cmd = ['g++', '-std=c++17', '-Wall', '-Wextra', '-I', str(p), '-I', str(DINO),
                       '-I', str(ROOT / 'scripts/tests/support'),
                       '-DDINOSAUR_ENABLE_4G=1',
                       f'-DDINOSAUR_ML307_TX_GPIO={tx}',
                       f'-DDINOSAUR_ML307_RX_GPIO={rx}',
                       f'-DCONFIG_WZH_DINOSAUR_ML307_DTR_GPIO={dtr}',
                       str(p / 'test.cc'), str(p / 'mqtt_impl.cc'), str(p / 'http_impl.cc'),
                       '-pthread', '-o', str(binary)]
                result = subprocess.run(cmd, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stderr)
            # Wi-Fi mode must exclude the cellular implementation entirely.
            (p / 'wifi.cc').write_text('''
#include "dinosaur_cellular.cc"
#if DINOSAUR_ENABLE_4G != 0
#error Wi-Fi mode did not take effect
#endif
int main() { return 0; }
''')
            subprocess.run(['g++', '-std=c++17', '-I', str(p), '-I', str(DINO),
                            '-DDINOSAUR_ENABLE_4G=0', str(p / 'wifi.cc'),
                            '-o', str(p / 'wifi')], check=True, capture_output=True, text=True)
            subprocess.run([str(p / 'wifi')], check=True)
            (p / 'at_diagnostic.cc').write_text('''
#include "config.h"
#if DINOSAUR_ENABLE_4G != 1
#error AT diagnostic must select cellular
#endif
int main() { return 0; }
''')
            subprocess.run(['g++', '-std=c++17', '-I', str(p), '-I', str(DINO),
                            '-DCONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC=1',
                            str(p / 'at_diagnostic.cc'), '-o', str(p / 'at_diagnostic')],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(p / 'at_diagnostic')], check=True)


if __name__ == '__main__':
    unittest.main()
