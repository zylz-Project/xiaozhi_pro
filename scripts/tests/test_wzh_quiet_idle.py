"""Replay stale sensor levels, own-motion settling and local speaker echoes."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / 'main/boards/common/wzh'
BOARDS = [('wzh_crawlPanda', 'panda'), ('wzh_tailRedPanda', 'panda'),
          ('wzh_dinosaur', 'dinosaur')]


class WzhQuietIdleTest(unittest.TestCase):
    def run_cpp(self, program):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'quiet.cc'
            source.write_text(program)
            executable = source.with_suffix('')
            result = subprocess.run(
                [compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                 '-I', str(COMMON), str(source), '-o', str(executable)],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_presence_never_saves_boot_busy_or_cooldown_edges_for_later(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_pet_behavior.h"
using namespace wzh;
int main(){
    PresenceGreeting presence;uint32_t now=0;
    auto quiet=[&](bool high,bool ready,unsigned duration){
        for(unsigned t=0;t<duration;t+=20)
            assert(!presence.Update(high,now+=20,ready));
    };
    auto arrive=[&](){
        quiet(true,true,180);
        assert(presence.Update(true,now+=20,true));
        presence.Accepted(now);
    };
    quiet(true,false,18000);quiet(true,true,120000); // Startup-high held after readiness.
    quiet(false,true,4000);arrive();
    quiet(true,true,2000);quiet(false,true,4000);
    quiet(true,true,120000); // An arrival during cooldown cannot wait out that cooldown.
    quiet(false,true,4000);quiet(true,false,400);quiet(true,true,120000); // Busy edge discarded.
    quiet(false,true,4000);quiet(true,true,100);quiet(false,true,200); // Short noise pulse.
    arrive();
    quiet(true,true,24U*60U*60U*1000U); // A stationary high level is never another greeting.
    quiet(false,true,4000);quiet(true,true,180);
    assert(presence.Update(true,now+=20,true));
    quiet(true,false,20);quiet(true,true,2000); // Becoming busy cancels even a brief pending edge.
    quiet(false,true,4000);quiet(true,true,180);
    assert(presence.Update(true,now+=20,true));
    // A rejected callback expires; it must not greet seconds later when a task becomes free.
    now+=1000;assert(!presence.Update(true,now,true));quiet(true,true,120000);
}
''')

    def test_static_acceleration_and_post_motion_orientation_never_restart_a_show(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_posture_input.h"
using namespace wzh;
int main(){
    PostureInput vertical;uint32_t vnow=0;
    for(int i=0;i<30;++i)
        assert(vertical.Update(0,0,1000,false,false,vnow+=20)==PetInteraction::kNone);
    vertical.Update(0,0,1230,false,false,vnow+=20);
    vertical.Update(0,0,1230,false,false,vnow+=20);
    assert(vertical.Update(0,0,1230,false,false,vnow+=20)==PetInteraction::kPickedUp);
    vertical.Accepted(vnow);
    for(int i=0;i<1000;++i)
        assert(vertical.Update(0,0,1230,false,false,vnow+=20)==PetInteraction::kNone);
    PostureInput input;uint32_t now=UINT32_MAX-3000;
    auto quiet=[&](int x,int y,int z,int samples){
        for(int i=0;i<samples;++i)
            assert(input.Update(x,y,z,false,false,now+=20)==PetInteraction::kNone);
    };
    quiet(0,0,1230,1000); // Stable sensor offset is not a pickup.
    // Three dynamic samples still detect a real pickup.
    input.Update(220,0,1230,false,false,now+=20);
    input.Update(-220,0,1230,false,false,now+=20);
    input.Update(220,0,1230,false,false,now+=20);
    assert(input.Update(-220,0,1230,false,false,now+=20)==PetInteraction::kPickedUp);
    input.Accepted(now);
    quiet(0,0,1230,200); // Quiet after pickup alone must not infer placement.
    for(int i=0;i<100;++i)
        assert(input.Update(i%2?1500:-1500,0,1000,true,false,now+=20)==PetInteraction::kNone);
    // The pet settles onto a DIFFERENT side after its own animation.
    quiet(0,1200,0,24*60*60*50); // 24 virtual hours, including the 32-bit clock rollover.
    for(int i=0;i<4;++i)
        assert(input.Update(i%2?220:-220,1200,0,false,false,now+=20)==PetInteraction::kNone);
    bool placed=false;
    for(int i=0;i<100;++i){
        auto event=input.Update(0,1200,0,false,false,now+=20);
        if(event!=PetInteraction::kNone){
            assert(event==PetInteraction::kPutDown&&!placed);placed=true;input.Accepted(now);
        }
    }
    assert(placed);quiet(0,1200,0,1000);
    input.Invalidate();quiet(1000,0,0,100); // Recovery learns rest without a surprise flip.
    bool flipped=false;
    for(int i=0;i<10;++i){
        auto event=input.Update(700,700,0,false,false,now+=20);
        if(event!=PetInteraction::kNone){
            assert(event==PetInteraction::kFlipped&&!flipped);flipped=true;input.Accepted(now);
        }
    }
    assert(flipped);
}
''')

    def test_production_sensor_task_drops_posture_observed_during_protection(self):
        source = (COMMON/'wzh_sensors.cc').read_text()
        task = queue_function(source, 'void Sensors::Task(')
        self.run_cpp(r'''
#include <cassert>
#include <functional>
#include <mutex>
#include "wzh_pet_behavior.h"
#include "wzh_posture_input.h"
uint32_t now=0;int gestures=0;
struct Done{};
template<class... T>void log_stub(const T&...){}
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
#define ESP_LOGD(...) log_stub(__VA_ARGS__)
int gpio_get_level(int){return 1;} // Boot-high PIR also must not surprise on recovery.
int64_t esp_timer_get_time(){return int64_t(now)*1000;}
void vTaskDelay(uint32_t ms){now+=ms;if(now>35000)throw Done{};}
#define pdMS_TO_TICKS(ms) (ms)
void i2c_master_bus_rm_device(void*){}
void i2c_del_master_bus(void*){}
namespace wzh {
const char* kTag="test";constexpr int kFalling=4;
int Orientation(int,int,int){return 4;}
struct Output{
    bool blocked(){return now<25000;} // Startup/low voltage prevents all gestures.
    bool powered(){return false;}unsigned faults(){return 0;}void SetFault(int,bool){}
};
struct Sensors{
    enum class Event{kHuman,kPickedUp,kShaken,kFlipped,kPutDown};
    struct Status{
        bool human=false,accelerometer=false,angles_valid=false;
        int x_mg=0,y_mg=0,z_mg=0,orientation=0;float pitch=0,roll=0;
    }status_;
    int human_=41,sda_=43,scl_=44;std::mutex mutex_;
    int token=0;void* bus_=&token;void* device_=&token;
    Output output;Output* output_=&output;
    std::function<bool(Event)>hook_;
    bool InitializeAccelerometer(int,int){return true;}
    bool Read(uint8_t,uint8_t* bytes,size_t){
        int x=now>=1000?1000:0,z=now>=1000?0:1000;
        if(now>=30000&&now<30100)x=now/20%2?1500:-1500;
        const int axes[]={x,0,z};
        for(int i=0;i<3;++i){
            auto raw=static_cast<uint16_t>(static_cast<int16_t>(axes[i]*8));
            bytes[2*i]=raw&255;bytes[2*i+1]=raw>>8;
        }
        return true;
    }
    void Task();
};
'''+task+r'''
}
int main(){
    wzh::Sensors sensor;
    sensor.hook_=[](wzh::Sensors::Event event){
        assert(now>=30000&&event==wzh::Sensors::Event::kShaken);++gestures;return true;
    };
    try{sensor.Task();}catch(Done&){}
    assert(gestures==1&&sensor.status_.accelerometer);
}
''')

    def test_all_three_power_tasks_require_release_and_cancel_failed_adc_contact(self):
        harness = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include "wzh_touch_input.h"
#include "wzh_pet_behavior.h"
template<class... T>void log_stub(const T&...){}
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
#define ESP_LOGD(...) log_stub(__VA_ARGS__)
constexpr int ESP_OK=0,TOUCH_ADC_CHANNEL=4;
const char* TAG="test";
int voltage=410;bool failed=false;
int adc_oneshot_read(int,int,int* sample){*sample=voltage;return failed?-1:ESP_OK;}
int AdcToMv(int,int raw){return raw;}
struct Power{
    int adc_=0,touch_adc_cali_=0;
    std::atomic<int>touch_voltage_mv_{0},touch_code_{0};
    std::atomic<bool>touch_valid_{false},touch_hook_ready_{true};
    wzh::TouchInputFilter touch_filter{5};
    std::function<void(uint8_t,uint8_t,uint8_t)>touch_hook_;
    void Step(){__STEP__}
};
int main(){
    Power power;wzh::PetBehavior pet;uint32_t now=0;int replies=0,cancels=0;
    power.touch_hook_=[&](uint8_t code,uint8_t,uint8_t){pet.Touch(code,now);if(code==0xFF)++cancels;};
    auto sample=[&](int mv,int count){voltage=mv;for(int i=0;i<count;++i){
        now+=20;power.Step();if(pet.PollTouch(now)!=wzh::TouchResponse::kNone)++replies;
    }};
    sample(410,200);assert(replies==0); // Stuck high on boot is status, not a press.
    sample(0,5);sample(410,5);assert(replies==1);
    failed=true;sample(410,5);assert(cancels==1&&!power.touch_valid_.load());
    failed=false;sample(410,200);assert(replies==1); // No phantom held/released event.
    sample(0,5);sample(410,5);assert(replies==2);
    sample(0,5);sample(410,2);sample(0,5);assert(replies==2); // Short glitch rejected.
}
'''
        for board, stem in BOARDS:
            source = (ROOT / 'main/boards' / board / (stem+'_power.cc')).read_text()
            start = source.index('        // IO5 上')
            end = source.index('        // UI uses rest voltage;', start)
            with self.subTest(board=board):
                self.run_cpp(harness.replace('__STEP__', source[start:end]))

    def test_echo_guard_covers_pending_output_decoder_tail_and_recent_pcm(self):
        source = (COMMON/'wzh_audio_player.cc').read_text()
        method = queue_function(source, 'bool AudioPlayer::LocalCommandEchoRisk(')
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <mutex>
#include <cstdint>
int64_t now=1000000;
int64_t esp_timer_get_time(){return now;}
struct Service{bool idle=true;bool IsPlaybackIdle(){return idle;}};
struct Application{
    Service service;static Application& GetInstance(){static Application a;return a;}
    Service& GetAudioService(){return service;}
};
struct AudioPlayer{
    std::mutex playback_mutex_;
    bool playback_pending_=false;
    std::atomic<bool>is_playing_{false};
    std::atomic<int64_t>tts_updated_us_{0};
    bool LocalCommandEchoRisk();
};
'''+method+r'''
int main(){
    AudioPlayer player;assert(!player.LocalCommandEchoRisk());
    player.playback_pending_=true;assert(player.LocalCommandEchoRisk());
    player.playback_pending_=false;player.is_playing_=true;assert(player.LocalCommandEchoRisk());
    player.is_playing_=false;auto& service=Application::GetInstance().service;
    service.idle=false;assert(player.LocalCommandEchoRisk());service.idle=true;
    player.tts_updated_us_=now;assert(player.LocalCommandEchoRisk());
    now+=349999;assert(player.LocalCommandEchoRisk());
    ++now;assert(!player.LocalCommandEchoRisk());
}
''')

    def test_all_three_board_routes_block_echo_actions_but_preserve_stop_and_primary_wake(self):
        harness = r'''
#include <cassert>
#include <cstring>
#include <string>
#include "wzh_pet_behavior.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
enum class Action{kHappy,kAffection,kEat};using PandaAction=Action;using DinoAction=Action;
enum class Body{kShakeHead,kLookLeft,kLookRight};using PandaBodyAction=Body;using DinoBodyAction=Body;
struct Controller{
    int actions=0,bodies=0,controls=0;
    wzh::PetControl last=wzh::PetControl::kNone;
    bool HandlePetControl(wzh::PetControl control,bool respond=true){
        (void)respond;
        if(control==wzh::PetControl::kNone)return false;
        last=control;++controls;return true;
    }
    bool IsConversationActive(){return false;}
    void TriggerAction(Action,bool){++actions;}
    void TriggerBodyAction(Body){++bodies;}
    bool HandleBodyCommand(const std::string& name){
        if(name=="shake_head"||name=="look_left"||name=="look_right"){++bodies;return true;}
        return false;
    }
};
struct Audio{bool echo=true;void StopPlayback(){} bool LocalCommandEchoRisk(){return echo;}};
struct Board{Controller controller_;Audio local_audio_;__ROUTE__};
int main(){
    Board board;
    for(auto command:{"local:happy","local:affection","local:eat","local:shake_head",
                      "local:look_left","local:look_right","local:wake"})
        assert(board.HandleLocalVoiceCommand(command));
    assert(board.controller_.actions==0&&board.controller_.bodies==0&&board.controller_.controls==0);
    for(auto command:{"local:stop","local:quiet","local:rest","local:sleepy"})
        assert(board.HandleLocalVoiceCommand(command));
    assert(board.controller_.controls==4);
    for(auto wake:{"你好小安","你好星宝","小龙小龙"}){
        assert(!board.HandleLocalVoiceCommand(wake));
        assert(board.controller_.last==wzh::PetControl::kWake);
    }
    board.local_audio_.echo=false;
    assert(board.HandleLocalVoiceCommand("local:happy"));
    assert(board.HandleLocalVoiceCommand("local:shake_head"));
    assert(board.HandleLocalVoiceCommand("local:wake"));
    assert(board.controller_.actions==1&&board.controller_.bodies==1&&board.controller_.controls==8);
}
'''
        for board, _ in BOARDS:
            source = (ROOT / 'main/boards' / board / (board+'.cc')).read_text()
            route = queue_function(source, 'bool HandleLocalVoiceCommand(').replace(' override', '')
            with self.subTest(board=board):
                self.run_cpp(harness.replace('__ROUTE__', route))


if __name__ == '__main__':
    unittest.main()
