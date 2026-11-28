"""Verify WZH boots into station mode and keeps BLE as the provisioning entry."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT=Path(__file__).resolve().parents[2]

class WifiStartupTest(unittest.TestCase):
    def test_wzh_legacy_entry_points_do_not_touch_station(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source=(ROOT/'main/boards/common/wifi_board.cc').read_text()
        methods='\n'.join(queue_function(source, name) for name in (
            'void WifiBoard::OnWifiConnectTimeout(',
            'void WifiBoard::StartWifiConfigMode(',
            'void WifiBoard::EnterWifiConfigMode('))
        program=r'''
#define CONFIG_BOARD_TYPE_WZH_CRAWLPANDA 1
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 0
#define CONFIG_BOARD_TYPE_WZH_TAILREDPANDA 0
#define ESP_LOGI(...) ((void)0)
struct WifiBoard {
 static void OnWifiConnectTimeout(void*); void StartWifiConfigMode();
 void EnterWifiConfigMode();
};
'''+methods+r'''
int main(){ WifiBoard board; board.OnWifiConnectTimeout(&board);
 board.StartWifiConfigMode(); board.EnterWifiConfigMode(); }
'''
        with tempfile.TemporaryDirectory() as d:
            cpp=Path(d)/'entry.cc';binary=Path(d)/'entry';cpp.write_text(program)
            result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(binary)],check=True,timeout=5)

    def test_wzh_boot_starts_station_without_web_provisioning(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        method=queue_function((ROOT/'main/boards/common/wifi_board.cc').read_text(),
                              'void WifiBoard::StartNetwork(')
        program=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#define CONFIG_BOARD_TYPE_WZH_CRAWLPANDA 1
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 0
#define CONFIG_BOARD_TYPE_WZH_TAILREDPANDA 0
#define CONFIG_WZH_SYSTEM_PROMPTS 1
#define ESP_MAC_WIFI_STA 0
#define ESP_OK 0
int esp_read_mac(uint8_t*,int){return -1;}
enum class WifiEvent{Scanning,Connecting,Connected,Disconnected,ConfigModeEnter,ConfigModeExit};
enum class NetworkEvent{Scanning,Connecting,Connected,Disconnected,WifiConfigModeEnter,WifiConfigModeExit};
struct WifiManagerConfig{
 std::string ssid_prefix,language,station_hostname;
 bool show_sleep_config=false,persistent_portal=false,show_ota_config=false,allow_config_ap=true;
 int station_scan_min_interval_seconds=0,station_scan_max_interval_seconds=0;
};
struct WifiManager{
 bool active=true;int station_starts=0;bool allow_config_ap=true;
 static WifiManager& GetInstance(){static WifiManager w;return w;}
 void Initialize(const WifiManagerConfig& config){allow_config_ap=config.allow_config_ap;}
 void SetEventCallback(std::function<void(WifiEvent,const std::string&)>){}
 void StartPersistentPortal(){}bool IsPersistentPortalActive(){return active;}
 void StartStation(){++station_starts;}bool IsConnected(){return false;}
 std::string GetApSsid(){return "pet";}std::string GetApWebUrl(){return "http://192.168.4.1";}
};
struct SsidManager{
 bool saved=false;static SsidManager& GetInstance(){static SsidManager s;return s;}
 std::vector<int> GetSsidList(){return saved?std::vector<int>{1}:std::vector<int>{};}
};
constexpr int kDeviceStateIdle=1;
struct Application{
 int state=0;static Application& GetInstance(){static Application a;return a;}
 void SetDeviceState(int value){state=value;}
 void Schedule(std::function<void()> callback){callback();}
 void Alert(const char*,const char*,const char*,std::string){}
};
namespace Lang{namespace Strings{constexpr auto CONNECT_TO_HOTSPOT="ssid",ACCESS_VIA_BROWSER="url",WIFI_CONFIG_MODE="setup";}}
namespace wzh{int prompts=0,ready=0;void RequestWifiSetupPrompt(){++prompts;}
void MarkStartupNetworkReady(){++ready;}}
struct WifiBoard{
 std::string wifi_ssid_prefix_="WZH";int prompts=0,fallbacks=0;
 void OnNetworkEvent(NetworkEvent,const std::string& = ""){}
 void OnNetworkStackReady(){}
 void EnterWifiConfigMode(){++prompts;}void TryWifiConnect(){++fallbacks;}
 void StartNetwork();
};
'''+method+r'''
int main(){
 // WZH always boots into station mode: no :80 provisioning prompt or fallback,
 // even with no saved network; BLE owns credential setup.
 WifiBoard first;first.StartNetwork();assert(wzh::prompts==0&&wzh::ready==1&&first.fallbacks==0);
 assert(Application::GetInstance().state==kDeviceStateIdle);
 SsidManager::GetInstance().saved=true;
 WifiBoard configured;configured.StartNetwork();assert(wzh::prompts==0&&wzh::ready==2&&configured.fallbacks==0);
 WifiManager::GetInstance().active=false;
 WifiBoard legacy;legacy.StartNetwork();assert(wzh::prompts==0&&wzh::ready==3&&legacy.fallbacks==0);
 assert(WifiManager::GetInstance().station_starts==3);
 assert(!WifiManager::GetInstance().allow_config_ap);
}
'''
        with tempfile.TemporaryDirectory() as d:
            cpp=Path(d)/'startup.cc';binary=Path(d)/'startup';cpp.write_text(program)
            result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(binary)],check=True,timeout=5)

    def test_disabled_ap_does_not_stop_station_and_other_boards_keep_ap(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        method=queue_function((ROOT/'project_components/esp-wifi-connect/wifi_manager.cc').read_text(),
                              'void WifiManager::StartConfigAp(')
        program=r'''
#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
enum class WifiEvent { Disconnected, ConfigModeEnter };
struct Station { int stops=0; void Stop(){++stops;} };
struct Portal {
 int starts=0;
 void SetSsidPrefix(const std::string&){} void SetLanguage(const std::string&){}
 void SetShowOtaConfig(bool){} void SetShowSleepConfig(bool){}
 void OnExitRequested(std::function<void()> callback){exit=callback;}
 bool Start(){++starts;return true;}
 std::function<void()> exit;
};
struct WifiManager {
 std::mutex mutex_; bool initialized_=true,config_mode_active_=false;
 bool persistent_portal_active_=false,station_active_=true;
 struct Config { bool allow_config_ap=true,show_ota_config=false,show_sleep_config=false;
  std::string ssid_prefix,language; } config_;
 Station station; Station* station_=&station;
 Portal portal; Portal* config_ap_=&portal;
 std::function<void()> before_config_ap_;
 void NotifyEvent(WifiEvent){} void StopConfigAp(){}
 void StartConfigAp();
};
'''+method+r'''
int main(){
 WifiManager wzh; wzh.config_.allow_config_ap=false; wzh.config_ap_=nullptr;
 wzh.StartConfigAp(); assert(wzh.station_active_ && wzh.station.stops==0);
 WifiManager legacy; legacy.StartConfigAp();
 assert(!legacy.station_active_ && legacy.station.stops==1);
 assert(legacy.config_mode_active_ && legacy.portal.starts==1);
}
'''
        with tempfile.TemporaryDirectory() as d:
            cpp=Path(d)/'disabled.cc';binary=Path(d)/'disabled';cpp.write_text(program)
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(binary)],check=True,timeout=5)
