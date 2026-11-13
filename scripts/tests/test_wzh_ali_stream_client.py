"""Exercise the real duplex worker against a deterministic framed transport."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AliStreamClientTest(unittest.TestCase):
    def test_fragmented_audio_and_failure_before_and_after_send(self):
        if not shutil.which('g++') or not shutil.which('gcc'):
            self.skipTest('C and C++ compilers required')
        headers = {
            'esp_transport.h': '''#pragma once
struct transport; using esp_transport_handle_t=transport*; using esp_err_t=int;
constexpr int ESP_OK=0;
int esp_transport_connect(transport*,const char*,int,int); int esp_transport_close(transport*);
int esp_transport_destroy(transport*); int esp_transport_poll_read(transport*,int);
int esp_transport_read(transport*,char*,int,int);
''',
            'esp_transport_ssl.h': '''#pragma once
#include "esp_transport.h"
transport* esp_transport_ssl_init();
void esp_transport_ssl_crt_bundle_attach(transport*,esp_err_t(*)(void*));
''',
            'esp_transport_ws.h': '''#pragma once
#include "esp_transport.h"
enum ws_transport_opcodes_t {WS_TRANSPORT_OPCODES_CONT=0,WS_TRANSPORT_OPCODES_TEXT=1,WS_TRANSPORT_OPCODES_BINARY=2,WS_TRANSPORT_OPCODES_FIN=128};
transport* esp_transport_ws_init(transport*); void esp_transport_ws_set_path(transport*,const char*);
int esp_transport_ws_set_headers(transport*,const char*);
int esp_transport_ws_send_raw(transport*,ws_transport_opcodes_t,const char*,int,int);
int esp_transport_ws_get_read_opcode(transport*); int esp_transport_ws_get_read_payload_len(transport*);
bool esp_transport_ws_get_fin_flag(transport*);
''',
            'esp_crt_bundle.h': '#pragma once\n#include "esp_transport.h"\nint esp_crt_bundle_attach(void*);\n',
            'esp_random.h': '#pragma once\n#include <cstddef>\nvoid esp_fill_random(void*,size_t);\n',
            'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
            'esp_log.h': '#pragma once\n#define ESP_LOGI(...) ((void)0)\n',
        }
        code = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "ali_stream_client.h"
#include "esp_transport_ws.h"
#include "cJSON.h"
struct Frame { int opcode; std::string data; bool fin; size_t offset=0; };
struct transport {};
static std::deque<Frame> frames;
static Frame active; static bool held=false;
static std::string task, auth; static int mode=0, sent_text=0;
static bool bundle=false; static int closed=0, destroyed=0;
static std::string event(const char* event_type,const char* type="") {
 return "{\"header\":{\"task_id\":\""+task+"\",\"event\":\""+event_type+
 "\"},\"payload\":{\"output\":{\"type\":\""+type+"\"}}}";
}
static void push_text(const std::string& s) {
 auto mid=s.size()/2;frames.push_back({1,s.substr(0,mid),false});
 frames.push_back({0,s.substr(mid),true});
}
int esp_crt_bundle_attach(void*){return 0;}
void esp_fill_random(void* p,size_t n){std::memset(p,0x33,n);}
int64_t esp_timer_get_time(){static int64_t t=0;return t+=1000;}
transport* esp_transport_ssl_init(){return new transport;}
void esp_transport_ssl_crt_bundle_attach(transport*,int(*)(void*)){bundle=true;}
transport* esp_transport_ws_init(transport*){return new transport;}
void esp_transport_ws_set_path(transport*,const char* p){assert(std::string(p)=="/api-ws/v1/inference");}
int esp_transport_ws_set_headers(transport*,const char* h){auth=h;return 0;}
int esp_transport_connect(transport*,const char*,int,int){return mode==1?-1:0;}
int esp_transport_close(transport*){++closed;return 0;}
int esp_transport_destroy(transport* p){++destroyed;delete p;return 0;}
int esp_transport_ws_send_raw(transport*,ws_transport_opcodes_t opcode,const char* bytes,int n,int){
 assert(int(opcode)==129);
 cJSON* root=cJSON_ParseWithLength(bytes,n);assert(root);
 auto* h=cJSON_GetObjectItemCaseSensitive(root,"header");
 std::string action=cJSON_GetObjectItemCaseSensitive(h,"action")->valuestring;
 task=cJSON_GetObjectItemCaseSensitive(h,"task_id")->valuestring;
 if(action=="run-task"){
  push_text(event("task-started"));
  if(mode==3)frames.push_back({2,std::string("\1\2",2),true});
 }
 if(action=="continue-task"){
  ++sent_text;
  if(mode==2){cJSON_Delete(root);return -1;}
  push_text(event("result-generated","sentence-synthesis"));
  frames.push_back({2,std::string("\1\2",2),false});frames.push_back({0,std::string("\3\4",2),true});
  push_text(event("result-generated","sentence-end"));
 }
 if(action=="finish-task")push_text(event("task-finished"));
 cJSON_Delete(root);return n;
}
int esp_transport_poll_read(transport*,int){return held||!frames.empty()?1:0;}
int esp_transport_read(transport*,char* out,int,int){
 if(!held){active=frames.front();frames.pop_front();held=true;}
 size_t n=std::min(size_t(3),active.data.size()-active.offset);
 std::memcpy(out,active.data.data()+active.offset,n);active.offset+=n;
 if(active.offset==active.data.size())held=false;
 return int(n);
}
int esp_transport_ws_get_read_opcode(transport*){return active.opcode;}
int esp_transport_ws_get_read_payload_len(transport*){return int(active.data.size());}
bool esp_transport_ws_get_fin_flag(transport*){return active.fin;}
int main(){
 using namespace voice_tts;
 auto run=[](int selected){
  mode=selected;frames.clear();held=false;sent_text=0;closed=destroyed=0;
  StreamRequest request; StreamResponse response;request.api_key="test";request.voice_id="voice";
  int step=0,ends=0,cancel_checks=0;std::vector<uint8_t> pcm;
  request.cancelled=[&]{return selected==4 && ++cancel_checks>2;};
  request.next_text=[&](std::string& s,bool& done){if(step++==0)s="hello";else done=true;};
  request.pcm_sink=[&](const uint8_t* p,size_t n){pcm.insert(pcm.end(),p,p+n);return true;};
  request.sentence_end=[&]{++ends;};
  auto result=SynthesizeStream(request,response);
  assert(bundle && closed==1 && destroyed==2);
  if(selected==0){assert(result==Result::kOk && response.text_committed && ends==1);
   assert((pcm==std::vector<uint8_t>{1,2,3,4}));}
  if(selected==1)assert(result==Result::kNetworkError && !response.text_committed && !sent_text);
  if(selected==2)assert(result==Result::kNetworkError && response.text_committed && pcm.empty());
  if(selected==3)assert(result==Result::kInvalidResponse && pcm.empty());
  if(selected==4)assert(result==Result::kCancelled && !response.text_committed && pcm.empty());
 };
 run(0);run(1);run(2);run(3);run(4);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            for name, content in headers.items():
                (p / name).write_text(content)
            (p / 'test.cc').write_text(code)
            json = ROOT / 'managed_components/espressif__cjson/cJSON'
            provider = ROOT / 'main/boards/common/wzh/voice_tts'
            subprocess.run(['gcc', '-c', str(json / 'cJSON.c'), '-o', str(p / 'json.o')], check=True)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(p), '-I', str(provider), '-I', str(json),
                            str(provider / 'ali_stream_client.cc'),
                            str(provider / 'ali_stream_protocol.cc'), str(p / 'test.cc'),
                            str(p / 'json.o'), '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True, timeout=10)
