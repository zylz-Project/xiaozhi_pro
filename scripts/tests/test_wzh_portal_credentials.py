"""Exercise checked Wi-Fi writes and the production power-loss recovery journal."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT=Path(__file__).resolve().parents[2]

class PortalCredentialTest(unittest.TestCase):
    def test_write_failure_and_power_loss_restore_previous_credentials(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source=(ROOT/'project_components/esp-wifi-connect/ssid_manager.cc').read_text()
        backup=queue_function(source,'struct PortalBackup')+';'
        names=['SaveToNvsChecked','WritePortalBackup','ClearPortalBackup','RecoverPortalBackup',
               'SavePortalTransaction','AddSsidChecked','RestoreListChecked','ClearChecked']
        methods='\n'.join(queue_function(source,'bool SsidManager::'+name+'(') for name in names)
        program=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#define ESP_LOGE(...) ((void)0)
#define NVS_NAMESPACE "wifi"
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define MAX_WIFI_SSID_COUNT 10
constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1;using nvs_handle_t=int;
constexpr uint32_t kPortalBackupVersion=0x314b4257;constexpr char kPortalBackupKey[]="portal_wbak";
'''+backup+r'''
std::map<std::string,std::vector<char>> disk,staged;int failed_writes=0;bool fail_commit=false;
int nvs_open(const char*,int,nvs_handle_t* h){*h=1;staged=disk;return ESP_OK;}
void nvs_close(nvs_handle_t){}
int nvs_set_blob(nvs_handle_t,const char*k,const void*v,size_t n){if(failed_writes>0){--failed_writes;return 2;}staged[k]={static_cast<const char*>(v),static_cast<const char*>(v)+n};return 0;}
int nvs_get_blob(nvs_handle_t,const char*k,void*v,size_t*n){auto it=disk.find(k);if(it==disk.end())return ESP_ERR_NVS_NOT_FOUND;if(*n<it->second.size())return 2;*n=it->second.size();std::memcpy(v,it->second.data(),*n);return 0;}
int nvs_set_str(nvs_handle_t h,const char*k,const char*v){return nvs_set_blob(h,k,v,std::strlen(v)+1);}
int nvs_erase_key(nvs_handle_t,const char*k){return staged.erase(k)?0:ESP_ERR_NVS_NOT_FOUND;}
int nvs_commit(nvs_handle_t){if(fail_commit)return 2;disk=staged;return 0;}
std::string value(const std::string& k){auto x=disk.at(k);return std::string(x.data());}
struct SsidItem{std::string ssid,password;};
struct SsidManager{
 std::mutex mutex_;std::vector<SsidItem> ssid_list_;
 bool SaveToNvsChecked();bool WritePortalBackup(const std::vector<SsidItem>&);bool ClearPortalBackup();
 bool RecoverPortalBackup();bool SavePortalTransaction(const std::vector<SsidItem>&);
 bool AddSsidChecked(const std::string&,const std::string&);
 bool RestoreListChecked(const std::vector<SsidItem>&);bool ClearChecked();
};
'''+methods+r'''
int main(){
 SsidManager m;m.ssid_list_={{"home","old-pass"}};assert(m.SaveToNvsChecked());
 failed_writes=1;assert(!m.AddSsidChecked("new","bad"));
 assert(m.ssid_list_.size()==1&&m.ssid_list_[0].password=="old-pass");
 assert(value("ssid")=="home"&&value("password")=="old-pass");
 assert(m.AddSsidChecked("home","new-pass"));assert(value("password")=="new-pass");
 assert(m.RestoreListChecked({{"home","old-pass"}}));assert(value("password")=="old-pass");
 fail_commit=true;assert(!m.ClearChecked());fail_commit=false;
 assert(m.ssid_list_.size()==1&&value("ssid")=="home");
 // Model an interrupted multi-key update after a durable backup was saved.
 assert(m.WritePortalBackup(m.ssid_list_));disk["ssid"]={'b','a','d',0};disk["password"]={'x',0};
 SsidManager reboot;reboot.ssid_list_={{"bad","x"}};assert(reboot.RecoverPortalBackup());
 assert(value("ssid")=="home"&&value("password")=="old-pass"&&!disk.count(kPortalBackupKey));
 assert(reboot.ssid_list_.size()==1&&reboot.ssid_list_[0].ssid=="home");
 assert(reboot.ClearChecked()&&reboot.ssid_list_.empty()&&disk.count("ssid")==0);
}
'''
        with tempfile.TemporaryDirectory() as d:
            cpp=Path(d)/'credentials.cc';binary=Path(d)/'credentials';cpp.write_text(program)
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(binary)],check=True,timeout=5)
