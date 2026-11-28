"""Exercise the actual upload worker, ownership and busy behavior without hardware."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
class VoiceWebTest(unittest.TestCase):
    def test_worker_rejects_invalid_and_keeps_only_one_upload(self):
        if not shutil.which('g++') or not shutil.which('gcc'):
            self.skipTest('C/C++ compiler required')
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'freertos').mkdir()
            (p/'freertos/FreeRTOS.h').write_text('#pragma once\n#define pdPASS 1\n')
            (p/'freertos/task.h').write_text('#pragma once\nusing TaskHandle_t=void*;\nint xTaskCreate(void(*)(void*),const char*,int,void*,int,TaskHandle_t*);\nvoid vTaskDelete(void*);\n')
            (p/'esp_heap_caps.h').write_text('#pragma once\n#include <cstdlib>\n')
            (p/'application.h').write_text('''#pragma once
class Application { public: static Application& GetInstance(){static Application a;return a;}
bool IsVoiceEnrollmentActive()const{return false;}bool IsExternalVoicePreviewActive()const{return false;} };
''')
            (p/'board.h').write_text('#pragma once\nclass Board { public: static Board& GetInstance(){static Board b;return b;} bool IsAudioMaintenanceActive()const{return false;} };\n')
            (p/'test.cc').write_text(r'''
#include "voice_web.h"
#include "voice_names.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
void(*worker)(void*)=nullptr;void* worker_arg=nullptr;
bool fail_create=false;
int xTaskCreate(void(*f)(void*),const char*,int,void* a,int,void**){
 if(fail_create)return 0;
 worker=f;worker_arg=a;return 1;
}
void vTaskDelete(void*){}
size_t json_allocations=0;
void* CountJsonAllocation(size_t bytes){++json_allocations;return std::malloc(bytes);}
namespace voice_tts {
std::string recorded_name,current_voice;size_t recorded_samples=0;std::vector<std::string> actions;
bool list_online=true;
int list_requests=0;
std::string LoadAliVoice(){return current_voice;}
bool VoiceResetActive(){return false;}
bool IsValidVoiceName(const std::string& n){return !n.empty()&&n.find('\0')==std::string::npos;}
VoiceOperation EnrollManagedVoice(const int16_t* pcm,size_t n,const std::string& name,const std::function<bool()>& cancelled,std::string& id){
 if(cancelled())return VoiceOperation::kCancelled;
 assert(pcm[0]==1234);recorded_samples=n;recorded_name=name;id="id";return VoiceOperation::kOk;}
VoiceOperation ListManagedVoices(std::vector<ManagedVoice>& v){
 ++list_requests;
 if(!list_online)return VoiceOperation::kNetwork;
 v={{"id","name",false}};return VoiceOperation::kOk;
}
VoiceOperation RenameManagedVoice(const std::string&,const std::string&){return VoiceOperation::kOk;}
VoiceOperation DeleteManagedVoice(const std::string& id){actions.push_back("delete:"+id);return VoiceOperation::kOk;}
VoiceOperation SelectManagedVoice(const std::string& id){actions.push_back("select:"+id);current_voice=id;return VoiceOperation::kOk;}
}
int main(){using namespace voice_tts;VoiceWeb web;
 auto outcome=[&](uint32_t request,const char* status){
  cJSON* body=cJSON_CreateObject();web.AddStatus(body,false);const cJSON* value=nullptr;
  assert(!cJSON_GetObjectItem(body,"voice_catalog"));
  bool found=false;
  cJSON_ArrayForEach(value,cJSON_GetObjectItem(body,"voice_operations")) {
   if(cJSON_GetObjectItem(value,"request")->valuedouble==request) {
    found=!std::strcmp(cJSON_GetObjectItem(value,"status")->valuestring,status);break;
   }
  }
  cJSON_Delete(body);assert(found);
 };
 uint8_t bad[2]={1,'x'};assert(!web.Upload(bad,sizeof(bad)));
 const size_t n=16000*6;auto* data=(uint8_t*)std::malloc(1+3+n*2);
 data[0]=3;std::memcpy(data+1,"abc",3);data[4]=0xd2;data[5]=0x04;
 fail_create=true;
 assert(!web.Upload(data,1+3+n*2)&&!web.Busy()&&!worker);
 assert(data[0]==3&&!std::memcmp(data+1,"abc",3)&&data[4]==0xd2&&data[5]==0x04);
 fail_create=false;
 const auto upload=web.Upload(data,1+3+n*2);
 assert(upload&&web.BusyWithMutation());assert(!web.Command("list","",""));assert(worker);
 outcome(upload,"busy");
 worker(worker_arg);assert(recorded_samples==n&&recorded_name=="abc");outcome(upload,"ok");
 cJSON* root=cJSON_CreateObject();web.AddStatus(root);
 assert(!cJSON_IsTrue(cJSON_GetObjectItem(root,"voice_management_busy")));
 assert(!std::strcmp(cJSON_GetObjectItem(root,"voice_management_status")->valuestring,"ok"));
 assert(cJSON_GetArraySize(cJSON_GetObjectItem(root,"voice_catalog"))==2);
 auto* native=cJSON_GetArrayItem(cJSON_GetObjectItem(root,"voice_catalog"),0);
 assert(!std::strcmp(cJSON_GetObjectItem(native,"id")->valuestring,"xiaozhi-native"));
 assert(!std::strcmp(cJSON_GetObjectItem(native,"name")->valuestring,"原声"));cJSON_Delete(root);
 cJSON_Hooks hooks{CountJsonAllocation,std::free};
 cJSON_InitHooks(&hooks);
 auto* summary=cJSON_CreateObject();json_allocations=0;
 web.AddStatus(summary,false);
 const auto summary_allocations=json_allocations;
 root=cJSON_CreateObject();json_allocations=0;
 web.AddStatus(root);
 assert(json_allocations>summary_allocations);
 cJSON_DeleteItemFromObjectCaseSensitive(root,"voice_catalog");
 assert(cJSON_Compare(root,summary,true));
 cJSON_Delete(root);cJSON_Delete(summary);
 cJSON_InitHooks(nullptr);
 list_online=false;
 assert(web.Command("list","",""));assert(web.Busy()&&!web.BusyWithMutation());worker(worker_arg);
 outcome(upload,"ok"); // An unrelated list failure cannot replace the upload result.
 list_online=true;
 assert(!web.Cancel());
 auto* cancelled_data=(uint8_t*)std::malloc(1+3+n*2);
 cancelled_data[0]=3;std::memcpy(cancelled_data+1,"abc",3);
 const auto cancelled_upload=web.Upload(cancelled_data,1+3+n*2);
 assert(cancelled_upload&&cancelled_upload!=upload);assert(web.Cancel());
 root=cJSON_CreateObject();web.AddStatus(root);
 assert(cJSON_IsTrue(cJSON_GetObjectItem(root,"voice_management_busy")));
 assert(!cJSON_IsTrue(cJSON_GetObjectItem(root,"voice_management_cancellable")));cJSON_Delete(root);
 worker(worker_arg);root=cJSON_CreateObject();web.AddStatus(root);
 assert(!std::strcmp(cJSON_GetObjectItem(root,"voice_management_status")->valuestring,"cancelled"));
 cJSON_Delete(root);outcome(cancelled_upload,"cancelled");
 assert(!web.Command("unexpected","",""));assert(web.Command("list","",""));assert(!web.Cancel());worker(worker_arg);
 auto rename=web.Command("rename","id","new name");assert(rename);
 outcome(rename,"busy");list_online=false;
 worker(worker_arg);outcome(rename,"ok");
 root=cJSON_CreateObject();web.AddStatus(root);
 auto* last=cJSON_GetArrayItem(cJSON_GetObjectItem(root,"voice_catalog"),1);
 assert(!std::strcmp(cJSON_GetObjectItem(last,"name")->valuestring,"new name"));cJSON_Delete(root);
 current_voice="id";auto remove=web.Command("delete","id","");assert(remove&&remove!=rename);worker(worker_arg);
 assert(web.Command("list","",""));worker(worker_arg);outcome(rename,"ok");outcome(remove,"ok");
 outcome(upload,"ok");outcome(cancelled_upload,"cancelled");
 root=cJSON_CreateObject();web.AddStatus(root);
 assert(cJSON_GetArraySize(cJSON_GetObjectItem(root,"voice_catalog"))==1);cJSON_Delete(root);
 assert(actions.size()==2&&actions[0]=="select:longanhuan_v3.1"&&actions[1]=="delete:id");
 const int previous_lists=list_requests;
 const auto select=web.Command("select","longanhuan_v3.1","");assert(select);worker(worker_arg);
 outcome(select,"ok");assert(list_requests==previous_lists);
 assert(web.Command("list","",""));worker(worker_arg);assert(list_requests==previous_lists+1);
}
''')
            json=ROOT/'managed_components/espressif__cjson/cJSON';voice=ROOT/'main/boards/common/wzh/voice_tts'
            subprocess.run(['gcc','-c',str(json/'cJSON.c'),'-o',str(p/'json.o')],check=True)
            build=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-I',str(p),'-I',str(json),'-I',str(voice),str(p/'test.cc'),str(voice/'voice_web.cc'),str(p/'json.o'),'-o',str(p/'test')],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            subprocess.run([str(p/'test')],check=True,timeout=10)
