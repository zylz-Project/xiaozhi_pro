"""Execute the user-portal authentication and network boundary policy."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]

class UserWebPolicyTest(unittest.TestCase):
    def test_setup_job_and_completion_survive_hotspot_address_change(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        portal = (ROOT / 'main/boards/common/wzh/wzh_user_web.cc').read_text()
        status = queue_function(portal, 'esp_err_t UserWebPortal::SetupStatus(')
        enter = queue_function(portal, 'if (path == "/api/enter")')
        source = r'''
#include "wzh_user_web_policy.h"
#include <cassert>
#include <map>
#include <memory>
#include <mutex>
using wzh::UserWebPolicy;
using esp_err_t = int;
struct cJSON { std::map<std::string, std::string> fields; };
cJSON* cJSON_CreateObject() { return new cJSON; }
void cJSON_Delete(cJSON* item) { delete item; }
void cJSON_AddBoolToObject(cJSON*, const char*, bool) {}
void cJSON_AddStringToObject(cJSON* item, const char* key, const char* value) {
    item->fields[key] = value;
}
cJSON* cJSON_AddObjectToObject(cJSON*, const char*) { return nullptr; }
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
struct httpd_req_t { std::string ctx, cookie; cJSON output; };
void httpd_resp_set_hdr(httpd_req_t* req, const char*, const char* value) {
    req->cookie = value;
}
int httpd_resp_send_500(httpd_req_t*) { return 500; }
int Error(httpd_req_t*, const char* status, const char*, const char*) {
    return (status[0]-'0')*100+(status[1]-'0')*10+status[2]-'0';
}
int Ok(httpd_req_t*) { return 200; }
int JsonResponse(httpd_req_t* req, const cJSON* data) { req->output = *data; return 200; }
std::string Cookie(httpd_req_t*, const char*) { return {}; }
Json ReadBody(httpd_req_t* req) {
    Json data(cJSON_CreateObject(), cJSON_Delete);
    data->fields["ctx"] = req->ctx;
    return data;
}
const char* String(cJSON* json, const char* name) { return json->fields.at(name).c_str(); }
struct WifiManager {
    static WifiManager GetInstance() { return {}; }
    bool IsConnected() const { return true; }
};
enum class JobPhase { kOnline, kFailed };
struct UserWebPortal {
    UserWebPolicy auth_;
    std::mutex mutex_;
    struct Job {
        uint32_t ip = 0xc0a80402;
        JobPhase phase = JobPhase::kOnline;
        int failure_reason = 0;
        std::string id = "setup-job", context;
    } setup_;
    static uint64_t Now() { return 100; }
    static std::array<uint8_t, 16> Random() { return {2}; }
    int SetupStatus(httpd_req_t*, uint32_t, bool, std::string_view job = {});
    int Enter(httpd_req_t* req, uint32_t ip, bool online, bool session = false);
};
''' + status + r'''
int UserWebPortal::Enter(httpd_req_t* req, uint32_t ip, bool online, bool session) {
    const std::string path = "/api/enter";
    const auto now = Now();
''' + enter + r'''
    return 404;
}
int main() {
    UserWebPortal portal;
    const uint32_t changed = 0xc0a80409, outside = 0xc0a80509;
    portal.setup_.context = portal.auth_.CreateCompletion(portal.setup_.ip, {1}, portal.Now());
    httpd_req_t req;
    assert(portal.SetupStatus(&req, changed, false, "wrong") == 401);
    assert(portal.SetupStatus(&req, outside, false, "setup-job") == 401);
    assert(portal.SetupStatus(&req, changed, false, "setup-job") == 200);
    req.ctx = req.output.fields.at("ctx");
    assert(portal.Enter(&req, outside, true) == 401);
    assert(portal.Enter(&req, changed, false) == 409);
    assert(portal.Enter(&req, changed, true) == 200);
    assert(req.cookie.find("wzh_sess=") == 0);
    assert(portal.Enter(&req, changed, true) == 401);
    assert(portal.Enter(&req, changed, true, true) == 200);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = Path(temp) / 'setup.cc'
            cpp.write_text(source)
            binary = Path(temp) / 'setup'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/boards/common/wzh'), str(cpp), '-o',
                            str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

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
 assert(UserWebPolicy::RouteFor("/manual",N::kOffline,true,false)==R::kManual);
 assert(UserWebPolicy::RouteFor("/",N::kOffline,false,false,true)==R::kLogin);
 assert(UserWebPolicy::RouteFor("/setup",N::kOffline,false,false,true)==R::kLogin);
 assert(UserWebPolicy::RouteFor("/manual",N::kOffline,false,false,true)==R::kManual);
 assert(UserWebPolicy::RouteFor("/manual",N::kUnprovisioned,false,false)==R::kManual);
 assert(UserWebPolicy::RouteFor("/manual",N::kOnline,false,false)==R::kManual);
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
 // Two phones hold independent cookies. Re-entering or logging out on one
 // does not replace the other phone's session.
 rnd[0]=3; std::string second_session;
 assert(policy.Login(ap2,"zylz1A2B","zylz1A2B",rnd,60102,second_session)==UserWebPolicy::LoginResult::kOk);
 assert(session!=second_session);
 assert(policy.CheckSession(ap1,session,60103));
 assert(policy.CheckSession(ap2,second_session,60103));
 policy.Logout(ap1,session);
 assert(!policy.CheckSession(ap1,session,60104));
 assert(policy.CheckSession(ap2,second_session,60104));
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
