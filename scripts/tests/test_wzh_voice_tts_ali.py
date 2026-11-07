"""Host tests for the actual Ali adapter with a fake HTTP transport."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class AliProviderTest(unittest.TestCase):
    def test_turn_snapshots_ali_credentials(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        source_file = (ROOT/'main/boards/common/wzh/voice_tts/voice_session.cc').read_text()
        begin = queue_function(source_file, 'bool VoiceSession::Begin(')
        source = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#define ESP_LOGI(...) ((void)0)
constexpr int pdPASS=1;using TaskHandle_t=void*;
void xTaskNotifyGive(void*){}
static bool fail_create=false;
int xTaskCreate(void(*)(void*),const char*,int,void*,int,TaskHandle_t* h){
 if(fail_create)return 0;
 *h=reinterpret_cast<void*>(1);return pdPASS;
}
namespace voice_tts {
std::string ali="ali-key",voice="voice-A";
std::string LoadAliKey(){return ali;}std::string LoadAliVoice(){return voice;}
struct VoiceSession {
 struct PendingTurn {std::string provider,api_key,voice_id;std::function<void(bool)> completion;uint32_t playback_generation=0,generation=0;};
 std::mutex mutex_;std::deque<std::string> queue_;std::function<void(bool)> completion_;
 std::atomic<uint32_t> generation_{0},active_generation_{0};TaskHandle_t task_=nullptr;uint32_t playback_generation_=0;
 std::unique_ptr<PendingTurn> pending_;bool stopping_=false;
 bool finished_=false,failed_=false,had_text_=false;
 bool overflow_=false;size_t queued_bytes_=0;
 std::string provider_,api_key_,voice_id_;
 static void TaskEntry(void*){}
 bool Begin(uint32_t,std::function<void(bool)>);
};
''' + begin + r'''
}
int main(){
 using namespace voice_tts;VoiceSession session;
 assert(session.Begin(7,[](bool){}));ali="new-key";voice="voice-B";
 assert(session.provider_=="ali" && session.api_key_=="ali-key" && session.voice_id_=="voice-A");
 assert(!session.Begin(8,[](bool){}));
 VoiceSession failed;fail_create=true;assert(!failed.Begin(8,[](bool){}));assert(failed.api_key_.empty());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);(p/'test.cc').write_text(source)
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                str(p/'test.cc'),'-o',str(p/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(p/'test')],check=True,timeout=10)

    def test_legacy_provider_does_not_override_ali_configuration(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        header = r'''
#pragma once
#include <cstddef>
using nvs_handle_t=int;using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,NVS_READWRITE=1,NVS_READONLY=0;
esp_err_t nvs_open(const char*,int,nvs_handle_t*);void nvs_close(nvs_handle_t);
esp_err_t nvs_set_str(nvs_handle_t,const char*,const char*);
esp_err_t nvs_get_str(nvs_handle_t,const char*,char*,size_t*);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_erase_key(nvs_handle_t,const char*);esp_err_t nvs_commit(nvs_handle_t);
'''
        source = r'''
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include "nvs.h"
#include "voice_config.h"
static std::map<std::string,std::string> values;
static std::map<std::string,std::string> blobs;
esp_err_t nvs_open(const char*,int,nvs_handle_t* h){*h=1;return 0;}
void nvs_close(nvs_handle_t){}
esp_err_t nvs_set_str(nvs_handle_t,const char* k,const char* v){values[k]=v;return 0;}
esp_err_t nvs_get_str(nvs_handle_t,const char* k,char* out,size_t* size){
 auto it=values.find(k);if(it==values.end())return ESP_ERR_NVS_NOT_FOUND;
 if(out){std::memcpy(out,it->second.c_str(),it->second.size()+1);}
 *size=it->second.size()+1;return 0;}
esp_err_t nvs_get_blob(nvs_handle_t,const char* k,void* out,size_t* size){
 auto it=blobs.find(k);if(it==blobs.end())return ESP_ERR_NVS_NOT_FOUND;
 if(*size<it->second.size()){return 2;}std::memcpy(out,it->second.data(),it->second.size());
 *size=it->second.size();return 0;}
esp_err_t nvs_set_blob(nvs_handle_t,const char* k,const void* data,size_t size){
 blobs[k]=std::string(static_cast<const char*>(data),size);return 0;}
esp_err_t nvs_erase_key(nvs_handle_t,const char* k){return values.erase(k)?0:ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_commit(nvs_handle_t){return 0;}
int main(){
 values["provider"]="mimo";values["api_key"]="old-key";
 values["ali_key"]="ali-test";values["ali_voice"]="voice-test";
 assert(voice_tts::LoadAliKey()=="ali-test" && voice_tts::LoadAliVoice()=="voice-test");
 assert(voice_tts::SaveAliVoice("voice-new"));
 assert(voice_tts::LoadAliVoice()=="voice-new" && voice_tts::LoadPreviousAliVoice()=="voice-test");
 assert(values["provider"]=="mimo" && values["api_key"]=="old-key");
 assert(!voice_tts::SaveAliKey("bad\nkey") && voice_tts::LoadAliKey()=="ali-test");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);(p/'nvs.h').write_text(header);(p/'test.cc').write_text(source)
            provider=ROOT/'main/boards/common/wzh/voice_tts'
            result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                '-I',str(p),'-I',str(provider),str(provider/'voice_config.cc'),str(p/'test.cc'),
                '-o',str(p/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(p/'test')],check=True,timeout=10)

    def test_request_download_and_untrusted_urls(self):
        if not shutil.which('g++') or not shutil.which('gcc'):
            self.skipTest('C/C++ compilers required')
        http_h = r'''
#pragma once
#include <cstddef>
struct esp_http_client;
using esp_http_client_handle_t=esp_http_client*;
enum { HTTP_METHOD_POST, HTTP_METHOD_GET, ESP_OK=0 };
struct esp_http_client_config_t {
 const char* url=nullptr; int method=0; int timeout_ms=0;
 void* crt_bundle_attach=nullptr; bool disable_auto_redirect=false;
};
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*);
void esp_http_client_cleanup(esp_http_client_handle_t);
int esp_http_client_set_header(esp_http_client_handle_t,const char*,const char*);
int esp_http_client_open(esp_http_client_handle_t,int);
int esp_http_client_write(esp_http_client_handle_t,const char*,int);
int esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t,char*,int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
'''
        source = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "esp_http_client.h"
#include "ali_provider.h"
#include "ali_url.h"
static std::string post, auth, destination;
static std::string sse_override;
static bool download_has_auth=false, complete=true, evil=false, use_sse=false, sse_header=false;
static bool reached_eof=false;
struct esp_http_client {int method;std::string body;size_t pos=0;};
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* c) {
 assert(c->disable_auto_redirect); auto* h=new esp_http_client{c->method,"",0};
 if(c->method==HTTP_METHOD_GET) {destination=c->url;h->body=std::string("RIFF1234WAVE",12);}
 else if(use_sse){h->body="data: {\"output\":{\"audio\":{\"data\":\"AQI=\"}}}\r\n\r\n"
 "data: {\"output\":{\"audio\":{\"data\":\"AwQ=\"},\"finish_reason\":\"stop\"}}\n\n";
 if(!sse_override.empty())h->body=sse_override;}
 else {h->body=evil?R"({"output":{"audio":{"url":"https://evil.example/a"}}})":
 R"({"output":{"audio":{"url":"http://dashscope-result-bj.oss-cn-beijing.aliyuncs.com/x.wav?signature=redacted"}}})";}
 return h;
}
void esp_http_client_cleanup(esp_http_client_handle_t h){delete h;}
int esp_http_client_set_header(esp_http_client_handle_t h,const char* k,const char* v){
 if(std::string(k)=="Authorization"){if(h->method==HTTP_METHOD_GET)download_has_auth=true;else auth=v;}
 if(std::string(k)=="X-DashScope-SSE")sse_header=std::string(v)=="enable";
 return 0;}
int esp_http_client_open(esp_http_client_handle_t,int){return 0;}
int esp_http_client_write(esp_http_client_handle_t,const char* p,int n){int count=std::min(n,7);post.append(p,count);return count;}
int esp_http_client_fetch_headers(esp_http_client_handle_t){return 0;}
int esp_http_client_get_status_code(esp_http_client_handle_t){return 200;}
int esp_http_client_read(esp_http_client_handle_t h,char* out,int cap){
 size_t n=std::min({size_t(cap),size_t(5),h->body.size()-h->pos});
 if(n==0)reached_eof=true;
 std::memcpy(out,h->body.data()+h->pos,n);h->pos+=n;return static_cast<int>(n);}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t){return complete;}
int mbedtls_base64_decode(unsigned char* out,size_t cap,size_t* written,const unsigned char* in,size_t n){
 if(n!=4 || cap<2)return -1;
 std::string s(reinterpret_cast<const char*>(in),n);
 if(s=="AQI="){out[0]=1;out[1]=2;}else if(s=="AwQ="){out[0]=3;out[1]=4;}else return -1;
 *written=2;return 0;
}
int main(){
 std::string url;
 assert(!voice_tts::AliAudioHttpsUrl("https://dashscope-result-bj.oss-cn-beijing.aliyuncs.com.evil/a",url));
 assert(!voice_tts::AliAudioHttpsUrl("https://evil@dashscope-result-bj.oss-cn-beijing.aliyuncs.com/a",url));
 assert(!voice_tts::AliAudioHttpsUrl("https://dashscope-result-bj.oss-cn-beijing.aliyuncs.com:443/a",url));
 voice_tts::AliProvider provider;voice_tts::Request r;voice_tts::Response out;
 r.api_key="fake-test-key";r.voice_id="test-voice";r.text="hi";
 assert(provider.Synthesize(r,out)==voice_tts::Result::kOk);
 assert(auth=="Bearer fake-test-key" && !download_has_auth);
 assert(destination.find("https://dashscope-result-bj.oss-cn-beijing.aliyuncs.com/")==0);
 assert(post.find("qwen-audio-3.1-tts-flash")!=std::string::npos);
 assert(post.find("test-voice")!=std::string::npos && post.find("24000")!=std::string::npos);
 assert(post.find("\"instruction\"")!=std::string::npos);
 assert(out.wav.size()==12);
 use_sse=true;post.clear();destination.clear();reached_eof=false;std::vector<uint8_t> pcm;
 r.pcm_sink=[&](const uint8_t* p,size_t n){assert(!reached_eof);pcm.insert(pcm.end(),p,p+n);return true;};
 assert(provider.Synthesize(r,out)==voice_tts::Result::kOk && out.streamed && sse_header);
 assert((pcm==std::vector<uint8_t>{1,2,3,4}) && destination.empty());
 assert(post.find("\"format\":\"pcm\"")!=std::string::npos);
 sse_override="data: {\"output\":{\"audio\":{\"data\":\"AQI=\"}}}\n\n";
 reached_eof=false;assert(provider.Synthesize(r,out)==voice_tts::Result::kInvalidResponse);
 sse_override="data: {\"code\":\"InvalidKey\",\"message\":\"redacted\"}\n\n";
 assert(provider.Synthesize(r,out)==voice_tts::Result::kInvalidResponse);
 sse_override.clear();r.pcm_sink=[](const uint8_t*,size_t){return false;};
 assert(provider.Synthesize(r,out)==voice_tts::Result::kInvalidResponse);
 r.pcm_sink={};use_sse=false;
 evil=true;destination.clear();assert(provider.Synthesize(r,out)==voice_tts::Result::kInvalidResponse);
 assert(destination.empty());evil=false;complete=false;
 assert(provider.Synthesize(r,out)==voice_tts::Result::kNetworkError);
 complete=true;int checks=0;r.cancelled=[&checks] {return ++checks>4;};
 assert(provider.Synthesize(r,out)==voice_tts::Result::kCancelled);
 r.cancelled=[] {return true;};assert(provider.Synthesize(r,out)==voice_tts::Result::kCancelled);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p/'esp_http_client.h').write_text(http_h)
            (p/'mbedtls').mkdir()
            (p/'mbedtls/base64.h').write_text('#pragma once\n#include <cstddef>\nint mbedtls_base64_decode(unsigned char*,size_t,size_t*,const unsigned char*,size_t);\n')
            (p/'esp_crt_bundle.h').write_text('#pragma once\nstatic void* esp_crt_bundle_attach=nullptr;\n')
            (p/'test.cc').write_text(source)
            json = ROOT/'managed_components/espressif__cjson/cJSON'
            subprocess.run(['gcc','-c',str(json/'cJSON.c'),'-o',str(p/'json.o')],check=True,capture_output=True)
            provider = ROOT/'main/boards/common/wzh/voice_tts'
            result = subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                '-I',str(p),'-I',str(provider),'-I',str(json),str(provider/'ali_provider.cc'),
                str(p/'test.cc'),str(p/'json.o'),'-o',str(p/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(p/'test')],check=True,timeout=10)


if __name__ == '__main__':
    unittest.main()
