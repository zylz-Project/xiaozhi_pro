"""Exercise real developer HTTP handlers and maintenance workers with hardware substitutes."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT/'main/boards/common/wzh'
JSON = ROOT/'managed_components/espressif__cjson/cJSON'


class WzhDeveloperConsoleTest(unittest.TestCase):
    def run_cpp(self, program):
        with tempfile.TemporaryDirectory() as directory:
            cpp=Path(directory)/'test.cc';cpp.write_text(program)
            obj=cpp.with_suffix('.o');exe=cpp.with_suffix('')
            result=subprocess.run(['gcc','-c',str(JSON/'cJSON.c'),'-o',str(obj)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-I',str(COMMON),'-I',str(JSON),str(cpp),str(obj),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr)

    def console(self):
        source=(COMMON/'wzh_web_console.cc').read_text()
        header=(COMMON/'wzh_web_console.h').read_text()
        cls=queue_function(header,'class WebConsole').replace('private:','public:')+';'
        methods='\n'.join(queue_function(source,marker) for marker in [
            'const char* HttpStatusText(', 'bool WebConsole::ReadJsonBody(',
            'esp_err_t WebConsole::SendJson(', 'esp_err_t WebConsole::SendError(',
            'bool WebConsole::RequireDeveloper(', 'esp_err_t WebConsole::HandleDeveloperLogin(',
            'esp_err_t WebConsole::HandleDeveloperLogout(', 'esp_err_t WebConsole::HandleDeveloperVolume(',
            'esp_err_t WebConsole::HandleDeveloperCenter(', 'esp_err_t WebConsole::HandleDeveloperErase(',
            'esp_err_t WebConsole::HandleStop(', 'void WebConsole::StartErase(',
            'void WebConsole::EraseTaskEntry(', 'void WebConsole::EraseTask(', 'void WebConsole::FinishErase('])
        return HARNESS.replace('__CLASS__',cls).replace('__METHODS__',methods)

    def test_saved_mute_survives_codec_start_and_invalid_volume_is_clamped(self):
        method=queue_function((ROOT/'main/audio/audio_codec.cc').read_text(), 'void AudioCodec::Start(')
        self.run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <initializer_list>
#define ESP_LOGI(...) ((void)0)
int stored=70;
struct Settings{Settings(const char*,bool){}int GetInt(const char*,int){return stored;}};
struct AudioCodec{int output_volume_=70;void Start();};
'''+method+r'''
int main(){
    for(int value:{-10,0,1,70,100,150}){
        stored=value;AudioCodec codec;codec.Start();assert(codec.output_volume_==std::clamp(value,0,100));
    }
}
''')

    def test_external_erase_refuses_uninitialized_busy_and_invalidates_partial_erase(self):
        source=(COMMON/'wzh_audio_store.cc').read_text()
        guard=queue_function(source,'struct WritingGuard')+';'
        method=queue_function(source,'bool AudioStore::EraseAll(')
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <mutex>
#include <string>
#include <vector>
#define ESP_LOGW(...) ((void)0)
constexpr int ESP_OK=0;
int raw_erases=0;bool succeed=true;
int wzh_flash_erase_all(){++raw_erases;return succeed?ESP_OK:-1;}
struct AudioStore{
    enum class Layout{kEmpty,kV3};
    uint64_t capacity_=0;uint32_t erase_size_=4096,toc_seq_=42;int current_slot_=3;
    std::atomic<bool>writing_{false};std::mutex mutex_;
    std::vector<int>files_{1,2};Layout layout_=Layout::kV3;
    bool EraseAll(std::string*);
};
'''+guard+method+r'''
int main(){
    AudioStore store;std::string error;
    assert(!store.EraseAll(&error)&&raw_erases==0&&store.files_.size()==2);
    store.capacity_=128000000;store.writing_=true;
    assert(!store.EraseAll(&error)&&raw_erases==0&&store.files_.size()==2);
    store.writing_=false;succeed=false;
    assert(!store.EraseAll(&error)&&raw_erases==1&&store.files_.empty());
    assert(!store.writing_&&store.layout_==AudioStore::Layout::kEmpty&&store.current_slot_==-1);
    succeed=true;assert(store.EraseAll(&error)&&raw_erases==2&&!store.writing_);
}
''')

    def test_server_auth_partial_json_volume_and_expiry(self):
        self.run_cpp(self.console()+r'''
int main(){
    WebConsole console;AudioStore store;AudioPlayer player;AudioHub hub;
    console.options_.store=&store;console.options_.player=&player;console.options_.hub=&hub;
    auto call=[&](const std::string& body,bool auth=true){
        auto r=Request(console,body,auth);WebConsole::HandleDeveloperVolume(&r);return r;
    };
    auto locked=call("{\"volume\":30}",false);assert(locked.status=="403 Forbidden");
    auto bad=Request(console,"{\"password\":\"wrong\"}",false);
    WebConsole::HandleDeveloperLogin(&bad);assert(bad.status=="403 Forbidden");
    auto login=Request(console,"{\"password\":\"1\"}",false);login.chunk=2;
    WebConsole::HandleDeveloperLogin(&login);assert(login.status=="200 OK"&&login.cursor==login.content_len);
    assert(std::strlen(console.developer_.token())==32);
    for(auto body:{"[]","{}junk","{\"volume\":1.5}","{\"volume\":101}","{\"volume\":-1}"}){
        auto r=call(body);assert(r.status=="400 Bad Request");
    }
    auto valid=call("{\"volume\":0}");assert(valid.status=="202 Accepted"&&Board::GetInstance().codec.volume==70);
    Application::GetInstance().Run();assert(Board::GetInstance().codec.volume==0);
    clock_us+=int64_t(DeveloperSession::kLifetimeMs)*1000;
    auto expired=call("{\"volume\":80}");assert(expired.status=="403 Forbidden");
    assert(Application::GetInstance().scheduled.empty());
}
''')

    def test_erase_is_confirmed_deferred_exclusive_and_waits_for_reader(self):
        self.run_cpp(self.console()+r'''
int main(){
    WebConsole console;AudioStore store;AudioPlayer player;AudioHub hub;
    console.options_.store=&store;console.options_.player=&player;console.options_.hub=&hub;
    bool motion=false;console.options_.maintenance=[&](bool enabled){motion=enabled;};
    std::array<uint8_t,16> random{};assert(console.developer_.Unlock("1",random,clock_us/1000));
    auto erase=[&](std::string body="{\"confirm\":\"ERASE\"}",bool auth=true){
        auto r=Request(console,body,auth);WebConsole::HandleDeveloperErase(&r);return r;
    };
    assert(erase("{\"confirm\":\"ERASE\"}",false).status=="403 Forbidden");
    assert(erase("{\"confirm\":\"erase\"}").status=="400 Bad Request");
    Board::GetInstance().battery=5;assert(erase().status=="409 Conflict");Board::GetInstance().battery=80;
    auto& app=Application::GetInstance();app.state=kDeviceStateSpeaking;
    assert(erase().status=="409 Conflict");app.state=kDeviceStateIdle;
    assert(erase().status=="202 Accepted");assert(console.maintenance_active()&&store.erases==0&&tasks.empty());
    assert(erase().status=="409 Conflict");
    app.state=kDeviceStateListening;app.Run();app.Run();
    assert(!console.maintenance_active()&&store.erases==0&&tasks.empty());
    app.state=kDeviceStateIdle;assert(erase().status=="202 Accepted");
    app.Run();assert(motion&&player.maintenance&&hub.maintenance&&store.erases==0&&tasks.size()==1);
    player.reader=true;RunTask();app.Run();
    assert(store.erases==0&&!motion&&!player.maintenance&&!hub.maintenance&&!console.erase_ok_);
    player.reader=false;assert(erase().status=="202 Accepted");app.Run();
    assert(store.erases==0);RunTask();assert(store.erases==1&&console.erase_ok_&&!console.maintenance_active());
    app.Run();assert(!motion&&!player.maintenance&&hub.invalidations==1);
    // Failure to create the worker cannot leave the device in maintenance.
    create_fails=true;assert(erase().status=="202 Accepted");app.Run();app.Run();
    assert(!console.maintenance_active()&&!motion&&!player.maintenance&&!hub.maintenance&&store.erases==1);
}
''')

    def test_preview_stop_cannot_reset_a_cloud_reply_after_scheduling(self):
        self.run_cpp(self.console()+r'''
int main(){
    WebConsole console;AudioPlayer player;console.options_.player=&player;
    auto& app=Application::GetInstance();player.playing=true;
    auto r=Request(console,"{}",false);WebConsole::HandleStop(&r);
    app.state=kDeviceStateSpeaking;app.Run();assert(player.stops==0);
    auto cloud=Request(console,"{}",false);WebConsole::HandleStop(&cloud);
    assert(cloud.status=="409 Conflict"&&player.stops==0);
    app.state=kDeviceStateIdle;auto local=Request(console,"{}",false);WebConsole::HandleStop(&local);app.Run();
    assert(player.stops==1);
    auto empty=Request(console,"{}",false);WebConsole::HandleStop(&empty);app.Run();assert(player.stops==1);
}
''')


HARNESS = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "cJSON.h"
#include "wzh_developer_session.h"
using esp_err_t=int;using httpd_handle_t=void*;struct esp_netif_t{};
constexpr int ESP_OK=0,ESP_FAIL=-1,HTTPD_RESP_USE_STRLEN=-1,HTTPD_SOCK_ERR_TIMEOUT=-2,pdPASS=1;
constexpr size_t kMaxBody=1024;
int64_t clock_us=1000000;
int64_t esp_timer_get_time(){return clock_us;}
void esp_fill_random(void* target,size_t size){std::memset(target,0x71,size);}
void vTaskDelay(unsigned ms){clock_us+=ms*1000;}void vTaskDelete(void*){}
#define pdMS_TO_TICKS(ms) (ms)
struct httpd_req_t{
    void* user_ctx=nullptr;std::string body,token,response,status="200 OK";
    int content_len=0,cursor=0,chunk=1024;
};
int httpd_req_recv(httpd_req_t*r,char*out,size_t size){
    int count=std::min({r->content_len-r->cursor,r->chunk,int(size)});
    std::memcpy(out,r->body.data()+r->cursor,count);r->cursor+=count;return count;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t*r,const char*){return r->token.size();}
int httpd_req_get_hdr_value_str(httpd_req_t*r,const char*,char*out,size_t size){
    if(r->token.size()+1>size)return ESP_FAIL;
    std::strcpy(out,r->token.c_str());return ESP_OK;
}
void httpd_resp_set_type(httpd_req_t*,const char*){}void httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){}
void httpd_resp_set_status(httpd_req_t*r,const char*status){r->status=status;}
int httpd_resp_send(httpd_req_t*r,const char*body,int size){r->response.assign(body,size<0?std::strlen(body):size);return ESP_OK;}
int httpd_resp_send_500(httpd_req_t*r){r->status="500 Internal Server Error";return ESP_OK;}
std::vector<std::function<void()>>tasks;bool create_fails=false;
int xTaskCreate(void(*f)(void*),const char*,unsigned,void* arg,int,void*){
    if(create_fails)return 0;
    tasks.push_back([=](){f(arg);});return pdPASS;
}
void RunTask(){auto f=tasks.front();tasks.erase(tasks.begin());f();}
enum DeviceState{kDeviceStateIdle,kDeviceStateListening,kDeviceStateSpeaking};
struct Codec{int volume=70;void SetOutputVolume(int value){volume=value;}};
struct Board{int battery=80;bool GetBatteryLevel(int&level,bool&charging,bool&discharging){level=battery;charging=false;discharging=true;return true;}Codec codec;static Board&GetInstance(){static Board b;return b;}Codec*GetAudioCodec(){return &codec;}};
struct Application{
    DeviceState state=kDeviceStateIdle;std::vector<std::function<void()>>scheduled;
    static Application&GetInstance(){static Application a;return a;}
    DeviceState GetDeviceState(){return state;}
    void Schedule(std::function<void()> f){scheduled.push_back(f);}
    void Run(){auto work=std::move(scheduled);scheduled.clear();for(auto&f:work)f();}
};
namespace wzh {
struct AudioStore{int erases=0;bool writing=false;uint64_t capacity(){return 128000000;}
    bool IsWriting(){return writing;}bool EraseAll(std::string*){++erases;return true;}};
struct AudioPlayer{bool maintenance=false,reader=false,playing=false;int stops=0;
    void SetMaintenance(bool on){maintenance=on;if(on)StopPlayback();}
    bool WorkerIdle(){return !reader;}bool Playing(){return playing;}void StopPlayback(){playing=false;++stops;}};
struct AudioHub{bool running=false,maintenance=false;int invalidations=0;
    bool job_running(){return running;}
    bool BeginLocalMaintenance(){if(running||maintenance)return false;maintenance=true;return true;}
    void EndLocalMaintenance(bool erased){maintenance=false;if(erased)++invalidations;}};
__CLASS__
WebConsole::~WebConsole(){}
__METHODS__
}
using namespace wzh;
httpd_req_t Request(WebConsole&self,const std::string&body,bool auth){
    httpd_req_t r;r.user_ctx=&self;r.body=body;r.content_len=body.size();
    if(auth)r.token=self.developer_.token();
    return r;
}
'''
