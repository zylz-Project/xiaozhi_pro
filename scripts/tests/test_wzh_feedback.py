"""Regression coverage for deliberate touch, handling, short sounds and cancellation."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / 'main/boards/common/wzh'


class WzhFeedbackTest(unittest.TestCase):
    def test_pad_variants_are_independent_and_triple_taps_rearm(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_pet_behavior.h"
using namespace wzh;
int main() {
    InteractionMotionSelector selector;
    for (int event=1;event<=4;++event) {
        auto pad=static_cast<PetInteraction>(event);
        assert(selector.Select(pad)==event-1);
        assert(selector.Select(PetInteraction::kPickedUp)%int(InteractionMotionSelector::kEventCount)==7);
        for(unsigned variant=1;variant<InteractionMotionSelector::kVariantCount;++variant)
            assert(selector.Select(pad)==int(variant*InteractionMotionSelector::kEventCount)+event-1);
        assert(selector.Select(pad)==event-1);
    }
    assert(selector.Select(PetInteraction::kNone)==-1);
    assert(selector.Select(static_cast<PetInteraction>(255))==-1);
    PetBehavior pet;
    for (uint32_t tap=0;tap<9;++tap) {
        uint32_t now=UINT32_MAX-700+tap*300;
        pet.Touch(1,now);
        auto response=pet.PollTouchEvent(now);
        assert(response.response==(tap%3==2?TouchResponse::kPlayful:TouchResponse::kAffection));
        pet.Touch(0,now+100);assert(pet.PollTouch(now+100)==TouchResponse::kNone);
    }
    // Switching pads starts a new sequence; holding does not retrigger every tick.
    pet.Touch(2,5000);assert(pet.PollTouch(5000)==TouchResponse::kAffection);
    assert(pet.PollTouch(6200)==TouchResponse::kHeld);
    assert(pet.PollTouch(9000)==TouchResponse::kNone);
    pet.Touch(0,9100);assert(pet.PollTouch(9100)==TouchResponse::kReleased);
}
''')

    def run_cpp(self, program):
        program = '#include <cstdio>\n#include <cstring>\n#include "wzh_user_pairs.h"\n' + program
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'test.cc'
            source.write_text(program)
            executable = Path(directory) / 'test'
            result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                     '-Werror', '-I', str(COMMON), str(source), '-o', str(executable)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_handling_while_powered_retry_settling_and_freefall(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_posture_input.h"
using namespace wzh;
int main() {
    PostureInput input;
    uint32_t now = UINT32_MAX-5000;
    for (int i=0; i<80; ++i,now+=20)
        assert(input.Update(i%2*20, 0, 1000, false, false, now)==PetInteraction::kNone);
    for (int i=0; i<80; ++i,now+=20)
        assert(input.Update(i%2*40, 0, 1000, true, false, now)==PetInteraction::kNone);
    now+=620;
    for (int i=0;i<30;++i)
        assert(input.Update(0,0,1000,false,false,now+=20)==PetInteraction::kNone);
    assert(input.Update(0,0,1230,false,false,now+=20)==PetInteraction::kNone);
    assert(input.Update(0,0,1260,false,false,now+=20)==PetInteraction::kNone);
    assert(input.Update(0,0,1240,false,false,now+=20)==PetInteraction::kPickedUp);
    // A rejected controller request remains available rather than consuming the event.
    assert(input.Update(0,0,1000,false,false,now+=20)==PetInteraction::kPickedUp);
    input.Accepted(now);
    for (int i=0;i<100;++i)
        assert(input.Update(0,0,1000,false,false,now+=20)==PetInteraction::kNone);
    // Only a NEW movement followed by quiet may infer placement.
    for(int i=0;i<4;++i)
        assert(input.Update(i%2?220:-220,0,1000,false,false,now+=20)==PetInteraction::kNone);
    PetInteraction settled=PetInteraction::kNone;
    for (int i=0;i<90;++i) {
        auto event=input.Update(0,0,1000,false,false,now+=20);
        if (event==PetInteraction::kPutDown) { settled=event;input.Accepted(now); }
    }
    assert(settled==PetInteraction::kPutDown);
    now+=1000;
    assert(input.Update(1500,0,1000,true,false,now+=20)==PetInteraction::kNone);
    assert(input.Update(-1500,0,1000,true,false,now+=20)==PetInteraction::kNone);
    now+=620;
    for(int i=0;i<30;++i)
        assert(input.Update(0,0,1000,false,false,now+=20)==PetInteraction::kNone);
    input.Update(1500,0,1000,false,false,now+=20);
    assert(input.Update(-1500,0,1000,false,false,now+=20)==PetInteraction::kShaken);
    input.Accepted(now);
    // Actual free-fall protection suppresses handling output independently.
    for(int i=0;i<10;++i)
        assert(input.Update(0,0,0,true,true,now+=20)==PetInteraction::kNone);
    input.Invalidate();
    assert(input.Update(0,0,1000,false,false,now+=20)==PetInteraction::kNone);
    now+=1000;
    for(int i=0;i<30;++i)
        assert(input.Update(0,0,1000,false,false,now+=20)==PetInteraction::kNone);
    PetInteraction tilt=PetInteraction::kNone;
    for(int i=0;i<10;++i) {
        auto event=input.Update(700,0,700,false,false,now+=20);
        if(event==PetInteraction::kFlipped) { tilt=event;input.Accepted(now); }
    }
    assert(tilt==PetInteraction::kFlipped);
}
''')

    def test_production_touch_routes_four_pads_and_handling_without_losing_fast_taps(self):
        harness = r'''
#include <atomic>
#include <array>
#include <cassert>
#include <mutex>
#include <vector>
#include "wzh_pet_behavior.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
namespace wzh { struct Sensors { enum class Event { kHuman,kPickedUp,kShaken,kFlipped,kPutDown }; }; }
enum class Action { kListen,kHappy,kDiscover };
using PandaAction=Action;using DinoAction=Action;
enum class Body { kShakeHead,kWaveLeft,kWaveRight,kRaiseArms,kLookLeft,kLookRight,kWagTail };
using PandaBodyAction=Body;using DinoBodyAction=Body;
struct Request {
    Action action;bool with_local_sound=false,is_body=false;int test_gpio=-1;
    uint32_t created_ms=0,generation=0;
    wzh::PetPriority priority=wzh::PetPriority::kExplicit;
    bool speech_bound=false;
    wzh::PetInteraction interaction=wzh::PetInteraction::kNone;
};
int64_t clock_us=30000000;
int64_t esp_timer_get_time(){return clock_us;}
constexpr int pdTRUE=1;
std::vector<Request> requests;
int xQueueOverwrite(void*,const Request* request){requests.push_back(*request);return pdTRUE;}
struct Output { bool locked=false;bool blocked(){return locked;}bool powered(){return false;}unsigned faults(){return locked?1:0;} };
struct Audio { struct Data{bool playing=false;};Data GetTtsMotionData(){return {};} };
struct Controller {
    using ActionRequest=Request;
    std::mutex request_mutex_;void* action_queue_=this;
    wzh::PetBehavior pet_;Output servo_output_;
    Audio* audio_=nullptr;
    std::atomic<bool> shutdown_requested_{false},request_active_{false},speaking_{false};
    std::atomic<int> pending_expression_{-1};
    std::atomic<uint32_t> expression_since_{0},intent_generation_{0};
    std::atomic<int64_t> last_request_us_{0};
    std::atomic<wzh::PetPriority> running_priority_{wzh::PetPriority::kSensor};
    Action last_action_=Action::kListen;
    int64_t last_action_time_us_=0;uint32_t last_action_generation_=0;
    wzh::PetMode pet_mode(){return pet_.Get(clock_us/1000).mode;}
    bool TriggerAction(Action,bool,wzh::PetPriority,bool=false){return true;}
    bool QueueRequest(ActionRequest,wzh::PetPriority);
    bool QueueInteraction(wzh::PetInteraction);
    bool HandleSensorEvent(int);
    void HandleTouch(uint8_t);
    void PollInteraction(uint32_t);
};
'''
        exercise = r'''
int main(){
    Controller c;
    for (uint8_t mask:{1,2,4,8}) {
        c.HandleTouch(mask);c.HandleTouch(0);clock_us+=200000;
    }
    assert(requests.size()==4);
    for(int i=0;i<4;++i){
        assert(requests[i].is_body);
        assert(requests[i].interaction==static_cast<wzh::PetInteraction>(i+1));
        assert(requests[i].priority==wzh::PetPriority::kExplicit);
        for(int j=0;j<i;++j)assert(requests[i].action!=requests[j].action);
    }
    c.HandleTouch(2);c.HandleTouch(0);clock_us+=100000;c.HandleTouch(2);c.HandleTouch(0);
    assert(requests.size()==6); // Repeated deliberate touches are not coalesced for 1.2 seconds.
    clock_us+=2000000;
    assert(c.HandleSensorEvent(int(wzh::Sensors::Event::kShaken)));
    assert(requests.back().interaction==wzh::PetInteraction::kShaken);
    assert(c.pet_.Get(clock_us/1000).gentle);
    assert(c.pet_.CanPlayInteractionSound(clock_us/1000));
    c.pet_.InteractionSoundStarted(clock_us/1000);
    assert(!c.pet_.CanPlayInteractionSound(clock_us/1000+649));
    assert(c.pet_.CanPlayInteractionSound(clock_us/1000+650));
    c.pet_.Control(wzh::PetControl::kQuiet,clock_us/1000);
    assert(!c.pet_.CanPlayInteractionSound(clock_us/1000+2000));
    c.pet_.Control(wzh::PetControl::kRest,clock_us/1000);
    const auto count=requests.size();c.HandleTouch(8);c.HandleTouch(0);
    assert(requests.size()==count);
    c.pet_.Control(wzh::PetControl::kWake,clock_us/1000);
    c.servo_output_.locked=true;c.HandleTouch(1);
    assert(requests.size()==count); // Feedback never bypasses motion protection.
    Controller busy;busy.request_active_.store(true);
    assert(!busy.HandleSensorEvent(int(wzh::Sensors::Event::kPickedUp)));
    assert(!busy.HandleSensorEvent(int(wzh::Sensors::Event::kPutDown)));
    assert(!busy.pet_.Get(clock_us/1000).gentle);
    assert(!busy.HandleSensorEvent(int(wzh::Sensors::Event::kShaken)));
    Controller blocked;blocked.servo_output_.locked=true;
    assert(!blocked.HandleSensorEvent(int(wzh::Sensors::Event::kShaken)));
    assert(!blocked.pet_.Get(clock_us/1000).gentle); // A rejected event cannot shrink the next motion.
}
'''
        for board, stem, cls in [('wzh_crawlPanda','panda','PandaController'),
                                  ('wzh_tailRedPanda','panda','PandaController'),
                                  ('wzh_dinosaur','dinosaur','DinosaurController')]:
            source=(ROOT/'main/boards'/board/(stem+'_controller.cc')).read_text()
            methods=''
            for name, ret in [('QueueRequest','bool'),('QueueInteraction','bool'),
                              ('HandleSensorEvent','bool'),('HandleTouch','void'),('PollInteraction','void')]:
                methods+=queue_function(source,ret+' '+cls+'::'+name+'(').replace(cls+'::','Controller::')+'\n'
            with self.subTest(board=board): self.run_cpp(harness+methods+exercise)

    def test_external_sound_selection_varies_and_uses_real_directory_only(self):
        self.run_cpp(r'''
#include <cassert>
#include <vector>
#include "wzh_interaction_sound.h"
struct File {
    char name[64]; unsigned size=100,duration_ms=1000; char category[16]="animal";
};
using namespace wzh;
int main() {
    InteractionSoundSelector selector;
    std::vector<File> files={{"熊猫撒娇声音"},{"熊猫宝宝嘤嘤"},{"熊猫好奇叫声"},
                             {"熊猫受惊声音"},{"吃竹咀嚼"},{"入睡打鼾"},
                             {"开心宝宝环境音",100,300,"ambient"}};
    assert(selector.Select(files,PetInteraction::kTouch1,0)==0);
    assert(selector.Select(files,PetInteraction::kTouch2,0)==1);
    assert(selector.Select(files,PetInteraction::kTouch3,0)==2);
    assert(selector.Select(files,PetInteraction::kShaken,0)==3);
    for(int i=0,last=-1;i<100;++i) {
        int chosen=selector.Select(files,PetInteraction::kTouch1,i);
        assert(chosen>=0&&chosen<4&&chosen!=last);
        selector.Remember(files[chosen].name);last=chosen;
    }
    // Directory reorder/delete does not confuse the remembered filename.
    selector.Remember(files[0].name);
    files={files[1],files[0]};
    assert(selector.Select(files,PetInteraction::kHeld,0)==0);
    files={{"仅有一条原始叫声"}};
    selector.Remember(files[0].name);
    assert(selector.Select(files,PetInteraction::kHeld,0)==0);
    files={{"开心环境音",100,1000,"ambient"},{"损坏条目",0,100,"animal"}};
    assert(selector.Select(files,PetInteraction::kPlayful,0)==-1);
    files.clear();
    assert(selector.Select(files,PetInteraction::kTouch1,0)==-1);
    files={{"BABY",100,1000,""},{"NUZZLE",100,1000,""}};
    assert(selector.Select(files,PetInteraction::kTouch2,0)==0);
    assert(selector.Select(files,PetInteraction::kHeld,0)==1);
    assert(selector.Select(files,PetInteraction::kNone,0)==-1);
    // Shorter recordings win when their content score is equal; files are not truncated.
    files={{"call long",100,7000,"animal"},{"call short",100,500,"animal"}};
    assert(selector.Select(files,PetInteraction::kTouch3,0)==1);
}
''')

    def test_external_sound_queue_preserves_file_and_rejects_busy_and_chat(self):
        source=(COMMON/'wzh_audio_player.cc').read_text()
        methods='\n'.join(queue_function(source,marker) for marker in [
            'bool AudioPlayer::PlayIndex(', 'bool AudioPlayer::QueueFile(',
            'std::vector<AudioFileInfo> AudioPlayer::PairedFiles(',
            'bool AudioPlayer::PlayInteractionSound('])
        program=r'''
#include <atomic>
#include <array>
#include <cassert>
#include <mutex>
#include <vector>
#include "wzh_interaction_sound.h"
using namespace wzh;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
constexpr int pdTRUE=1;
enum {kDeviceStateIdle,kDeviceStateConnecting,kDeviceStateListening,kDeviceStateSpeaking};
struct AudioFileInfo { char name[64];unsigned size=100,duration_ms=1000,offset=0,sample_rate=48000;char category[16]="animal"; };
struct AudioStore {
    bool writing=false;
    std::vector<AudioFileInfo> files={{"撒娇",100,1000,100},{"宝宝开心",100,1000,200}};
    bool IsWriting(){return writing;}
    auto Snapshot(){return files;}
    bool GetFile(int index,AudioFileInfo* out){
        if(index<0||index>=int(files.size()))return false;
        *out=files[index];return true;
    }
};
struct Service {bool idle=true;bool IsPlaybackIdle(){return idle;}};
struct Application {
    int state=kDeviceStateIdle;Service service;
    static Application& GetInstance(){static Application instance;return instance;}
    int GetDeviceState(){return state;}Service& GetAudioService(){return service;}
};
struct PlayRequest {int index;unsigned generation;AudioFileInfo file;};
std::vector<PlayRequest> queue;
bool queue_full=false;
int xQueueSend(void*,const PlayRequest* request,int wait){
    assert(wait==0);if(queue_full)return 0;
    queue.push_back(*request);return pdTRUE;
}
unsigned esp_random(){return 0;}
struct AudioPlayer {
    std::atomic<bool>maintenance_{false},worker_active_{false};
    std::array<char,64>current_name_{};
    AudioStore store;AudioStore* store_=&store;void* play_queue_=this;
    std::atomic<unsigned> playback_generation_{7};
    InteractionSoundSelector interaction_sounds_;std::mutex reaction_mutex_,playback_mutex_;
    bool playback_pending_=false;
    bool PlayIndex(int);bool QueueFile(int,const AudioFileInfo&);
    std::vector<AudioFileInfo> PairedFiles()const;
    bool PlayInteractionSound(PetInteraction);
};
'''
        exercise=r'''
int main(){
    AudioPlayer p;auto& app=Application::GetInstance();
    assert(p.PlayInteractionSound(PetInteraction::kTouch1));
    assert(queue.back().file.offset==100&&queue.back().generation==7);
    assert(!p.PlayInteractionSound(PetInteraction::kTouch1)); // Reserved before decoder starts.
    assert(queue.size()==1);p.playback_pending_=false;
    assert(p.PlayInteractionSound(PetInteraction::kTouch1));
    assert(queue.back().file.offset==200);p.playback_pending_=false;
    p.store.writing=true;assert(!p.PlayInteractionSound(PetInteraction::kTouch1));
    p.store.writing=false;
    for(auto state:{kDeviceStateConnecting,kDeviceStateListening,kDeviceStateSpeaking}){
        app.state=state;assert(!p.PlayInteractionSound(PetInteraction::kTouch1));
    }
    app.state=kDeviceStateIdle;
    app.service.idle=false;assert(!p.PlayInteractionSound(PetInteraction::kTouch1));
    app.service.idle=true;
    queue_full=true;assert(!p.PlayInteractionSound(PetInteraction::kTouch1));queue_full=false;
    // Rejections above must not change the next variety choice.
    assert(p.PlayInteractionSound(PetInteraction::kTouch1));
    assert(queue.back().file.offset==100);
    p.store.files.erase(p.store.files.begin());
    assert(queue.back().file.offset==100); // copied entry, not a later directory index lookup
    p.playback_pending_=false;
    assert(p.PlayIndex(0));assert(queue.back().file.offset==200);
    assert(!p.PlayIndex(-1));assert(!p.PlayIndex(10));
    p.store.files.clear();auto count=queue.size();
    assert(!p.PlayInteractionSound(PetInteraction::kTouch1));assert(queue.size()==count);
}
'''
        self.run_cpp(program+methods+exercise)

    def test_external_playback_reads_selected_flash_file_and_honors_stop(self):
        source=(COMMON/'wzh_audio_player.cc').read_text()
        method=queue_function(source,'bool AudioPlayer::PlayFile(')
        program=r'''
#include <algorithm>
#include <atomic>
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "wzh_sound_score.h"
using namespace wzh;
int64_t fake_time=1000000;
int64_t esp_timer_get_time(){return fake_time;}
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned ms){fake_time+=ms*1000;}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
constexpr int ESP_OK=0;
constexpr size_t kReadChunk=4096;
enum {kDeviceStateIdle,kDeviceStateConnecting,kDeviceStateListening,kDeviceStateSpeaking};
struct AudioFileInfo {const char* name="original Flash recording";unsigned offset=777,size=5000,duration_ms=6000; const char* category="animal";};
struct AudioStore {bool writing=false;bool IsWriting(){return writing;}unsigned data_start(){return 131072;}};
struct AudioStreamPacket {int sample_rate=0,frame_duration=0;std::vector<uint8_t> payload;};
std::atomic<unsigned>* generation_ptr=nullptr;
int stop_after_packets=0;bool stop_during_admission=false;
struct Service {
    bool idle=true;int packets=0;
    bool IsPlaybackIdle(){if(stop_during_admission){stop_during_admission=false;++*generation_ptr;}return idle;}
    uint32_t GetPlaybackGeneration(){return 7;}
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet,bool,uint32_t generation){
        assert(generation==7);
        assert(packet->sample_rate==24000&&packet->frame_duration==60&&!packet->payload.empty());
        ++packets;
        if(stop_after_packets==packets)++*generation_ptr;
        return true;
    }
};
struct Application {
    int state=kDeviceStateIdle;Service service;
    static Application& GetInstance(){static Application instance;return instance;}
    int GetDeviceState(){return state;}Service& GetAudioService(){return service;}
};
struct Codec {bool enabled=false;bool output_enabled(){return enabled;}void EnableOutput(bool value){enabled=value;if(value&&stop_during_admission)++*generation_ptr;}};
struct Board {
    Codec codec;
    static Board& GetInstance(){static Board instance;return instance;}
    Codec* GetAudioCodec(){return &codec;}
};
struct OggDemuxer {
    std::function<void(const uint8_t*,int,size_t)> output;
    void OnDemuxerFinished(decltype(output) value){output=value;}
    void Process(const uint8_t* bytes,size_t size){output(bytes,24000,std::min<size_t>(size,16));}
};
std::vector<std::pair<unsigned,size_t>> reads;
int wzh_flash_read(unsigned address,uint8_t* bytes,size_t size){
    assert(size<=4096);reads.push_back({address,size});std::fill(bytes,bytes+size,42);bytes[0]=24;return ESP_OK;
}
struct AudioPlayer {
    std::atomic<bool>maintenance_{false},worker_active_{false};
    std::array<char,64>current_name_{};
    struct PlayRequest {int index;unsigned generation;AudioFileInfo file;};
    AudioStore store;AudioStore* store_=&store;std::mutex playback_mutex_;
    std::atomic<unsigned> playback_generation_{1};
    std::atomic<int> motion_sound_index_{-1};
    std::atomic<const SoundScore*> motion_score_{nullptr};
    std::atomic<SoundKind> motion_kind_{SoundKind::kCall};
    std::atomic<unsigned> motion_sequence_{0};
    std::atomic<int64_t> local_pcm_start_us_{0};
    std::atomic<int> local_packet_ms_{60};
    std::atomic<unsigned> motion_elapsed_ms_{0},motion_duration_ms_{0};
    std::atomic<float> motion_level_{0},motion_attack_{0};
    std::atomic<bool> is_playing_{false};
    float fast_envelope_=0,slow_envelope_=0;
    void UpdateEnvelope(size_t){}
    bool PlayFile(const PlayRequest&);
};
'''
        exercise=r'''
int main(){
    AudioPlayer p;generation_ptr=&p.playback_generation_;
    AudioPlayer::PlayRequest request{3,1,{}};
    assert(p.PlayFile(request));
    assert(reads.size()==2&&reads[0].first==131072+777&&reads[0].second==4096);
    assert(reads[1].first==131072+777+4096&&reads[1].second==904);
    assert(p.motion_sound_index_==3&&p.motion_duration_ms_==6000&&!p.is_playing_);
    reads.clear();auto& app=Application::GetInstance();
    for(auto state:{kDeviceStateConnecting,kDeviceStateListening,kDeviceStateSpeaking}){
        app.state=state;assert(!p.PlayFile(request));assert(reads.empty());
    }
    app.state=kDeviceStateIdle;
    p.store.writing=true;assert(!p.PlayFile(request));assert(reads.empty());p.store.writing=false;
    app.service.packets=0;stop_after_packets=1;
    assert(!p.PlayFile(request));assert(reads.size()==1);
    reads.clear();assert(!p.PlayFile(request));assert(reads.empty());
    AudioPlayer race;generation_ptr=&race.playback_generation_;
    Board::GetInstance().codec.enabled=false;stop_during_admission=true;
    assert(!race.PlayFile(request));assert(reads.empty()&&!race.is_playing_);
    // Stop between admission and metadata publication cannot resurrect a stale worker.
}
'''
        self.run_cpp(program+method+exercise)

    def test_stopped_or_replaced_audio_requests_do_not_resume_in_worker(self):
        source=(COMMON/'wzh_audio_player.cc').read_text()
        task=queue_function(source,'void AudioPlayer::Task()')
        program=r'''
#include <atomic>
#include <array>
#include <cassert>
#include <cstdint>
#include <mutex>
struct Done{};
constexpr int pdTRUE=1,portMAX_DELAY=0;
struct PlayRequest { int index;uint32_t generation; };
int reads=0,played=0;bool replace_during_play=false;
int xQueueReceive(void*,PlayRequest* request,int){
    if(reads==0)*request={0,0};
    else if(reads==1)*request={1,1};
    else throw Done{};
    ++reads;return pdTRUE;
}
struct AudioPlayer{
    std::atomic<bool>maintenance_{false},worker_active_{false};
    std::array<char,64>current_name_{};
    std::atomic<uint32_t> playback_generation_{1};
    std::atomic<bool> is_playing_{false};
    std::mutex playback_mutex_;bool playback_pending_=true;
    void* play_queue_=this;
    void Task();
    void PlayFile(const PlayRequest& request){
        assert(request.index==1&&request.generation==1);++played;
        if(replace_during_play)++playback_generation_; // Stop, then a new reserved request.
    }
};
'''
        self.run_cpp(program+task+r'''
int main(){
    AudioPlayer p;try{p.Task();}catch(Done&){}assert(played==1&&!p.playback_pending_);
    reads=played=0;replace_during_play=true;
    AudioPlayer next;try{next.Task();}catch(Done&){}
    assert(played==1&&next.playback_pending_&&next.playback_generation_==2);
}''')

    def test_production_accelerometer_probe_and_read_recovery(self):
        source=(COMMON/'wzh_sensors.cc').read_text()
        program=r'''
#include <cassert>
#include <cstring>
#include <functional>
#include <mutex>
#include "wzh_pet_behavior.h"
#include "wzh_posture_input.h"
uint32_t clock_ms=0;
int creates=0,removes=0,read_errors=0,gestures=0,fall_while_powered=0;
struct Done{};
template<class... T>void log_stub(const T&...){}
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
#define ESP_LOGD(...) log_stub(__VA_ARGS__)
constexpr int ESP_OK=0,I2C_NUM_1=1,I2C_CLK_SRC_DEFAULT=1,I2C_ADDR_BIT_LEN_7=7;
using gpio_num_t=int;
using i2c_master_bus_handle_t=void*;using i2c_master_dev_handle_t=void*;
struct i2c_master_bus_config_t {
    int i2c_port,sda_io_num,scl_io_num,clk_source,glitch_ignore_cnt;
    struct {int enable_internal_pullup;}flags;
};
struct i2c_device_config_t {int dev_addr_length,device_address,scl_speed_hz;};
uint8_t registers_[256]={};
int i2c_new_master_bus(i2c_master_bus_config_t* config,void** bus){
    assert(config->i2c_port==1&&config->sda_io_num==43&&config->scl_io_num==44);
    *bus=registers_;++creates;return ESP_OK;
}
int i2c_master_probe(void*,int address,int){return clock_ms>=5000&&address==0x18?ESP_OK:-1;}
int i2c_master_bus_add_device(void*,i2c_device_config_t* config,void** device){
    assert(config->scl_speed_hz==100000);*device=registers_;return ESP_OK;
}
int i2c_master_bus_rm_device(void*){++removes;return ESP_OK;}
int i2c_del_master_bus(void*){return ESP_OK;}
int i2c_master_transmit(void*,const uint8_t* bytes,size_t,int){
    registers_[bytes[0]]=bytes[1];return ESP_OK;
}
int i2c_master_transmit_receive(void*,uint8_t* reg,size_t,uint8_t* bytes,size_t size,int){
    if((*reg&0x7f)==0x28){
        if(clock_ms>=6100&&clock_ms<6180){++read_errors;return -1;}
        std::memset(bytes,0,size);
        int x=((clock_ms>=7000&&clock_ms<8000)||(clock_ms>=9000&&clock_ms<9100))
                  ?(clock_ms/20%2?1500:-1500):0;
        int z=1000;
        if(clock_ms>=7600&&clock_ms<7680)x=z=0;
        const int values[]={x,0,z};
        for(int axis=0;axis<3;++axis){
            const auto raw=static_cast<uint16_t>(static_cast<int16_t>(values[axis]*8));
            bytes[axis*2]=raw&255;bytes[axis*2+1]=raw>>8;
        }
    }else if(*reg==0x0f)*bytes=0x11;
    else if(*reg==0x70)*bytes=0x28;
    else *bytes=registers_[*reg];
    return ESP_OK;
}
int gpio_get_level(int){return 0;}
int64_t esp_timer_get_time(){return int64_t(clock_ms)*1000;}
void vTaskDelay(uint32_t ticks){clock_ms+=ticks;if(clock_ms>10000)throw Done{};}
#define pdMS_TO_TICKS(ms) (ms)
namespace wzh {
constexpr const char* kTag="test";
int Orientation(int,int,int){return 4;}
constexpr int kFalling=4;
struct Output{
    bool blocked(){return false;}bool powered(){return clock_ms>=7000&&clock_ms<8000;}
    unsigned faults(){return 0;}
    void SetFault(int,bool enabled){if(enabled&&powered())++fall_while_powered;}
};
struct Sensors{
    enum class Event{kHuman,kPickedUp,kShaken,kFlipped,kPutDown};
    struct Status{
        bool human=false,accelerometer=false,angles_valid=false;
        int x_mg=0,y_mg=0,z_mg=0,orientation=-1;
        float pitch=0,roll=0;
    }status_;
    std::mutex mutex_;
    int sda_=43,scl_=44,human_=41;
    void* bus_=nullptr;void* device_=nullptr;
    Output output;Output* output_=&output;
    std::function<bool(Event)>hook_;
    bool InitializeAccelerometer(gpio_num_t,gpio_num_t);
    bool Read(uint8_t,uint8_t*,size_t);bool Write(uint8_t,uint8_t);
    void Task();
};
'''
        for marker in ['bool Sensors::Read(', 'bool Sensors::Write(',
                       'bool Sensors::InitializeAccelerometer(', 'void Sensors::Task()']:
            program+=queue_function(source,marker)+'\n'
        program+=r'''
} // namespace wzh
int main(){
    wzh::Sensors sensor;
    sensor.hook_=[](wzh::Sensors::Event event){
        assert(clock_ms>=9000&&event==wzh::Sensors::Event::kShaken);++gestures;return true;
    };
    try{sensor.Task();}catch(Done&){}
    assert(creates>=3&&removes>=1&&read_errors>=3);
    assert(sensor.status_.accelerometer&&sensor.status_.angles_valid);
    assert(sensor.status_.z_mg==1000&&sensor.device_!=nullptr);
    assert(gestures==1&&fall_while_powered>=1);
}
'''
        self.run_cpp(program)


if __name__=='__main__': unittest.main()
