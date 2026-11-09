"""Exercise actual NVS migration, atomic voice-pair updates and rollback."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class VoiceConfigTest(unittest.TestCase):
    def test_legacy_migration_rollback_and_failed_commit(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = ROOT / 'main/boards/common/wzh/voice_tts/voice_config.cc'
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p/'nvs.h').write_text(r'''
#pragma once
#include <cstddef>
using nvs_handle_t=int;using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,NVS_READONLY=0,NVS_READWRITE=1;
int nvs_open(const char*,int,nvs_handle_t*);void nvs_close(nvs_handle_t);
int nvs_get_str(nvs_handle_t,const char*,char*,size_t*);int nvs_set_str(nvs_handle_t,const char*,const char*);
int nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
int nvs_erase_key(nvs_handle_t,const char*);int nvs_commit(nvs_handle_t);
''')
            (p/'test.cc').write_text(r'''
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "nvs.h"
#include "voice_config.h"
std::map<std::string,std::vector<char>> stored, pending;bool fail_commit=false;
int nvs_open(const char*,int,nvs_handle_t* h){*h=1;pending.clear();return ESP_OK;}
void nvs_close(nvs_handle_t){pending.clear();}
int nvs_get_blob(nvs_handle_t,const char* k,void* out,size_t* n){
 auto it=stored.find(k);if(it==stored.end())return ESP_ERR_NVS_NOT_FOUND;
 if(*n<it->second.size()){return 2;}*n=it->second.size();if(out)memcpy(out,it->second.data(),*n);return ESP_OK;}
int nvs_set_blob(nvs_handle_t,const char* k,const void* v,size_t n){pending[k]=std::vector<char>((const char*)v,(const char*)v+n);return ESP_OK;}
int nvs_get_str(nvs_handle_t h,const char* k,char* out,size_t* n){if(!out){auto i=stored.find(k);if(i==stored.end())return 1;*n=i->second.size();return 0;}return nvs_get_blob(h,k,out,n);}
int nvs_set_str(nvs_handle_t h,const char* k,const char* v){return nvs_set_blob(h,k,v,strlen(v)+1);}
int nvs_erase_key(nvs_handle_t,const char* k){stored.erase(k);return ESP_OK;}
int nvs_commit(nvs_handle_t){if(fail_commit)return 2;for(auto& x:pending)stored[x.first]=x.second;return ESP_OK;}
int main(){
 const char legacy[]="legacy";stored["ali_voice"]={legacy,legacy+sizeof(legacy)};
 assert(voice_tts::LoadAliVoice()=="legacy"&&voice_tts::LoadPreviousAliVoice().empty());
 assert(voice_tts::SaveAliVoice("newvoice"));
 assert(voice_tts::LoadAliVoice()=="newvoice"&&voice_tts::LoadPreviousAliVoice()=="legacy");
 assert(voice_tts::RestorePreviousAliVoice());
 assert(voice_tts::LoadAliVoice()=="legacy"&&voice_tts::LoadPreviousAliVoice()=="newvoice");
 assert(voice_tts::RestorePreviousAliVoice());
 fail_commit=true;assert(!voice_tts::SaveAliVoice("failed"));fail_commit=false;
 assert(voice_tts::LoadAliVoice()=="newvoice"&&voice_tts::LoadPreviousAliVoice()=="legacy");
 assert(!voice_tts::SaveAliVoice("invalid voice"));
 assert(voice_tts::SaveAliVoice(""));assert(voice_tts::LoadAliVoice().empty());
 assert(voice_tts::RestorePreviousAliVoice()&&voice_tts::LoadAliVoice()=="newvoice");
 assert(voice_tts::SaveAliVoice("other") && voice_tts::LoadPreviousAliVoice()=="newvoice");
 assert(!voice_tts::ForgetPreviousAliVoice("other"));
 assert(voice_tts::ForgetPreviousAliVoice("newvoice") && voice_tts::LoadPreviousAliVoice().empty());
 assert(voice_tts::SaveAliKey("keep-key"));
 assert(voice_tts::ClearAliVoiceSelection());
 assert(voice_tts::LoadAliVoice().empty()&&voice_tts::LoadPreviousAliVoice().empty());
 assert(voice_tts::LoadAliKey()=="keep-key"&&stored.count("ali_voice")==0);
 stored["ali_pair"]={'b','a','d'};assert(voice_tts::LoadAliVoice().empty());
 assert(!voice_tts::SaveAliVoice("must-not-overwrite-corrupt"));
 assert(stored["ali_pair"].size()==3);
}
''')
            exe=p/'test'
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-I',str(p),'-I',str(source.parent),str(p/'test.cc'),str(source),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(exe)],check=True,timeout=10)
