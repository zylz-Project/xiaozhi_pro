"""Exercise the production switch-and-rollback worker without Wi-Fi hardware."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT=Path(__file__).resolve().parents[2]

class PortalNetworkTest(unittest.TestCase):
    def test_first_boot_announces_provisioning_only_without_saved_networks(self):
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
 bool show_sleep_config=false,persistent_portal=false,show_ota_config=false;
 int station_scan_min_interval_seconds=0,station_scan_max_interval_seconds=0;
};
struct WifiManager{
 bool active=true;int station_starts=0;
 static WifiManager& GetInstance(){static WifiManager w;return w;}
 void Initialize(const WifiManagerConfig&){}
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
 void EnterWifiConfigMode(){++prompts;}void TryWifiConnect(){++fallbacks;}
 void StartNetwork();
};
'''+method+r'''
int main(){
 WifiBoard first;first.StartNetwork();assert(wzh::prompts==1&&wzh::ready==1&&first.fallbacks==0);
 assert(Application::GetInstance().state==kDeviceStateIdle);
 SsidManager::GetInstance().saved=true;
 WifiBoard configured;configured.StartNetwork();assert(wzh::prompts==1&&wzh::ready==2&&configured.fallbacks==0);
 WifiManager::GetInstance().active=false;
 WifiBoard legacy;legacy.StartNetwork();assert(wzh::prompts==1&&wzh::ready==3&&legacy.fallbacks==1);
}
'''
        with tempfile.TemporaryDirectory() as d:
            cpp=Path(d)/'startup.cc';binary=Path(d)/'startup';cpp.write_text(program)
            result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(binary)],check=True,timeout=5)

    def test_validate_save_connect_and_rollback(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        code=queue_function((ROOT/'project_components/esp-wifi-connect/wifi_manager.cc').read_text(),
                            'bool WifiManager::TestAndSavePortalNetwork(')
        program=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
int64_t clock_us=0;int64_t esp_timer_get_time(){return clock_us;}
void vTaskDelay(int ms){clock_us+=int64_t(ms)*1000;}
struct Netif{};Netif temporary;bool have_netif=true;
Netif* esp_netif_get_handle_from_ifkey(const char*){return have_netif?&temporary:nullptr;}
void esp_wifi_disconnect(){}
struct Item{std::string ssid,password;};
struct SsidManager{
 std::vector<Item>items{{"old","old-pass"}};bool save_ok=true,restored=false;
 static SsidManager& GetInstance(){static SsidManager instance;return instance;}
 std::vector<Item> GetSsidList(){return items;}
 bool AddSsidChecked(const std::string&s,const std::string&p){if(!save_ok)return false;items.insert(items.begin(),{s,p});return true;}
 bool RestoreListChecked(const std::vector<Item>& old){items=old;restored=true;return true;}
};
struct Portal{bool candidate_ok=true;int failure=15;bool ConnectToWifi(const std::string&,const std::string&){return candidate_ok;}int LastConnectFailureReason()const{return failure;}};
struct WifiManager{
 std::mutex mutex_;bool persistent_portal_active_=true;Portal portal;Portal*config_ap_=&portal;
 bool reconnect_ok=true;int starts=0,stops=0;bool connected=false;std::string current;
 std::atomic<int> portal_failure_reason_{0};
 void StopStation(){++stops;connected=false;}
 void StartStation(){++starts;connected=reconnect_ok;current=SsidManager::GetInstance().items.front().ssid;}
 bool IsConnected(){return connected;}std::string GetSsid(){return current;}
 bool TestAndSavePortalNetwork(const std::string&,const std::string&,const std::function<void()>&);
 int LastPortalFailureReason()const{return portal_failure_reason_.load();}
};
'''+code+r'''
int main(){
 auto& db=SsidManager::GetInstance();int accepted=0;
 WifiManager ok;assert(ok.TestAndSavePortalNetwork("new","new-pass",[&]{++accepted;}));
 assert(accepted==1&&db.items.front().ssid=="new"&&ok.starts==1&&ok.LastPortalFailureReason()==0);
 db.items={{"old","old-pass"}};accepted=0;WifiManager wrong;wrong.portal.candidate_ok=false;
 assert(!wrong.TestAndSavePortalNetwork("new","bad",[&]{++accepted;}));
 assert(accepted==0&&db.items.size()==1&&db.items[0].password=="old-pass"&&wrong.starts==1);
 assert(wrong.LastPortalFailureReason()==15);
 assert(!wrong.TestAndSavePortalNetwork("new","bad",{}));
 have_netif=false;assert(!wrong.TestAndSavePortalNetwork("new","bad",{})&&wrong.LastPortalFailureReason()==-2);
 have_netif=true;
 db.items={{"old","old-pass"}};db.save_ok=false;WifiManager fail_save;
 assert(!fail_save.TestAndSavePortalNetwork("new","new-pass",{}));
 assert(fail_save.LastPortalFailureReason()==-1);
 assert(db.items.size()==1&&db.items[0].password=="old-pass");db.save_ok=true;
 db.items={{"old","old-pass"}};db.restored=false;clock_us=0;WifiManager failed_reconnect;
 failed_reconnect.reconnect_ok=false;
 assert(!failed_reconnect.TestAndSavePortalNetwork("new","new-pass",{}));
 assert(failed_reconnect.LastPortalFailureReason()==-2);
 assert(db.restored&&db.items.size()==1&&db.items[0].ssid=="old"&&db.items[0].password=="old-pass");
 assert(failed_reconnect.stops==2&&failed_reconnect.starts==2);
}
'''
        with tempfile.TemporaryDirectory() as d:
            cpp=Path(d)/'network.cc';binary=Path(d)/'network';cpp.write_text(program)
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(binary)],check=True,timeout=5)
