"""Check reset ordering and that failures never reach Wi-Fi erasure."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class FactoryResetPolicyTest(unittest.TestCase):
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
 for(int failure=0;failure<=3;++failure){
  std::vector<std::string> calls;Policy::Progress result;Policy::Operations op;
  op.validate=[&](){calls.push_back("validate");return failure!=1;};
  op.reset_voices=[&](){calls.push_back("voices");return failure!=2;};
  op.clear_wifi=[&](){calls.push_back("wifi");return failure!=3;};
  op.report=[&](const auto& p){result=p;};
  const bool ok=Policy::Run(op);assert(ok==(failure==0));
  if(failure==1||failure==2) for(auto& c:calls)assert(c!="wifi");
  if(failure==0){assert(calls.size()==3&&calls[1]=="voices"&&calls[2]=="wifi");assert(result.phase==Policy::Phase::kDone);}
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
