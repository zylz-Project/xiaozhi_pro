"""Exercise directional touch and the deferred text input on a live conversation."""
from pathlib import Path
import unittest

import test_firmware_runtime_optimizations as runtime
from test_wzh_action_queue import queue_function


ROOT = Path(__file__).resolve().parents[2]


class WzhConversationTouchTest(unittest.TestCase):
    run_cpp = runtime.FirmwareRuntimeOptimizationsTest.run_cpp

    def test_dinosaur_back_stroke_resolves_once_and_preserves_other_touches(self):
        header = ROOT / 'main/boards/common/wzh/wzh_conversation_touch.h'
        touch_input = header.with_name('wzh_touch_input.h')
        self.run_cpp(f'#include "{header}"\n#include "{touch_input}"\n' + r'''
#include <cassert>
using namespace wzh;
int main(){
    PetBehavior dino(true);
    dino.Touch(4,1000);assert(dino.PollTouch(1000)==TouchResponse::kNone);
    assert(dino.PollTouch(999)==TouchResponse::kNone); // A motion tick sampled just before the ADC edge.
    dino.Touch(12,1170,false); // The second pad rose during the same contact.
    auto event=dino.PollTouchEvent(1170);
    assert(event.response==TouchResponse::kStrokeForward&&event.mask==12);
    assert(event.interaction()==PetInteraction::kHeld);
    assert(ConversationTouchText(event,{"头","胸前","前背","后背"}).find("顺摸")!=std::string::npos);
    dino.Touch(8,1300);assert(dino.PollTouch(2900)==TouchResponse::kNone);
    dino.Touch(0,3000);assert(dino.PollTouch(3000)==TouchResponse::kNone);

    dino.Touch(8,3300);dino.Touch(0,3430);dino.Touch(4,3550);
    event=dino.PollTouchEvent(3550);
    assert(event.response==TouchResponse::kStrokeReverse&&event.interaction()==PetInteraction::kPlayful);
    dino.Touch(0,3600);

    dino.Touch(12,4000);assert(dino.PollTouch(4000)==TouchResponse::kPlayful);
    dino.Touch(4,4050);assert(dino.PollTouch(4050)==TouchResponse::kNone);
    dino.Touch(0,4120);dino.Touch(4,4500);dino.Touch(0,4620);
    event=dino.PollTouchEvent(4860);assert(event.response==TouchResponse::kAffection&&event.mask==4);
    dino.Touch(8,5100);dino.Touch(255,5220);dino.Touch(4,5340);
    assert(dino.PollTouch(5340)==TouchResponse::kNone);
    dino.ResetTouch();assert(dino.PollTouch(6000)==TouchResponse::kNone);
    dino.Touch(0,6050);dino.Touch(4,6200);
    assert(dino.PollTouch(6850)==TouchResponse::kAffection);
    assert(dino.PollTouch(22000)==TouchResponse::kNone);
    dino.Touch(0,22100);assert(dino.PollTouch(22100)==TouchResponse::kNone);

    PetBehavior panda;
    panda.Touch(4,1000);assert(panda.PollTouch(1000)==TouchResponse::kAffection);
    panda.Touch(8,1140);assert(panda.PollTouch(1140)==TouchResponse::kNone);

    // A short gap between back pads can be shorter than the ordinary
    // release debounce. The new pad still provides the directional edge.
    for(bool forward : {false, true}) {
        PetBehavior filtered(true);
        TouchInputFilter input(5);
        uint32_t now=0;
        const auto feed=[&](int voltage, int count) {
            for(int i=0;i<count;++i){
                now+=20;
                TouchInputFilter::Event event{};
                if(input.Update(voltage,now,event)&&input.AcceptInteraction(event))
                    filtered.Touch(event.code,now,event.StartsContact());
            }
        };
        feed(0,21);feed(forward?780:1629,6);
        assert(filtered.PollTouch(now)==TouchResponse::kNone);
        feed(0,8);feed(forward?1629:780,5);
        assert(filtered.PollTouch(now)==(forward?TouchResponse::kStrokeForward
                                              :TouchResponse::kStrokeReverse));
        feed(0,21);
        assert(filtered.PollTouch(now)==TouchResponse::kNone);
    }
    // A pause in ADC updates can hide the end of the first contact. A
    // different pad after the pause must not complete a directional stroke.
    PetBehavior recovered(true);
    TouchInputFilter input(5);
    auto sample = [&](int voltage, uint32_t now) {
        TouchInputFilter::Event event{};
        if (!input.Update(voltage, now, event))
            return;
        if (input.AcceptInteraction(event))
            recovered.Touch(event.code, now, event.StartsContact());
        else if (event.code < 0)
            recovered.Touch(0xFF, now, false);
    };
    for(uint32_t t=20;t<=420;t+=20)sample(0,t);
    for(uint32_t t=440;t<=520;t+=20)sample(780,t);
    assert(recovered.PollTouch(520)==TouchResponse::kNone);
    for(uint32_t t=680;t<=760;t+=20)sample(1629,t);
    assert(recovered.PollTouch(760)==TouchResponse::kNone);
    for(uint32_t t=780;t<=1180;t+=20)sample(0,t);
    for(uint32_t t=1200;t<=1940;t+=20)sample(780,t);
    assert(recovered.PollTouch(1940)==TouchResponse::kAffection);
}
''')

    def test_single_back_taps_keep_their_position_and_survive_a_new_press(self):
        header = ROOT / 'main/boards/common/wzh/wzh_conversation_touch.h'
        self.run_cpp(f'#include "{header}"\n' + r'''
#include <cassert>
using namespace wzh;
int main(){
    for (uint8_t pad : {4, 8}) {
        PetBehavior dino(true);
        const auto expected = pad == 4 ? PetInteraction::kTouch3 : PetInteraction::kTouch4;
        dino.Touch(pad,1000);
        dino.Touch(0,1110);
        assert(dino.PollTouch(1109)==TouchResponse::kNone);
        assert(dino.PollTouch(1110)==TouchResponse::kNone);
        dino.Touch(pad,1240); // Another tap on the same pad, inside the swipe gap.
        auto first=dino.PollTouchEvent(1240);
        assert(first.interaction()==expected && first.mask==pad);
        assert(ConversationTouchText(first,{"头","胸前","前背","后背"}).find(
            pad==4?"前背":"后背")!=std::string::npos);
        assert(dino.PollTouch(1250)==TouchResponse::kNone);
        dino.Touch(0,1400);
        assert(dino.PollTouchEvent(1631).interaction()==expected);
        assert(dino.PollTouch(2000)==TouchResponse::kNone);
        dino.Touch(pad,2800);
        dino.Touch(0,2910);
        dino.Touch(1,3020);
        assert(dino.PollTouchEvent(3020).interaction()==expected);
        assert(dino.PollTouchEvent(3040).interaction()==PetInteraction::kTouch1);
        assert(dino.PollTouch(3500)==TouchResponse::kNone);
    }
}
''')

    def test_touch_waits_for_conversation_gap_and_is_discarded_after_session_change(self):
        source = (ROOT / 'main/application.cc').read_text()
        methods = '\n'.join(queue_function(source, 'void Application::' + name + '(')
                            for name in ('QueueTouchConversationInput', 'SendPendingTouchConversationInput'))
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
enum { kDeviceStateIdle,kDeviceStateListening,kDeviceStateSpeaking };
int64_t now_us=2000000;
int64_t esp_timer_get_time(){return now_us;}
struct Board {
 bool online=true,maintenance=false;
 static Board& GetInstance(){static Board b;return b;}
 bool IsNetworkAvailable(){return online;}bool IsAudioMaintenanceActive(){return maintenance;}
};
struct Audio {
 bool voice=false,idle=true;
 bool IsVoiceDetected(){return voice;}bool IsPlaybackIdle(){return idle;}
};
struct Protocol {
 bool open=true;std::vector<std::string> received;
 bool IsAudioChannelOpened(){return open;}
 bool SendListenText(const std::string& text){received.push_back(text);return true;}
};
struct Application {
 struct TouchConversationInput {std::string text;uint32_t interaction_generation=0,protocol_generation=0;
                                int64_t received_us=0;};
 std::mutex touch_input_mutex_;TouchConversationInput pending_touch_input_;
 int64_t next_touch_input_us_=0;std::atomic<int64_t> last_listening_voice_edge_us_{0};
 std::atomic<uint32_t> interaction_generation_{1},protocol_generation_{1};
 std::atomic<bool> interaction_stopped_{false},voice_enrollment_active_{false};
 std::atomic<bool> external_voice_preview_active_{false},tts_start_pending_{false};
 std::atomic<bool> barge_in_reconnecting_{false},voice_enrollment_reconnecting_{false};
 Protocol transport;Protocol* protocol_=&transport;Audio audio_service_;
 bool pending_listening_start_=false,shutdown=false;
 int state=kDeviceStateIdle;
 int GetDeviceState(){return state;}bool IsShuttingDown(){return shutdown;}
 void QueueTouchConversationInput(std::string);
 void SendPendingTouchConversationInput();
};
''' + methods + r'''
int main(){
 Application app;
 app.QueueTouchConversationInput("offline");assert(app.pending_touch_input_.text.empty());
 app.state=kDeviceStateSpeaking;app.QueueTouchConversationInput("head");
 now_us+=800000;app.SendPendingTouchConversationInput();assert(app.transport.received.empty());
 app.state=kDeviceStateListening;app.audio_service_.idle=false;
 app.QueueTouchConversationInput("tail");now_us+=800000;
 app.SendPendingTouchConversationInput();assert(app.transport.received.empty());
 app.audio_service_.idle=true;app.audio_service_.voice=true;
 app.SendPendingTouchConversationInput();assert(app.transport.received.empty());
 app.audio_service_.voice=false;app.last_listening_voice_edge_us_=now_us;
 now_us+=1000000;app.SendPendingTouchConversationInput();assert(app.transport.received.empty());
 now_us+=250000;app.SendPendingTouchConversationInput();
 assert((app.transport.received==std::vector<std::string>{"tail"}));

 app.QueueTouchConversationInput("previous interaction");++app.interaction_generation_;
 now_us+=4000000;app.SendPendingTouchConversationInput();assert(app.pending_touch_input_.text.empty());
 app.QueueTouchConversationInput("previous protocol");++app.protocol_generation_;
 now_us+=500000;app.SendPendingTouchConversationInput();assert(app.pending_touch_input_.text.empty());
 app.QueueTouchConversationInput("expired");now_us+=7000000;
 app.SendPendingTouchConversationInput();assert(app.pending_touch_input_.text.empty());
 app.QueueTouchConversationInput("disconnected");Board::GetInstance().online=false;
 now_us+=500000;app.SendPendingTouchConversationInput();assert(app.transport.received.size()==1);
 Board::GetInstance().online=true;app.QueueTouchConversationInput("fresh");
 now_us+=500000;app.SendPendingTouchConversationInput();assert(app.transport.received.back()=="fresh");
}
''')

    def test_text_turn_uses_shared_listen_envelope_and_escapes_text(self):
        source = (ROOT / 'main/protocols/protocol.cc').read_text()
        methods = '\n'.join(queue_function(source, marker) for marker in (
            'void Protocol::SendWakeWordDetected(', 'bool Protocol::SendListenText('))
        self.run_cpp(r'''
#include <cassert>
#include <memory>
#include <string>
#include "cJSON.h"
struct Protocol {
 std::string session_id_="session",sent;
 std::string session_id()const{return session_id_;}
 bool SendText(const std::string& text){sent=text;return true;}
 void SendWakeWordDetected(const std::string&);
 bool SendListenText(const std::string&);
};
''' + methods + r'''
int main(){
 Protocol protocol;
 const std::string text="设备感应到用户摸了你的\"头\"。";
 assert(protocol.SendListenText(text));
 auto* json=cJSON_Parse(protocol.sent.c_str());assert(json);
 assert(std::string(cJSON_GetObjectItem(json,"session_id")->valuestring)=="session");
 assert(std::string(cJSON_GetObjectItem(json,"type")->valuestring)=="listen");
 assert(std::string(cJSON_GetObjectItem(json,"state")->valuestring)=="detect");
 assert(std::string(cJSON_GetObjectItem(json,"text")->valuestring)==text);
 cJSON_Delete(json);
 protocol.SendWakeWordDetected("你好小安");assert(protocol.sent.find("你好小安")!=std::string::npos);
}
''', use_json=True)
