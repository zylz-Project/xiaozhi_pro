"""Test persistent local labels with the production name store."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class VoiceNamesTest(unittest.TestCase):
    def test_limits_duplicates_corruption_and_commit_failure(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        nvs = r'''
#pragma once
#include <cstddef>
using nvs_handle_t=int;using esp_err_t=int;
constexpr int ESP_OK=0, ESP_ERR_NVS_NOT_FOUND=1, NVS_READONLY=0,NVS_READWRITE=1;
int nvs_open(const char*,int,nvs_handle_t*);void nvs_close(nvs_handle_t);
int nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);int nvs_commit(nvs_handle_t);
'''
        cpp = r'''
#include "voice_names.h"
#include "nvs.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
std::string stored,pending; bool failed=false;
int nvs_open(const char*,int,nvs_handle_t* h){*h=1;pending=stored;return ESP_OK;}
void nvs_close(nvs_handle_t){}
int nvs_get_blob(nvs_handle_t,const char*,void* out,size_t* n){
 if(stored.empty())return ESP_ERR_NVS_NOT_FOUND;if(*n<stored.size())return 2;
 std::memcpy(out,stored.data(),stored.size());*n=stored.size();return ESP_OK;}
int nvs_set_blob(nvs_handle_t,const char*,const void* p,size_t n){pending.assign(static_cast<const char*>(p),n);return ESP_OK;}
int nvs_commit(nvs_handle_t){if(failed)return 2;stored=pending;return ESP_OK;}
int main(){using namespace voice_tts;std::vector<VoiceName> list;
 assert(LoadVoiceNames(list)&&list.empty());
 for(int i=0;i<10;i++)assert(SetVoiceName("id"+std::to_string(i),"声音"+std::to_string(i)));
 assert(!SetVoiceName("extra","超额"));assert(!SetVoiceName("id0","声音1"));
 assert(!SetVoiceName("id0",std::string(49,'x')));assert(!SetVoiceName("id0","bad\n"));
 assert(SetVoiceName("id0","新名字"));assert(LoadVoiceNames(list)&&list.size()==10&&list[0].name=="新名字");
 failed=true;assert(!RemoveVoiceName("id0"));failed=false;
 assert(LoadVoiceNames(list)&&list.size()==10);assert(RemoveVoiceName("id0"));
 assert(LoadVoiceNames(list)&&list.size()==9);assert(SetVoiceName("extra","新音色"));
 stored[0]='x';assert(!LoadVoiceNames(list));assert(!SetVoiceName("id1","bad"));
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d); (p/'nvs.h').write_text(nvs);(p/'test.cc').write_text(cpp)
            voice=ROOT/'main/boards/common/wzh/voice_tts'
            build=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-Wno-misleading-indentation','-I',str(p),'-I',str(voice),str(p/'test.cc'),
                str(voice/'voice_names.cc'),'-o',str(p/'test')],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            subprocess.run([str(p/'test')],check=True,timeout=10)
