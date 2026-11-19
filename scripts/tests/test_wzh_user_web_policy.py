"""Execute the user-portal authentication and network boundary policy."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class UserWebPolicyTest(unittest.TestCase):
    def test_sessions_completion_rate_limit_and_boundaries(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = r'''
#include "wzh_user_web_policy.h"
#include <cassert>
#include <cstring>
using wzh::UserWebPolicy;
int main() {
 assert(UserWebPolicy::IsApClient(0xc0a80402));
 assert(!UserWebPolicy::IsApClient(0xc0a80502));
 assert(UserWebPolicy::ValidOrigin("192.168.4.1", "http://192.168.4.1"));
 assert(!UserWebPolicy::ValidOrigin("192.168.4.1.evil", "http://192.168.4.1.evil"));
 assert(!UserWebPolicy::ValidOrigin("192.168.4.1", "http://attacker"));
 assert(UserWebPolicy::Password(0x1a,0x2b)=="zylz1A2B");
 assert(!strcmp(UserWebPolicy::SetupFailure(15).code,"SETUP_BAD_PASSWORD"));
 assert(!strcmp(UserWebPolicy::SetupFailure(202).message,"密码不正确，请重新输入"));
 assert(!strcmp(UserWebPolicy::SetupFailure(201).code,"SETUP_NOT_FOUND"));
 assert(!strcmp(UserWebPolicy::SetupFailure(203).code,"SETUP_REFUSED"));
 assert(!strcmp(UserWebPolicy::SetupFailure(0).code,"SETUP_TIMEOUT"));
 assert(!strcmp(UserWebPolicy::SetupFailure(-1).code,"SETUP_SAVE_FAILED"));
 assert(!strcmp(UserWebPolicy::SetupFailure(-2).code,"SETUP_TIMEOUT"));
 using N=UserWebPolicy::Network; using R=UserWebPolicy::Route;
 assert(UserWebPolicy::RouteFor("/",N::kUnprovisioned,false,false)==R::kSetupFirst);
 assert(UserWebPolicy::RouteFor("/",N::kConnecting,true,false)==R::kSetupConnecting);
 assert(UserWebPolicy::RouteFor("/manual",N::kOffline,true,false)==R::kSetupOffline);
 assert(UserWebPolicy::RouteFor("/",N::kOffline,false,false,true)==R::kLogin);
 assert(UserWebPolicy::RouteFor("/setup",N::kOffline,false,false,true)==R::kLogin);
 assert(UserWebPolicy::RouteFor("/manual",N::kOffline,false,false,true)==R::kLogin);
 assert(UserWebPolicy::RouteFor("/",N::kOffline,false,false,false)==R::kSetupOffline);
 assert(UserWebPolicy::RouteFor("/",N::kOnline,false,false)==R::kLogin);
 assert(UserWebPolicy::RouteFor("/setup",N::kOnline,true,false)==R::kSetupSwitch);
 assert(UserWebPolicy::RouteFor("/manual",N::kOnline,true,false)==R::kManual);
 assert(UserWebPolicy::RouteFor("/",N::kOnline,true,false)==R::kDetails);
 assert(UserWebPolicy::RouteFor("/done",N::kOffline,false,false)==R::kRoot);
 assert(UserWebPolicy::RouteFor("/done",N::kConnecting,false,true)==R::kDone);
 assert(UserWebPolicy::LocalAccessible(false,false,true,true));
 assert(!UserWebPolicy::LocalAccessible(false,false,false,true));
 assert(!UserWebPolicy::LocalAccessible(false,false,true,false));
 assert(UserWebPolicy::AllowOfflineLogin(true,false,false,false));
 assert(!UserWebPolicy::AllowOfflineLogin(false,false,false,false));
 assert(!UserWebPolicy::AllowOfflineLogin(true,false,false,true));
 UserWebPolicy policy; std::array<uint8_t,16> rnd{}; std::string session;
 const uint32_t ap1=0xc0a80402,ap2=0xc0a80409,non_ap=0xc0a80502;
 for(int i=0;i<4;++i) assert(policy.Login(ap1,"wrong","zylz1A2B",rnd,100,session)==UserWebPolicy::LoginResult::kBadPassword);
 assert(policy.Login(ap1,"wrong","zylz1A2B",rnd,100,session)==UserWebPolicy::LoginResult::kRateLimited);
 assert(policy.Login(ap1,"zylz1A2B","zylz1A2B",rnd,101,session)==UserWebPolicy::LoginResult::kRateLimited);
 assert(policy.Login(ap1,"zylz1A2B","zylz1A2B",rnd,60100,session)==UserWebPolicy::LoginResult::kOk);
 assert(policy.CheckSession(ap1,session,60101));
 assert(policy.CheckSession(ap2,session,60101));
 assert(!policy.CheckSession(non_ap,session,60101));
 assert(!policy.CheckSession(ap1,session,60100+UserWebPolicy::kSessionMs));
 rnd[0]=1; auto ctx=policy.CreateCompletion(ap1,rnd,1000); rnd[0]=2; std::string entered;
 assert(!policy.Enter(non_ap,ctx,rnd,1001,true,entered));
 assert(!policy.Enter(ap1,ctx,rnd,1001,false,entered));
 assert(policy.Enter(ap2,ctx,rnd,1001,true,entered));
 assert(!policy.Enter(ap2,ctx,rnd,1002,true,entered));
 assert(policy.CheckSession(ap1,entered,1002));
 policy.Logout(ap1,entered); assert(!policy.CheckSession(ap2,entered,1002));
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = Path(temp)/'policy.cc'; cpp.write_text(source)
            binary = Path(temp)/'policy'
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                            '-I',str(ROOT/'main/boards/common/wzh'),str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
