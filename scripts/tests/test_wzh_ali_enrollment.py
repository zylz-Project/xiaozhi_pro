"""Exercise enrollment capture bounds and stop behavior using production code."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class AliEnrollmentTest(unittest.TestCase):
    def test_first_enrollment_prompt_uses_builtin_without_changing_selected_voice(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/boards/common/wzh/voice_tts/ali_voice_enrollment.cc').read_text()
        methods = queue_function(source, 'bool AliVoiceEnrollment::IsConfigured(') + '\n' + \
            queue_function(source, 'bool AliVoiceEnrollment::Speak(')
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
int64_t esp_timer_get_time(){return 0;}
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
void ulTaskNotifyTake(int,int){}
void xTaskNotifyGive(void*){}
std::string key, selected;
std::string LoadAliKey(){return key;}
std::string LoadAliVoice(){return selected;}
struct BuiltinVoice{const char* id;};
const BuiltinVoice kBuiltinVoices[]={{"builtin"}};
struct AudioService{uint32_t GetPlaybackGeneration(){return 42;}};
struct Application{AudioService audio;static Application& GetInstance(){static Application a;return a;}
 AudioService& GetAudioService(){return audio;}};
struct ExternalTts {
 std::string ready_voice, begun_voice;std::function<void(bool)> callback;
 bool ReadyForVoice(const std::string& v){ready_voice=v;return true;}
 bool BeginWithVoice(uint32_t gen,const std::string& v,std::function<void(bool)> done){
  assert(gen==42);begun_voice=v;callback=done;return true;}
 bool Enqueue(const char*){return true;}
 void Finish(){callback(true);}
 void Cancel(){}
};
struct Board{ExternalTts external;static Board& GetInstance(){static Board b;return b;}
 ExternalTts* GetExternalTts(){return &external;}};
struct AliVoiceEnrollment{
 std::atomic<bool> active_{true},prompt_done_{false},prompt_ok_{false};
 std::atomic<uint32_t> generation_{1};std::mutex mutex_;void* task_=nullptr;
 bool IsConfigured() const;bool Speak(const char*,uint32_t);
};
''' + methods + r'''
int main(){
 AliVoiceEnrollment a;assert(!a.IsConfigured());key="key";assert(a.IsConfigured());
 assert(a.Speak("prompt",1));auto& e=Board::GetInstance().external;
 assert(e.ready_voice=="builtin"&&e.begun_voice=="builtin"&&selected.empty());
 selected="cloned";assert(a.Speak("prompt",1));
 assert(e.ready_voice.empty()&&e.begun_voice.empty()&&selected=="cloned");
 assert(!a.Speak("prompt",2));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.cc'
            path.write_text(program)
            exe = path.with_suffix('')
            result = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                     '-Werror', str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(exe)], check=True, timeout=10)

    def test_capture_stops_after_speech_and_silence_without_overflow(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/boards/common/wzh/voice_tts/ali_voice_enrollment.cc').read_text()
        capture = queue_function(source, 'bool AliVoiceEnrollment::Capture(')
        program = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
constexpr size_t kRate=16000,kMaxSamples=20*kRate,kMinSpeechSamples=5*kRate,kEndSilenceSamples=3*kRate/2;
void xTaskNotifyGive(void*){}
struct AliVoiceEnrollment {
 std::atomic<bool> active_{false},recording_{false},audio_done_{false};
 std::mutex mutex_;int16_t* samples_=nullptr;size_t count_=0,speech_samples_=0;
 size_t first_speech_=0,last_speech_=0,silence_samples_=0;void* task_=nullptr;
 bool Capture(const int16_t*,size_t,bool);
};
''' + capture + r'''
int main(){
 AliVoiceEnrollment a; std::vector<int16_t> output(kMaxSamples), frame(960,42);
 assert(!a.Capture(frame.data(),frame.size(),true));
 a.active_=true;a.recording_=true;a.samples_=output.data();
 for(int i=0;i<10;i++) assert(a.Capture(frame.data(),frame.size(),false));
 for(int i=0;i<84;i++) assert(a.Capture(frame.data(),frame.size(),true));
 assert(a.first_speech_==9600&&a.last_speech_==90240&&!a.audio_done_);
 for(int i=0;i<25;i++) assert(a.Capture(frame.data(),frame.size(),false));
 assert(a.audio_done_&&!a.recording_&&a.speech_samples_>=kMinSpeechSamples);
 const auto count=a.count_;a.Capture(frame.data(),frame.size(),true);assert(a.count_==count);
 AliVoiceEnrollment b;b.active_=true;b.recording_=true;b.samples_=output.data();
 for(int i=0;i<334;i++)b.Capture(frame.data(),frame.size(),true);
 assert(b.audio_done_&&b.count_==kMaxSamples);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.cc'
            path.write_text(program)
            exe = path.with_suffix('')
            result = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                     '-Werror', str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(exe)], check=True, timeout=10)

    def test_enrollment_request_stream_and_status_poll(self):
        if not shutil.which('g++') or not shutil.which('gcc'):
            self.skipTest('C++ compiler required')
        json_dir = ROOT / 'managed_components/espressif__cjson/cJSON'
        provider = ROOT / 'main/boards/common/wzh/voice_tts/ali_enrollment.cc'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'freertos').mkdir()
            (path / 'mbedtls').mkdir()
            (path / 'esp_crt_bundle.h').write_text('inline void esp_crt_bundle_attach() {}\n')
            (path / 'esp_log.h').write_text('#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n')
            (path / 'esp_timer.h').write_text('#include <cstdint>\ninline int64_t esp_timer_get_time(){return 0;}\n')
            (path / 'esp_heap_caps.h').write_text('#include <cstdlib>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\nextern bool fail_scratch;\ninline void* heap_caps_malloc(size_t n,int){return fail_scratch?nullptr:malloc(n);}\n')
            (path / 'freertos/FreeRTOS.h').write_text('#define pdMS_TO_TICKS(x) (x)\n')
            (path / 'freertos/task.h').write_text('inline void vTaskDelay(int) {}\n')
            (path / 'mbedtls/base64.h').write_text('''#include <cstddef>\nextern "C" int mbedtls_base64_encode(unsigned char*,size_t,size_t*,const unsigned char*,size_t);\n''')
            (path / 'esp_http_client.h').write_text(r'''
#pragma once
#include <string>
using esp_err_t=int;constexpr int ESP_OK=0;constexpr int HTTP_METHOD_POST=1;
struct esp_http_client_config_t {const char*url=nullptr;int method=0;int timeout_ms=0;
 void (*crt_bundle_attach)()=nullptr;bool disable_auto_redirect=false;};
struct esp_http_client {std::string body,response;size_t offset=0,expected=0;};
using esp_http_client_handle_t=esp_http_client*;
esp_http_client* esp_http_client_init(const esp_http_client_config_t*);
void esp_http_client_cleanup(esp_http_client*);
int esp_http_client_set_header(esp_http_client*,const char*,const char*);
int esp_http_client_open(esp_http_client*,int);
int esp_http_client_write(esp_http_client*,const char*,int);
int esp_http_client_fetch_headers(esp_http_client*);
int esp_http_client_get_status_code(esp_http_client*);
int esp_http_client_read(esp_http_client*,char*,int);
bool esp_http_client_is_complete_data_received(esp_http_client*);
''')
            (path / 'test.cc').write_text(r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <openssl/evp.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "ali_enrollment.h"
std::vector<std::string> bodies;
int next_status=200;
bool fail_scratch=false;
bool list_mode=false;
esp_http_client* esp_http_client_init(const esp_http_client_config_t* c){assert(c->disable_auto_redirect);return new esp_http_client;}
void esp_http_client_cleanup(esp_http_client* p){delete p;}
int esp_http_client_set_header(esp_http_client*,const char*,const char*){return 0;}
int esp_http_client_open(esp_http_client* p,int n){p->expected=n;return 0;}
int esp_http_client_write(esp_http_client* p,const char* data,int n){n=std::min(n,101);p->body.append(data,n);return n;}
int esp_http_client_fetch_headers(esp_http_client* p){assert(p->body.size()==p->expected);bodies.push_back(p->body);
 if(list_mode) {if(p->body.find("list_voice")!=std::string::npos && p->body.find("\"page_index\":1")!=std::string::npos) p->response="{\"output\":{\"voice_list\":[]}}";
 else if(p->body.find("list_voice")!=std::string::npos) p->response="{\"output\":{\"voice_list\":[{\"voice_id\":\"qwen-audio-3.1-tts-flash-device1234-abc\"},{\"voice_id\":\"qwen-audio-3.1-tts-flash-other-xyz\"}]}}";
 else {p->response="{\"output\":{},\"request_id\":\"x\"}";} return p->response.size();}
 if(bodies.size()==1)p->response="{\"output\":{\"voice_id\":\"qwen-audio-3.1-tts-flash-panda-123\"}}";
 else if(bodies.size()==2)p->response="{\"output\":{\"status\":\"DEPLOYING\"}}";
 else {p->response="{\"output\":{\"status\":\"OK\"}}";}return p->response.size();}
int esp_http_client_get_status_code(esp_http_client*){return next_status;}
int esp_http_client_read(esp_http_client* p,char* data,int n){n=std::min(size_t(n),p->response.size()-p->offset);memcpy(data,p->response.data()+p->offset,n);p->offset+=n;return n;}
bool esp_http_client_is_complete_data_received(esp_http_client* p){return p->offset==p->response.size();}
extern "C" int mbedtls_base64_encode(unsigned char* out,size_t cap,size_t* used,const unsigned char* in,size_t n){
 size_t need=4*((n+2)/3);if(cap<need+1)return -1;*used=EVP_EncodeBlock(out,in,n);return 0;}
int main(){
 std::vector<int16_t> pcm(16000*6,1234);std::string voice;
 assert(voice_tts::CreateAliVoice("test-key",pcm.data(),pcm.size(),[]{return false;},voice));
 assert(voice=="qwen-audio-3.1-tts-flash-panda-123"&&bodies.size()==3);
 cJSON* root=cJSON_Parse(bodies[0].c_str());assert(root);
 auto* input=cJSON_GetObjectItemCaseSensitive(root,"input");
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(input,"action")->valuestring,"create_voice"));
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(input,"target_model")->valuestring,"qwen-audio-3.1-tts-flash"));
 const char* audio=cJSON_GetObjectItemCaseSensitive(input,"audio")->valuestring;
 const char* prefix="data:audio/wav;base64,";assert(!strncmp(audio,prefix,strlen(prefix)));
 const char* encoded=audio+strlen(prefix);int len=strlen(encoded);
 std::vector<unsigned char> wav(len/4*3+4);int decoded=EVP_DecodeBlock(wav.data(),(const unsigned char*)encoded,len);
 if(len&&encoded[len-1]=='='){--decoded;}if(len>1&&encoded[len-2]=='='){--decoded;}
 assert(decoded==44+int(pcm.size()*2));assert(!memcmp(wav.data(),"RIFF",4));
 assert(!memcmp(wav.data()+8,"WAVEfmt ",8));assert(!memcmp(wav.data()+44,pcm.data(),pcm.size()*2));
 cJSON_Delete(root);
 bodies.clear();fail_scratch=true;
 assert(voice_tts::CreateAliVoice("test-key",pcm.data(),pcm.size(),[]{return false;},voice));
 root=cJSON_Parse(bodies[0].c_str());assert(root);input=cJSON_GetObjectItemCaseSensitive(root,"input");
 audio=cJSON_GetObjectItemCaseSensitive(input,"audio")->valuestring;encoded=audio+strlen(prefix);len=strlen(encoded);
 wav.resize(len/4*3+4);decoded=EVP_DecodeBlock(wav.data(),(const unsigned char*)encoded,len);
 if(len&&encoded[len-1]=='='){--decoded;}if(len>1&&encoded[len-2]=='='){--decoded;}
 assert(decoded==44+int(pcm.size()*2)&&!memcmp(wav.data()+44,pcm.data(),pcm.size()*2));
 cJSON_Delete(root);
 bodies.clear();next_status=401;voice="old";int http_status=0;
 assert(!voice_tts::CreateAliVoice("test-key",pcm.data(),pcm.size(),[]{return false;},voice,&http_status));
 assert(voice.empty()&&bodies.size()==1&&http_status==401);
 next_status=200;list_mode=true;bodies.clear();std::vector<std::string> voices;
 assert(voice_tts::ListAliVoices("test-key","device1234",[]{return false;},voices));
 assert(voices.size()==1 && voices[0]=="qwen-audio-3.1-tts-flash-device1234-abc");
 auto before=bodies.size();
 assert(!voice_tts::DeleteAliVoice("test-key","device1234","qwen-audio-3.1-tts-flash-other-xyz",[]{return false;}));
 assert(bodies.size()==before);
 assert(voice_tts::DeleteAliVoice("test-key","device1234",voices[0],[]{return false;}));
 assert(bodies.size()==before+1 && bodies.back().find("delete_voice")!=std::string::npos);
 assert(!voice_tts::ListAliVoices("test-key","bad!",[]{return false;},voices));
}
''')
            obj = path / 'json.o'
            subprocess.run(['gcc', '-c', str(json_dir / 'cJSON.c'), '-o', str(obj)], check=True)
            exe = path / 'test'
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(path), '-I', str(json_dir), '-I', str(provider.parent),
                str(path / 'test.cc'), str(provider), str(obj), '-lcrypto', '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(exe)], check=True, timeout=10)
