"""Exercise persisted tutorial progress and legacy migration with NVS failures."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / 'main/boards/common/wzh'


@unittest.skipUnless(shutil.which('g++'), 'g++ required')
class OnboardingStoreTest(unittest.TestCase):
    def test_reboot_completion_migration_and_invalid_record(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            (temp / 'esp_random.h').write_text('#pragma once\n#include <cstdint>\nuint32_t esp_random();\n')
            (temp / 'nvs.h').write_text(r'''
#pragma once
#include <cstddef>
#include <cstdint>
using nvs_handle_t=int;
constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,ESP_FAIL=2,ESP_ERR_INVALID_STATE=3;
constexpr int NVS_READONLY=0,NVS_READWRITE=1;
int nvs_open(const char*,int,nvs_handle_t*);
int nvs_get_u8(nvs_handle_t,const char*,uint8_t*);
int nvs_set_u8(nvs_handle_t,const char*,uint8_t);
int nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
int nvs_get_u32(nvs_handle_t,const char*,uint32_t*);
int nvs_set_u32(nvs_handle_t,const char*,uint32_t);
int nvs_erase_all(nvs_handle_t);
int nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
''')
            (temp / 'test.cc').write_text(r'''
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "nvs.h"
#include "wzh_onboarding_store.h"
std::map<std::string,std::vector<uint8_t>> store;
bool fail_write=false;int commits=0;
std::string key(int handle,const char* name){return std::to_string(handle)+":"+name;}
int nvs_open(const char* name,int mode,nvs_handle_t* out){
 *out=std::strcmp(name,"onboarding")==0?1:2;
 if(mode==NVS_READONLY){bool found=false;for(auto& e:store)if(e.first.rfind(std::to_string(*out)+":",0)==0)found=true;
  if(!found)return ESP_ERR_NVS_NOT_FOUND;}
 return ESP_OK;
}
int nvs_get_u8(int h,const char* n,uint8_t* v){auto it=store.find(key(h,n));if(it==store.end())return ESP_ERR_NVS_NOT_FOUND;*v=it->second[0];return ESP_OK;}
int nvs_set_u8(int h,const char* n,uint8_t v){if(fail_write)return ESP_FAIL;store[key(h,n)]={v};return ESP_OK;}
int nvs_get_blob(int h,const char* n,void* out,size_t* len){auto it=store.find(key(h,n));if(it==store.end())return ESP_ERR_NVS_NOT_FOUND;if(*len<it->second.size())return ESP_FAIL;*len=it->second.size();std::memcpy(out,it->second.data(),*len);return ESP_OK;}
int nvs_set_blob(int h,const char* n,const void* data,size_t len){if(fail_write)return ESP_FAIL;auto p=static_cast<const uint8_t*>(data);store[key(h,n)]={p,p+len};return ESP_OK;}
int nvs_get_u32(int h,const char* n,uint32_t* value){size_t len=sizeof(*value);return nvs_get_blob(h,n,value,&len);}
int nvs_set_u32(int h,const char* n,uint32_t value){return nvs_set_blob(h,n,&value,sizeof(value));}
int nvs_erase_all(int h){if(fail_write)return ESP_FAIL;for(auto it=store.begin();it!=store.end();){
 if(it->first.rfind(std::to_string(h)+":",0)==0)it=store.erase(it);else ++it;}return ESP_OK;}
uint32_t esp_random(){static uint32_t next=12;return next++;}
int nvs_commit(int){++commits;return ESP_OK;} void nvs_close(int){}
int main(){
 using namespace wzh;OnboardingProgress p;
 assert(LoadOnboardingProgress(p)&&!p.Done()&&p.step==0);
 for(int i=0;i<6;++i)p.Finish(true);
 assert(SaveOnboardingProgress(p));OnboardingProgress reboot;
 assert(LoadOnboardingProgress(reboot)&&reboot.step==6&&reboot.flags==1);
 for(int i=6;i<13;++i)reboot.Finish(true);
 assert(SaveOnboardingProgress(reboot));assert(store[key(2,"setup_guide")][0]==1);
 OnboardingProgress again;assert(LoadOnboardingProgress(again)&&again.Done()&&again.step==13);
 store.clear();store[key(2,"setup_guide")]={1};OnboardingProgress legacy;
 assert(LoadOnboardingProgress(legacy)&&legacy.Done());
 store.erase(key(2,"setup_guide"));
 bool guide=false;
 assert(ReadMiniProgramGuideDone(guide)&&!guide);
 const int before=commits;
 assert(MarkMiniProgramGuideDone(0)&&ReadMiniProgramGuideDone(guide)&&guide);
 assert(MarkMiniProgramGuideDone(0)&&commits==before+1);
 assert(!MarkMiniProgramGuideDone(5));
 assert(GetOnboardingEpoch()==0&&ResetOnboardingProgress()&&GetOnboardingEpoch()==12);
 assert(ReadMiniProgramGuideDone(guide)&&!guide&&!MarkMiniProgramGuideDone(0));
 assert(MarkMiniProgramGuideDone(12)&&ReadMiniProgramGuideDone(guide)&&guide);
 OnboardingProgress reset;assert(LoadOnboardingProgress(reset)&&!reset.Done()&&reset.step==0);
 assert(GetOnboardingEpoch()==12&&ResetOnboardingProgress()&&GetOnboardingEpoch()==13);
 // A v1 record (no skipped mask) still loads; a skip survives a round trip.
 store.clear();OnboardingProgress skipped;
 for(int i=0;i<3;++i)skipped.Interrupted();
 assert(skipped.skipped==1&&skipped.step==1);
 assert(SaveOnboardingProgress(skipped));
 OnboardingProgress reloaded;assert(LoadOnboardingProgress(reloaded));
 assert(reloaded.skipped==1&&reloaded.step==0);
 reloaded.Finish(true);assert(reloaded.skipped==0);
 // v1 blob: six bytes, no skipped mask. It loads with skipped==0, and the
 // unfinished step 0 is replayed exactly as v1 firmware would have done.
 store[key(1,"progress")]={1,0,0,0,0,0};
 OnboardingProgress v1;assert(LoadOnboardingProgress(v1)&&v1.skipped==0&&v1.step==0);
 store[key(1,"progress")][0]=99;OnboardingProgress invalid;assert(!LoadOnboardingProgress(invalid));
 store.clear();fail_write=true;assert(!LoadOnboardingProgress(invalid));
}
''')
            binary = temp / 'test'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(temp), '-I', str(COMMON), str(temp / 'test.cc'),
                            str(COMMON / 'wzh_onboarding_store.cc'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
