"""Run the production double-click lifecycle and speaker handoff with task substitutes."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
JSON = ROOT/'managed_components/espressif__cjson/cJSON'

APP_HARNESS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <cstring>
#include "cJSON.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
enum DeviceState{kDeviceStateIdle,kDeviceStateConnecting,kDeviceStateListening,kDeviceStateSpeaking,
    kDeviceStateStarting,kDeviceStateWifiConfiguring,kDeviceStateActivating,kDeviceStateUpgrading,kDeviceStateAudioTesting};
using ListeningMode=int;
constexpr int kListeningModeManualStop=1,kListeningModeAutoStop=2,kListeningModeRealtime=3,kAbortReasonNone=0;
enum class PowerSaveLevel{PERFORMANCE,LOW_POWER};
namespace Lang{namespace Sounds{constexpr int OGG_POPUP=1;}namespace Strings{constexpr int SPEAKING=2,LISTENING=3;}}
struct AudioStreamPacket{};
struct TextGlyph{};
struct TextGlyphPayload{static bool Parse(const cJSON*,std::vector<TextGlyph>&,uint8_t&){return false;}};
std::vector<std::string> events;
struct Audio{
    uint32_t generation=0;int packets=0,prompts=0,resets=0;
    bool voice=false,wake=false,playback_idle=true,processor_running=false;
    uint32_t GetPlaybackGeneration(){return generation;}
    void ResetDecoder(){++generation;++resets;packets=0;events.push_back("reset");}
    void EnableVoiceProcessing(bool enabled){voice=enabled;if(enabled)ResetDecoder();}
    void EnableWakeWordDetection(bool enabled){wake=enabled;}
    bool IsAfeWakeWord(){return true;}
    bool IsPlaybackIdle(){return playback_idle;}
    bool IsAudioProcessorRunning(){return processor_running;}
    void MarkDrained(){playback_idle=true;}
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>,bool,std::optional<uint32_t> expected=std::nullopt){
        if(expected&&*expected!=generation){return false;}
        ++packets;return true;
    }
    int send_packets=0;
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue(){
        if(send_packets>0){--send_packets;return std::make_unique<AudioStreamPacket>();}
        return nullptr;
    }
    void PlaySound(int){++prompts;playback_idle=false;events.push_back("prompt");}
};
struct Display{
    int messages=0,emotions=0;
    void SetEmotion(const char*){++emotions;}
    void SetChatMessage(const char*,const char*){++messages;}
    void AddTextGlyphs(const std::vector<TextGlyph>&,uint8_t){}
    void SetStatus(int){}
};
struct Board{
    bool maintenance=false;bool IsAudioMaintenanceActive(){return maintenance;}
    Display display;
    static Board& GetInstance(){static Board b;return b;}
    void SetPowerSaveLevel(PowerSaveLevel){}
    Display* GetDisplay(){return &display;}
};
struct Protocol{
    bool opened=false,open_succeeds=true,deferred_close=false;int opens=0,closes=0,listens=0;
    std::vector<std::function<void()>>delayed_closes;
    std::function<void()> during_open,closed;
    std::function<void(std::unique_ptr<AudioStreamPacket>)>audio;
    bool IsAudioChannelOpened(){return opened;}
    bool OpenAudioChannel(){++opens;if(during_open)during_open();opened=open_succeeds;return opened;}
    void CloseAudioChannel(){opened=false;++closes;if(closed){if(deferred_close)delayed_closes.push_back(closed);else closed();}}
    void DeliverCloses(){auto work=std::move(delayed_closes);delayed_closes.clear();for(auto&f:work)f();}
    void SendStartListening(int){++listens;}
    void OnIncomingAudio(decltype(audio) callback){audio=callback;}
    void OnAudioChannelClosed(decltype(closed) callback){closed=callback;}
};
constexpr unsigned MAIN_EVENT_TOGGLE_CONVERSATION=1;
class Application;
unsigned pending_bits=0;
void xEventGroupSetBits(void*,unsigned bits){pending_bits|=bits;}
class Application{
public:
    DeviceState state=kDeviceStateIdle;
    Audio audio_service_;
    Protocol protocol;
    Protocol*protocol_=&protocol;
    std::atomic<bool>interaction_stopped_{false};
    std::atomic<uint32_t>interaction_generation_{0};
    bool play_popup_on_listening_=false,pending_listening_start_=false,aborted_=false;
    bool entry_prompt_before_listening_=false;
    static constexpr size_t kMaxEarlyAudioPackets=8;
    std::mutex early_audio_mutex_;
    std::deque<std::unique_ptr<AudioStreamPacket>> early_audio_;
    std::atomic<bool> tts_start_pending_{false};
    int listening_mode_=kListeningModeAutoStop,aborts=0,local_stops=0;
    void*event_group_=this;
    std::vector<std::function<void()>>scheduled;
    DeviceState GetDeviceState(){return state;}
    bool SetDeviceState(DeviceState next){
        if(next==state)return true;
        if(next==kDeviceStateConnecting){++local_stops;audio_service_.ResetDecoder();events.push_back("local_stopped");}
        state=next;events.push_back(next==kDeviceStateIdle?"idle":"state");return true;
    }
    ListeningMode GetDefaultListeningMode(){return kListeningModeAutoStop;}
    void Schedule(std::function<void()>callback){scheduled.push_back(callback);}
    void Drain(){while(!scheduled.empty()){auto work=std::move(scheduled);scheduled.clear();for(auto&f:work)f();}}
    void ConfigureWakeWordForListening(){audio_service_.wake=true;}
    void AbortSpeaking(int){++aborts;}
    void ToggleConversation();void HandleToggleConversationEvent();
    void ContinueOpenAudioChannel(ListeningMode);void StartListeningAudio();void SetListeningMode(ListeningMode);
    void FlushEarlyAudio();void ClearEarlyAudio();
    void StopInteraction(bool play_exit_sound=false);
    void Button(){ToggleConversation();if(pending_bits){pending_bits=0;HandleToggleConversationEvent();}}
    void Install(){auto&board=Board::GetInstance();__AUDIO_CALLBACK__;__CLOSE_CALLBACK__;}
    void Tts(const char* body){
        auto holder=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>(cJSON_Parse(body),cJSON_Delete);
        auto*root=holder.get();assert(root);auto*type=cJSON_GetObjectItem(root,"type");
        auto*display=Board::GetInstance().GetDisplay();
        __TTS_BRANCH__
    }
    void SpeakingStateEvent(){auto*display=Board::GetInstance().GetDisplay();
        switch(state){__SPEAKING_CASE__ default:break;}
    }
    void ListeningStateEvent(){auto*display=Board::GetInstance().GetDisplay();
        switch(state){__LISTENING_CASE__ default:break;}
    }
};
__METHODS__
'''


class WzhConversationToggleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('g++') or not shutil.which('gcc'):
            raise unittest.SkipTest('host C/C++ compilers required')
        cls.temp=tempfile.TemporaryDirectory()
        cls.json=Path(cls.temp.name)/'json.o'
        subprocess.run(['gcc','-c',str(JSON/'cJSON.c'),'-o',str(cls.json)],check=True,capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_cpp(self, program):
        with tempfile.TemporaryDirectory() as directory:
            cpp=Path(directory)/'test.cc';cpp.write_text(program)
            exe=cpp.with_suffix('')
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-I',str(JSON),str(cpp),str(self.json),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr)

    def application(self):
        source=(ROOT/'main/application.cc').read_text()
        methods='\n'.join(queue_function(source,'void Application::'+name+'(') for name in [
            'ToggleConversation','HandleToggleConversationEvent','ContinueOpenAudioChannel',
            'StopInteraction','StartListeningAudio','SetListeningMode','FlushEarlyAudio',
            'ClearEarlyAudio'])
        audio=queue_function(source,'protocol_->OnIncomingAudio(')
        closed=queue_function(source,'protocol_->OnAudioChannelClosed(')
        begin=source.index('        if (strcmp(type->valuestring, "tts") == 0)')
        end=source.index(' else if (strcmp(type->valuestring, "mcp")',begin)
        tts=source[begin:end]
        handle=source.index('void Application::HandleStateChangedEvent')
        begin=source.index('        case kDeviceStateListening:',handle)
        end=source.index('        case kDeviceStateSpeaking:',begin)
        listening=source[begin:end]
        begin=source.index('        case kDeviceStateSpeaking:',handle)
        end=source.index('        case kDeviceStateStarting:',begin)
        return (APP_HARNESS.replace('__METHODS__',methods).replace('__AUDIO_CALLBACK__',audio+')')
            .replace('__CLOSE_CALLBACK__',closed+')').replace('__TTS_BRANCH__',tts)
            .replace('__LISTENING_CASE__',listening)
            .replace('__SPEAKING_CASE__',source[begin:end]))

    def test_enter_and_exit_from_listening_speaking_or_connecting_and_reenter(self):
        self.run_cpp(self.application()+r'''
int main(){
    for(bool deferred:{false,true})for(auto active:{kDeviceStateListening,kDeviceStateSpeaking,kDeviceStateConnecting}){
        Application app;app.Install();events.clear();app.protocol.deferred_close=deferred;
        app.Button();assert(app.state==kDeviceStateConnecting&&app.audio_service_.prompts==0);
        app.Drain();assert(app.state==kDeviceStateListening&&app.local_stops==1);
        assert(app.audio_service_.prompts==0); // Wait for actual listening setup.
        app.StartListeningAudio();assert(app.audio_service_.prompts==1&&app.audio_service_.voice);
        assert(events[events.size()-2]=="reset"&&events.back()=="prompt");
        app.state=active;app.protocol.opened=true;app.pending_listening_start_=true;
        app.play_popup_on_listening_=true;
        app.Button();assert(app.interaction_stopped_); // Immediate downlink gate before main task runs.
        app.Drain();assert(app.state==kDeviceStateIdle&&!app.audio_service_.voice);
        assert(app.protocol.closes==1&&app.aborts==1&&app.audio_service_.prompts==2);
        assert(!app.pending_listening_start_&&!app.play_popup_on_listening_);
        assert(events[events.size()-2]=="prompt"&&events.back()=="idle");
        app.Button();app.protocol.DeliverCloses();app.Drain();app.StartListeningAudio();
        assert(app.state==kDeviceStateListening&&!app.interaction_stopped_&&app.audio_service_.prompts==3);
    }
    for(auto state:{kDeviceStateStarting,kDeviceStateWifiConfiguring,kDeviceStateActivating,kDeviceStateUpgrading,kDeviceStateAudioTesting}){
        Application app;app.state=state;app.Button();app.Drain();assert(app.state==state&&app.protocol.opens==0&&app.audio_service_.prompts==0);
    }
    Application open;open.Install();open.protocol.opened=true;open.Button();open.Drain();
    assert(open.local_stops==1&&open.protocol.opens==0&&open.state==kDeviceStateListening);
}
''')

    def test_ready_prompt_plays_before_non_realtime_microphone_and_upload(self):
        self.run_cpp(self.application()+r'''
int main(){
    // Auto-stop: the ready prompt is queued first, but the microphone and
    // START_LISTENING wait for the prompt's actual playback to drain.
    Application prompt;prompt.Install();prompt.Button();prompt.Drain();
    assert(prompt.state==kDeviceStateListening&&prompt.play_popup_on_listening_);
    prompt.ListeningStateEvent();
    assert(prompt.audio_service_.prompts==1&&!prompt.audio_service_.voice);
    assert(prompt.pending_listening_start_&&!prompt.play_popup_on_listening_);
    assert(prompt.protocol.listens==0);
    prompt.audio_service_.MarkDrained();
    assert(prompt.pending_listening_start_);
    prompt.pending_listening_start_=false; // The Run loop clears it before starting.
    prompt.StartListeningAudio();
    assert(prompt.audio_service_.voice&&prompt.protocol.listens==1);
    assert(prompt.audio_service_.prompts==1&&!prompt.pending_listening_start_);

    // Wake-word entry keeps the original XiaoZhi simultaneous listen/play path.
    Application wake;wake.Install();wake.Button();wake.Drain();
    wake.entry_prompt_before_listening_=false; // Popup came from a wake word.
    wake.ListeningStateEvent();
    assert(wake.audio_service_.voice&&wake.audio_service_.prompts==1);
    assert(wake.protocol.listens==1&&!wake.pending_listening_start_);

    // Realtime AEC keeps XiaoZhi's simultaneous listen/play behavior.
    Application realtime;realtime.Install();realtime.Button();realtime.Drain();
    realtime.listening_mode_=kListeningModeRealtime; // AEC-on default mode
    realtime.ListeningStateEvent();
    assert(realtime.audio_service_.voice&&realtime.audio_service_.prompts==1);
    assert(realtime.protocol.listens==1&&!realtime.pending_listening_start_);

    // Auto-stop TTS tail without a prompt: still wait for the tail to drain.
    Application tail;tail.Install();tail.Button();tail.Drain();
    tail.entry_prompt_before_listening_=false; // TTS stop, not double-click entry.
    tail.play_popup_on_listening_=false;tail.audio_service_.playback_idle=false;
    tail.ListeningStateEvent();
    assert(tail.pending_listening_start_&&!tail.audio_service_.voice);
    assert(tail.audio_service_.prompts==0&&tail.protocol.listens==0);
    tail.audio_service_.MarkDrained();tail.StartListeningAudio();
    assert(tail.audio_service_.voice&&tail.protocol.listens==1);
}
''')

    def test_first_tts_packet_survives_pending_start_and_stop_drops_uplink(self):
        self.run_cpp(self.application()+r'''
int main(){
    Application app;app.Install();app.Button();app.Drain();
    app.Tts(R"({"type":"tts","state":"start"})");
    app.protocol.audio(std::make_unique<AudioStreamPacket>());
    assert(app.audio_service_.packets==0); // Buffered until the start transition runs.
    app.Drain();
    assert(app.state==kDeviceStateSpeaking&&app.audio_service_.packets==1);
    app.protocol.audio(std::make_unique<AudioStreamPacket>());
    assert(app.audio_service_.packets==2);
    app.audio_service_.send_packets=3;
    app.StopInteraction();
    assert(app.audio_service_.send_packets==0); // Pending uplink is dropped at once.
    app.Drain();
    assert(app.state==kDeviceStateIdle&&app.audio_service_.packets==0);
    app.Tts(R"({"type":"tts","state":"start"})");app.Drain();
    assert(app.state==kDeviceStateIdle); // A late start must not resurrect Idle.
}
''')

    def test_short_reply_start_audio_and_stop_in_one_batch_preserves_the_prefix(self):
        self.run_cpp(self.application()+r'''
int main(){
    Application app;app.Install();app.Button();app.Drain();
    app.Tts(R"({"type":"tts","state":"start"})");
    app.protocol.audio(std::make_unique<AudioStreamPacket>());
    app.Tts(R"({"type":"tts","state":"stop"})");
    assert(app.early_audio_.size()==1);
    app.Drain();
    assert(app.state==kDeviceStateListening&&app.audio_service_.packets==1);
    Application cancelled;cancelled.Install();cancelled.Button();cancelled.Drain();
    cancelled.Tts(R"({"type":"tts","state":"start"})");
    cancelled.protocol.audio(std::make_unique<AudioStreamPacket>());
    cancelled.Tts(R"({"type":"tts","state":"stop"})");
    cancelled.ToggleConversation();cancelled.Drain();
    assert(cancelled.state==kDeviceStateIdle&&cancelled.audio_service_.packets==0);
}
''')

    def test_flash_maintenance_blocks_new_conversation_and_recovers_after_release(self):
        self.run_cpp(self.application()+r'''
int main(){
    Application app;app.Install();auto&board=Board::GetInstance();
    board.maintenance=true;app.Button();app.Drain();
    assert(app.state==kDeviceStateIdle&&app.protocol.opens==0&&app.audio_service_.prompts==0);
    board.maintenance=false;app.Button();app.Drain();
    assert(app.state==kDeviceStateListening&&app.protocol.opens==1);
    board.maintenance=true;app.ToggleConversation();app.Drain();
    assert(app.state==kDeviceStateIdle&&!app.protocol.opened);
}
''')

    def test_cancel_landing_after_open_check_is_not_cleared_by_listening_setup(self):
        self.run_cpp(self.application()+r'''
int main(){
    Application app;app.Install();app.Button();app.Drain();
    assert(app.state==kDeviceStateListening&&app.protocol.opened);
    // Reproduce the window between ContinueOpenAudioChannel's stop check and
    // SetListeningMode: the cancel is already pending when setup runs.
    app.state=kDeviceStateConnecting;app.StopInteraction();
    app.SetListeningMode(kListeningModeAutoStop);
    app.Drain();
    assert(app.state==kDeviceStateIdle&&!app.audio_service_.voice);
    assert(app.protocol.closes==1&&!app.protocol.opened);
}
''')

    def test_cancel_during_handshake_and_failed_open_do_not_start_microphone_or_entry_prompt(self):
        self.run_cpp(self.application()+r'''
int main(){
    Application early;early.Install();early.Button();early.ToggleConversation();early.Drain();
    assert(early.state==kDeviceStateIdle&&early.protocol.opens==0&&!early.audio_service_.voice);
    Application cancelled;cancelled.Install();
    cancelled.protocol.during_open=[&]{cancelled.ToggleConversation();assert(cancelled.interaction_stopped_);};
    cancelled.Button();cancelled.Drain();
    assert(cancelled.state==kDeviceStateIdle&&!cancelled.audio_service_.voice&&cancelled.protocol.closes==1);
    assert(cancelled.audio_service_.prompts==0&&!cancelled.play_popup_on_listening_);
    Application failure;failure.Install();failure.protocol.open_succeeds=false;
    failure.Button();failure.Drain();assert(failure.state==kDeviceStateIdle&&failure.audio_service_.prompts==0);
    failure.protocol.open_succeeds=true;failure.Button();failure.Drain();failure.StartListeningAudio();
    assert(failure.state==kDeviceStateListening&&failure.audio_service_.prompts==1);
}
''')

    def test_old_tts_close_and_stop_work_cannot_break_a_new_session_or_drop_first_syllable(self):
        self.run_cpp(self.application()+r'''
int main(){
    Application app;app.Install();app.Button();app.Drain();app.StartListeningAudio();
    app.Tts(R"({"type":"tts","state":"start"})");
    app.Tts(R"({"type":"tts","state":"sentence_start","text":"old"})");
    app.Tts(R"({"type":"stt","text":"old user speech"})");
    app.Tts(R"({"type":"llm","emotion":"happy"})");
    int emotions=Board::GetInstance().display.emotions;
    app.state=kDeviceStateSpeaking;app.Button(); // Queue old TTS first, then cancel it.
    int before=Board::GetInstance().display.messages;
    app.Drain();assert(app.state==kDeviceStateIdle);
    assert(Board::GetInstance().display.emotions==emotions);
    assert(Board::GetInstance().display.messages==before+1); // Close clears system text; old sentence is discarded.
    app.Button();app.Drain();app.StartListeningAudio();
    app.Tts(R"({"type":"tts","state":"start"})");app.Drain();
    assert(app.state==kDeviceStateSpeaking);
    app.protocol.audio(std::make_unique<AudioStreamPacket>());assert(app.audio_service_.packets==1);
    app.SpeakingStateEvent();assert(app.audio_service_.packets==1); // No reset after Speaking already admitted audio.
    int resets=app.audio_service_.resets;
    app.Tts(R"({"type":"tts","state":"start"})");app.Drain();assert(app.audio_service_.resets==resets);
    app.Tts(R"({"type":"tts","state":"stop"})");app.protocol.closed();
    app.state=kDeviceStateIdle;app.Button(); // New generation before queued old stop/close callbacks.
    app.Drain();assert(app.state==kDeviceStateListening);
    app.state=kDeviceStateSpeaking;app.StopInteraction();
    app.state=kDeviceStateIdle;app.Button(); // A superseded queued stop must not close the new channel.
    app.Drain();assert(app.state==kDeviceStateListening&&!app.interaction_stopped_);
    app.state=kDeviceStateSpeaking;app.StopInteraction();
    app.protocol.audio(std::make_unique<AudioStreamPacket>());app.Drain();
    assert(app.audio_service_.packets==0&&app.state==kDeviceStateIdle);
}
''')

    def test_three_boot_buttons_only_double_click_toggle_and_connection_wakes_pet_silently(self):
        for board in ['wzh_crawlPanda','wzh_tailRedPanda','wzh_dinosaur']:
            source=(ROOT/'main/boards'/board/(board+'.cc')).read_text()
            button=queue_function(source,'void InitializeButton(')
            mode=queue_function(source,'void InitializeConversationMode(')
            with self.subTest(board=board):
                self.run_cpp(r'''
#include <cassert>
#include <functional>
#define ESP_LOGI(...) ((void)0)
enum DeviceState{kDeviceStateIdle,kDeviceStateStarting,kDeviceStateWifiConfiguring,kDeviceStateConnecting,
 kDeviceStateListening,kDeviceStateSpeaking,kDeviceStateUpgrading,kDeviceStateFatalError,kDeviceStateAudioTesting};
namespace wzh{enum class PetControl{kWake};}
struct Controller{
 int wakes=0;bool active=false,speaking=false;
 void HandlePetControl(wzh::PetControl,bool respond){assert(!respond);++wakes;}
 void SetMaintenance(bool){}void SetConversationActive(bool x){active=x;}void SetSpeaking(bool x){speaking=x;}
};
struct Application{
 DeviceState state=kDeviceStateIdle;int toggles=0;
 std::function<void(DeviceState,DeviceState)>listener;
 static Application&GetInstance(){static Application a;return a;}
 DeviceState GetDeviceState(){return state;}void ToggleConversation(){++toggles;}
 void AddDeviceStateChangeListener(decltype(listener) f){listener=f;}
};
struct Button{std::function<void()>click,double_click;
 void OnClick(std::function<void()>f){click=f;}void OnDoubleClick(std::function<void()>f){double_click=f;}};
struct Board{
    bool maintenance=false;bool IsAudioMaintenanceActive(){return maintenance;}
    struct Console {bool maintenance_active(){return false;}} web_console_;
    Button boot_button_;Controller controller_;int configs=0;void EnterWifiConfigMode(){++configs;}
__BUTTON__
__MODE__
};
int main(){
 Board b;b.InitializeButton();b.InitializeConversationMode();auto&app=Application::GetInstance();
 b.boot_button_.click();assert(app.toggles==0&&b.configs==0);
 b.boot_button_.double_click();b.boot_button_.double_click();assert(app.toggles==2);
 app.state=kDeviceStateWifiConfiguring;b.boot_button_.click();assert(b.configs==1&&app.toggles==2);
 app.listener(kDeviceStateIdle,kDeviceStateConnecting);assert(b.controller_.wakes==1&&b.controller_.active);
 app.listener(kDeviceStateConnecting,kDeviceStateListening);app.listener(kDeviceStateListening,kDeviceStateSpeaking);
 assert(b.controller_.wakes==1&&b.controller_.speaking);
 app.listener(kDeviceStateSpeaking,kDeviceStateIdle);assert(!b.controller_.active&&!b.controller_.speaking);
}
'''.replace('__BUTTON__',button).replace('__MODE__',mode))

    def test_prompt_producer_stops_after_reset_instead_of_enqueuing_old_remainder(self):
        method=queue_function((ROOT/'main/audio/audio_service.cc').read_text(),'void AudioService::PlaySound(')
        self.run_cpp(r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>
struct AudioStreamPacket{int sample_rate=0,frame_duration=0;std::vector<uint8_t>payload;};
struct OggDemuxer{
 std::function<void(const uint8_t*,int,size_t)>callback;
 void OnDemuxerFinished(decltype(callback)f){callback=f;}void Reset(){}
 void Process(const uint8_t*p,size_t n){for(int i=0;i<5;++i)callback(p,16000,n);}
};
struct AudioService{
 uint32_t generation=7;int accepted=0,rejected=0;bool cancel=false;
 uint32_t GetPlaybackGeneration(){return generation;}
 bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>p,bool wait,uint32_t expected){
  assert(wait&&p->sample_rate==16000&&p->frame_duration==60&&!p->payload.empty());
  if(expected!=generation){++rejected;return false;}
  ++accepted;if(cancel)++generation;return true;
 }
 void PlaySound(const std::string_view&);
};
'''+method+r'''
int main(){
 AudioService ordinary;ordinary.PlaySound("opus");assert(ordinary.accepted==5&&ordinary.rejected==0);
 AudioService cancelled;cancelled.cancel=true;cancelled.PlaySound("opus");
 assert(cancelled.accepted==1&&cancelled.rejected==1);
}
''')

    def test_output_worker_drops_cancelled_pcm_and_old_motion_envelope(self):
        method=queue_function((ROOT/'main/audio/audio_service.cc').read_text(),'void AudioService::AudioOutputTask(')
        self.run_cpp(r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "/home/wzh/program/ESP32/zylz_project/test_xiaozhi_Final/main/audio/audio_playout_buffer.h"
#define ESP_LOGW(...) ((void)0)
constexpr unsigned AUDIO_POWER_CHECK_INTERVAL_MS=1000;
void esp_timer_stop(void*){}void esp_timer_start_periodic(void*,unsigned){}
struct AudioTask{uint32_t playback_generation=7,timestamp=0;std::vector<int16_t>pcm={500,1000};};
struct Codec{
 bool enabled=false;int writes=0;
 std::function<void()>enable,write;
 int output_sample_rate(){return 16000;}
 bool output_enabled(){return enabled;}void EnableOutput(bool x){enabled=x;if(enable)enable();}
 void OutputData(std::vector<int16_t>&){++writes;if(write)write();}
};
struct AudioService{
 Codec codec;Codec*codec_=&codec;void*audio_power_timer_=nullptr;
 std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<std::unique_ptr<AudioTask>>audio_playback_queue_;
 std::atomic<bool>service_stopped_{false};uint32_t playback_generation_=7;
 AudioPlayoutBuffer playout_buffer_;
 uint32_t BufferedPlaybackMsLocked()const{return 0;}
 bool buffered_playback_=true;
 bool output_in_flight_=false;
 float output_fast_env_=0,output_slow_env_=0;
 std::function<void(float,float)>output_audio_level_listener_;
 std::chrono::steady_clock::time_point last_output_time_;
 struct{int playback_count=0;}debug_statistics_;
 struct{std::function<void()>on_playback_drained;}callbacks_;
 uint32_t GetPlaybackGeneration(){return playback_generation_;}
 bool MarkPlaybackDrainedLocked(){return audio_playback_queue_.empty()&&!output_in_flight_;}
 void AudioOutputTask();
};
'''+method+r'''
int main(){
 for(int cancellation=0;cancellation<3;++cancellation){
  AudioService service;int levels=0,drained=0;
  service.audio_playback_queue_.push_back(std::make_unique<AudioTask>());
  service.output_audio_level_listener_=[&](float level,float){assert(level>.03f);++levels;};
  service.callbacks_.on_playback_drained=[&]{++drained;};
  service.codec.enable=[&]{if(cancellation==1){++service.playback_generation_;service.service_stopped_=true;}};
  service.codec.write=[&]{if(cancellation==2)++service.playback_generation_;service.service_stopped_=true;};
  service.AudioOutputTask();
  assert(service.codec.writes==(cancellation==1?0:1));
  assert(levels==(cancellation==0?1:0)&&service.debug_statistics_.playback_count==levels);
  assert(drained==1&&!service.output_in_flight_);
 }
}
''')
