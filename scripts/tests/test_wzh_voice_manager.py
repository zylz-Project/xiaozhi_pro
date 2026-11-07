"""Test the production voice manager against a simulated Ali account."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

class VoiceManagerTest(unittest.TestCase):
    def test_per_device_limit_and_guarded_management(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        code = r'''
#include "voice_manager.h"
#include "voice_names.h"
#include "voice_identity.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
using namespace voice_tts;
std::vector<std::string> remote;std::vector<VoiceName> labels;
std::string current="legacy",previous;int creates=0,deletes=0,reserved=-1;bool create_fail=false,fail_before_submit=false;
int esp_read_mac(unsigned char* m,int){unsigned char a[6]={1,2,3,4,5,6};std::memcpy(m,a,6);return 0;}
namespace voice_tts {
std::string LoadAliKey(){return "key";}
std::string LoadAliVoice(){return current;}
std::string LoadPreviousAliVoice(){return previous;}
bool SaveAliVoice(const std::string& id){previous=current;current=id;return true;}
bool LoadVoiceReservation(int& out){out=reserved;return true;}
bool SaveVoiceReservation(int value){reserved=value;return true;}
bool CommitAliVoice(const std::string& id){return SaveAliVoice(id);}
bool ForgetPreviousAliVoice(const std::string& id){if(previous==id)previous.clear();return true;}
bool IsValidVoiceName(const std::string& name){return !name.empty() && name.size()<=48;}
bool LoadVoiceNames(std::vector<VoiceName>& out){out=labels;return true;}
bool SetVoiceName(const std::string& id,const std::string& name){
 for(auto& e:labels)if(e.id==id){e.name=name;return true;}labels.push_back({id,name});return true;}
bool RemoveVoiceName(const std::string& id){labels.erase(std::remove_if(labels.begin(),labels.end(),[&](auto& x){return x.id==id;}),labels.end());return true;}
bool ListAliVoices(const std::string&,const std::string& prefix,const std::function<bool()>&,std::vector<std::string>& out){
 assert(prefix==VoicePrefixFromMac((const uint8_t*)"\1\2\3\4\5\6",6));out=remote;return true;}
bool DeleteAliVoice(const std::string&,const std::string&,const std::string& id,const std::function<bool()>&){
 ++deletes;remote.erase(std::find(remote.begin(),remote.end(),id));return true;}
bool CreateAliVoice(const std::string&,const int16_t*,size_t,const std::function<bool()>&,
 std::string& out,int*,const std::string& prefix,bool* ack,bool* submitted){++creates;if(submitted)*submitted=!fail_before_submit;if(create_fail){if(ack)*ack=false;return false;}if(ack)*ack=true;out="qwen-audio-3.1-tts-flash-"+prefix+"-1";remote.push_back(out);return true;}
}
int main(){std::string id;int16_t samples[1]={};auto no=[](){return false;};
 assert(EnrollManagedVoice(samples,1,"龙安欢",no,id)==VoiceOperation::kInvalid && creates==0);
 assert(EnrollManagedVoice(samples,1,"妈妈",no,id)==VoiceOperation::kOk && creates==1 && current==id);
 assert(RenameManagedVoice(id,"妈妈新声音")==VoiceOperation::kOk);
 assert(SelectKnownVoice(id,no)==VoiceOperation::kOk);
 assert(SelectKnownVoice("unknown",no)==VoiceOperation::kInvalid);
 assert(SelectKnownVoice(id,[]{return true;})==VoiceOperation::kCancelled);
 std::vector<ManagedVoice> all;assert(ListManagedVoices(all)==VoiceOperation::kOk && all.size()==kBuiltinVoiceCount+1);
 assert(DeleteManagedVoice("someone-else")==VoiceOperation::kInvalid && deletes==0);
 assert(DeleteManagedVoice(id)==VoiceOperation::kInvalid && deletes==0);
 assert(SelectManagedVoice("longanhuan_v3.1")==VoiceOperation::kOk);
 assert(DeleteManagedVoice(id)==VoiceOperation::kOk && deletes==1 && remote.empty() && previous.empty());
 remote.assign(10,"different");assert(EnrollManagedVoice(samples,1,"新",no,id)==VoiceOperation::kFull && creates==1);
 remote.clear();create_fail=true;fail_before_submit=true;
 assert(EnrollManagedVoice(samples,1,"新",no,id)==VoiceOperation::kNetwork && reserved==-1);
 fail_before_submit=false;assert(EnrollManagedVoice(samples,1,"新",no,id)==VoiceOperation::kNetwork && reserved==0);
 assert(EnrollManagedVoice(samples,1,"新",no,id)==VoiceOperation::kUncertain && creates==3);
 remote.push_back("created-after-timeout");assert(ListManagedVoices(all)==VoiceOperation::kOk && reserved==-1);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'esp_mac.h').write_text('#pragma once\n#define ESP_OK 0\n#define ESP_MAC_WIFI_STA 0\nint esp_read_mac(unsigned char*,int);\n');(p/'test.cc').write_text(code)
            voice=ROOT/'main/boards/common/wzh/voice_tts'
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-Wno-misleading-indentation','-I',str(p),'-I',str(voice),str(p/'test.cc'),
                str(voice/'voice_manager.cc'),'-o',str(p/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(p/'test')],check=True,timeout=10)
