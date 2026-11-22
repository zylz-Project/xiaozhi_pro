"""Fault-inject the persistent ambiguous-create reservation."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
class VoiceReservationTest(unittest.TestCase):
    def test_reboot_and_failed_commit_preserve_reservation(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            (p/'nvs.h').write_text('''#pragma once\n#include <cstddef>\n#include <cstdint>\nusing nvs_handle_t=int;\nconstexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,NVS_READONLY=0,NVS_READWRITE=1;\nint nvs_open(const char*,int,nvs_handle_t*);void nvs_close(nvs_handle_t);\nint nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);int nvs_commit(nvs_handle_t);\nint nvs_get_u8(nvs_handle_t,const char*,uint8_t*);int nvs_set_u8(nvs_handle_t,const char*,uint8_t);int nvs_erase_key(nvs_handle_t,const char*);\n''')
            (p/'test.cc').write_text(r'''
#include "voice_reservation.h"
#include "nvs.h"
#include <cassert>
#include <cstring>
#include <string>
std::string durable,pending;bool failed=false;
int cloud_durable=-1,cloud_pending=-1,cloud_writes=0;
int nvs_open(const char*,int,nvs_handle_t* h){*h=1;pending=durable;cloud_pending=cloud_durable;return 0;}void nvs_close(nvs_handle_t){}
int nvs_get_blob(nvs_handle_t,const char*,void* out,size_t* n){if(durable.empty())return 1;if(*n<durable.size())return 2;std::memcpy(out,durable.data(),durable.size());*n=durable.size();return 0;}
int nvs_set_blob(nvs_handle_t,const char*,const void* p,size_t n){pending.assign((const char*)p,n);return 0;}
int nvs_get_u8(nvs_handle_t,const char*,uint8_t* out){if(cloud_durable<0)return 1;*out=cloud_durable;return 0;}
int nvs_set_u8(nvs_handle_t,const char* key,uint8_t value){assert(std::strlen(key)<=15);++cloud_writes;cloud_pending=value;return 0;}
int nvs_erase_key(nvs_handle_t,const char*){++cloud_writes;cloud_pending=-1;return 0;}
int nvs_commit(nvs_handle_t){if(failed)return 2;durable=pending;cloud_durable=cloud_pending;return 0;}
int main(){int x=99;assert(voice_tts::LoadVoiceReservation(x)&&x==-1);
 assert(voice_tts::SaveVoiceReservation(9));assert(voice_tts::LoadVoiceReservation(x)&&x==9);
 failed=true;assert(!voice_tts::SaveVoiceReservation(-1));failed=false;
 assert(voice_tts::LoadVoiceReservation(x)&&x==9);assert(!voice_tts::SaveVoiceReservation(10));
 assert(voice_tts::SaveVoiceReservation(-1));assert(voice_tts::LoadVoiceReservation(x)&&x==-1);
 durable[0]='x';assert(!voice_tts::LoadVoiceReservation(x));
 bool observed=true;assert(voice_tts::LoadCloudVoiceObservation(observed)&&!observed);
 assert(voice_tts::SaveCloudVoiceObservation(false)&&cloud_writes==0);
 assert(voice_tts::SaveCloudVoiceObservation(true)&&cloud_writes==1);
 assert(voice_tts::SaveCloudVoiceObservation(true)&&cloud_writes==1);
 failed=true;assert(!voice_tts::SaveCloudVoiceObservation(false));failed=false;
 assert(voice_tts::LoadCloudVoiceObservation(observed)&&observed);
 assert(voice_tts::SaveCloudVoiceObservation(false));
 assert(voice_tts::LoadCloudVoiceObservation(observed)&&!observed);}
''')
            voice=ROOT/'main/boards/common/wzh/voice_tts'
            build=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(p),'-I',str(voice),str(p/'test.cc'),str(voice/'voice_reservation.cc'),'-o',str(p/'test')],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            subprocess.run([str(p/'test')],check=True,timeout=10)
