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
int xTaskCreate(void(*f)(void*),const char*,int,void* a,int,void**){worker=f;worker_arg=a;return 1;}
void vTaskDelete(void*){}
namespace voice_tts {
std::string recorded_name;size_t recorded_samples=0;
bool IsValidVoiceName(const std::string& n){return !n.empty()&&n.find('\0')==std::string::npos;}
VoiceOperation EnrollManagedVoice(const int16_t* pcm,size_t n,const std::string& name,const std::function<bool()>&,std::string& id){
 assert(pcm[0]==1234);recorded_samples=n;recorded_name=name;id="id";return VoiceOperation::kOk;}
VoiceOperation ListManagedVoices(std::vector<ManagedVoice>& v){v={{"id","name",false}};return VoiceOperation::kOk;}
VoiceOperation RenameManagedVoice(const std::string&,const std::string&){return VoiceOperation::kOk;}
VoiceOperation DeleteManagedVoice(const std::string&){return VoiceOperation::kOk;}
VoiceOperation SelectManagedVoice(const std::string&){return VoiceOperation::kOk;}
}
int main(){using namespace voice_tts;VoiceWeb web;
 uint8_t bad[2]={1,'x'};assert(!web.Upload(bad,sizeof(bad)));
 const size_t n=16000*6;auto* data=(uint8_t*)std::malloc(1+3+n*2);
 data[0]=3;std::memcpy(data+1,"abc",3);data[4]=0xd2;data[5]=0x04;
 assert(web.Upload(data,1+3+n*2));assert(!web.Command("list","",""));assert(worker);
 worker(worker_arg);assert(recorded_samples==n&&recorded_name=="abc");
 cJSON* root=cJSON_CreateObject();web.AddStatus(root);
 assert(!cJSON_IsTrue(cJSON_GetObjectItem(root,"voice_management_busy")));
 assert(!std::strcmp(cJSON_GetObjectItem(root,"voice_management_status")->valuestring,"ok"));
 assert(cJSON_GetArraySize(cJSON_GetObjectItem(root,"voice_catalog"))==1);cJSON_Delete(root);
 assert(!web.Command("unexpected","",""));assert(web.Command("list","",""));worker(worker_arg);
}
''')
            json=ROOT/'managed_components/espressif__cjson/cJSON';voice=ROOT/'main/boards/wzh_crawlPanda/voice_tts'
            subprocess.run(['gcc','-c',str(json/'cJSON.c'),'-o',str(p/'json.o')],check=True)
            build=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-I',str(p),'-I',str(json),'-I',str(voice),str(p/'test.cc'),str(voice/'voice_web.cc'),str(p/'json.o'),'-o',str(p/'test')],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            subprocess.run([str(p/'test')],check=True,timeout=10)
