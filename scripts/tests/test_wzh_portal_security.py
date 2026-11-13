"""Exercise production developer-server LAN-only, origin, and auth gates."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT=Path(__file__).resolve().parents[2]

class PortalSecurityTest(unittest.TestCase):
    def test_developer_dispatch_gate(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source=(ROOT/'main/boards/common/wzh/wzh_web_console.cc').read_text()
        method=queue_function(source,'esp_err_t WebConsole::GuardedDispatch(')
        code=r'''
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include "wzh_user_web_policy.h"
using esp_err_t=int;using socklen_t=unsigned;
constexpr int ESP_OK=0,AF_INET=2,HTTP_GET=0,HTTP_POST=1,HTTPD_403_FORBIDDEN=403,HTTPD_RESP_USE_STRLEN=-1;
struct sockaddr{};struct sockaddr_in{int sin_family=AF_INET;struct{unsigned s_addr=0;}sin_addr;};
unsigned peer_ip=0;int getpeername(int,sockaddr* address,socklen_t*){reinterpret_cast<sockaddr_in*>(address)->sin_addr.s_addr=peer_ip;return 0;}
unsigned ntohl(unsigned value){return value;}
struct httpd_req_t{void* user_ctx=nullptr;int method=HTTP_GET;const char*uri="/";std::map<std::string,std::string>headers;int status=200;};
int httpd_req_to_sockfd(httpd_req_t*){return 1;}
size_t httpd_req_get_hdr_value_len(httpd_req_t*r,const char*k){return r->headers[k].size();}
int httpd_req_get_hdr_value_str(httpd_req_t*r,const char*k,char*out,size_t size){auto s=r->headers[k];if(s.size()+1>size)return -1;std::strcpy(out,s.c_str());return 0;}
int httpd_resp_send_err(httpd_req_t*r,int status,const char*){r->status=status;return status;}
void httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){}
void httpd_resp_set_status(httpd_req_t*r,const char*){r->status=503;}
int httpd_resp_send(httpd_req_t*,const char*,int){return 0;}
struct WifiManager{static WifiManager& GetInstance(){static WifiManager w;return w;}bool IsPersistentPortalActive(){return true;}};
namespace wzh {
bool WebPeerIpv4(int,uint32_t& ip){ip=peer_ip;return true;}
struct WebConsole {
 struct RouteSlot {WebConsole*owner;esp_err_t(*handler)(httpd_req_t*);};
 struct Portal{bool ResetActive(){return false;}};Portal* user_portal_=nullptr;
 bool authorized=false,maintenance=false;int hits=0;
 bool RequireDeveloper(httpd_req_t*r){if(!authorized){r->status=403;return false;}return true;}
 bool maintenance_active(){return maintenance;}
 static esp_err_t GuardedDispatch(httpd_req_t*);
};
'''+method+r'''
}
int Handler(httpd_req_t*r){++static_cast<wzh::WebConsole*>(r->user_ctx)->hits;return 0;}
int main(){
 wzh::WebConsole c;wzh::WebConsole::RouteSlot slot{&c,Handler};httpd_req_t req;req.user_ctx=&slot;
 // AP clients cannot reach the LAN maintenance console.
 peer_ip=0xc0a80402;wzh::WebConsole::GuardedDispatch(&req);assert(req.status==403&&c.hits==0);
 // The page and read-only status/audio endpoints are public on the LAN.
 peer_ip=0xc0a80102;req.status=200;wzh::WebConsole::GuardedDispatch(&req);assert(c.hits==1);
 req.uri="/api/status";req.status=200;wzh::WebConsole::GuardedDispatch(&req);assert(c.hits==2);
 // Developer APIs still require the session token.
 req.uri="/api/developer/status";req.status=200;wzh::WebConsole::GuardedDispatch(&req);assert(req.status==403&&c.hits==2);
 c.authorized=true;req.status=200;wzh::WebConsole::GuardedDispatch(&req);assert(c.hits==3);
 req.method=HTTP_POST;req.headers={{"Host","192.168.1.2:8080"},{"Origin","http://attacker"}};
 req.status=200;wzh::WebConsole::GuardedDispatch(&req);assert(req.status==403&&c.hits==3);
 req.headers["Origin"]="http://192.168.1.2:8080";req.status=200;wzh::WebConsole::GuardedDispatch(&req);assert(c.hits==4);
 // CLI clients send no Origin; same-origin cookies are guarded by SameSite.
 req.headers={{"Host","192.168.1.2:8080"}};req.status=200;wzh::WebConsole::GuardedDispatch(&req);assert(c.hits==5);
 c.maintenance=true;req.status=200;wzh::WebConsole::GuardedDispatch(&req);assert(req.status==503&&c.hits==5);
}
'''
        with tempfile.TemporaryDirectory() as d:
            cpp=Path(d)/'guard.cc';binary=Path(d)/'guard';cpp.write_text(code)
            result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
              '-I',str(ROOT/'main/boards/common/wzh'),str(cpp),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(binary)],check=True,timeout=5)
