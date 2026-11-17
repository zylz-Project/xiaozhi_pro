"""Check reset ordering and that failures never reach Wi-Fi erasure."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]

class FactoryResetPolicyTest(unittest.TestCase):
    def test_onboarding_legacy_marker_is_cleared_before_progress(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        code = (ROOT/'main/boards/common/wzh/wzh_user_web.cc').read_text()
        methods = queue_function(code, 'bool SetSetupGuide(') + '\n' + queue_function(code, 'bool ClearOnboarding(')
        source = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
using nvs_handle_t=int;
constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,NVS_READWRITE=0;
#define pdMS_TO_TICKS(v) (v)
std::vector<std::string> calls;bool active=false,fail_legacy=false;uint64_t clock_ms=0;
uint64_t Now(){return clock_ms;}void vTaskDelay(int v){clock_ms+=v;}
void StopOnboarding(){calls.push_back("stop");}bool IsOnboardingActive(){return active;}
int nvs_open(const char* ns,int,int* h){*h=std::string(ns)=="wzh_web"?1:2;return ESP_OK;}
int nvs_set_u8(int,const char*,int){return ESP_OK;}
int nvs_erase_key(int,const char*){calls.push_back("legacy");return fail_legacy?2:ESP_OK;}
int nvs_erase_all(int){calls.push_back("progress");return ESP_OK;}
int nvs_commit(int h){calls.push_back(h==1?"legacy_commit":"progress_commit");return ESP_OK;}
void nvs_close(int){}
''' + methods + r'''
int main(){
 assert(ClearOnboarding());
 assert((calls==std::vector<std::string>{"stop","legacy","legacy_commit","progress","progress_commit"}));
 calls.clear();fail_legacy=true;assert(!ClearOnboarding());
 for(const auto& call:calls)assert(call!="progress");
 calls.clear();fail_legacy=false;active=true;clock_ms=0;assert(!ClearOnboarding());
 assert((calls==std::vector<std::string>{"stop"}));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp=Path(directory)/'clear.cc';cpp.write_text(source);binary=cpp.with_suffix('')
            result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(binary)],check=True,timeout=5)

    def test_success_and_each_failure_boundary(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = r'''
#include "wzh_factory_reset_policy.h"
#include <cassert>
#include <string>
#include <vector>
using Policy=wzh::FactoryResetPolicy;
int main(){
 for(int failure=0;failure<=4;++failure){
  std::vector<std::string> calls;Policy::Progress result;Policy::Operations op;
  op.validate=[&](){calls.push_back("validate");return failure!=1;};
  op.reset_voices=[&](){calls.push_back("voices");return failure!=2;};
  op.clear_onboarding=[&](){calls.push_back("onboarding");return failure!=3;};
  op.clear_wifi=[&](){calls.push_back("wifi");return failure!=4;};
  op.report=[&](const auto& p){result=p;};
  const bool ok=Policy::Run(op);assert(ok==(failure==0));
  if(failure>=1&&failure<=3) for(auto& c:calls)assert(c!="wifi");
  if(failure==0){assert(calls.size()==4&&calls[1]=="voices"&&calls[2]=="onboarding"&&calls[3]=="wifi");assert(result.phase==Policy::Phase::kDone);}
  else assert(result.phase==Policy::Phase::kFailed&&!result.error.empty());
 }
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp=Path(temp)/'reset.cc'; cpp.write_text(source)
            binary=Path(temp)/'reset'
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                            '-I',str(ROOT/'main/boards/common/wzh'),str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
