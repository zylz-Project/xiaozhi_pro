"""Product lifecycle and failed-update regressions using production C++ methods."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT/'main/boards/common/wzh'
BOARDS = [('wzh_crawlPanda','panda','PandaController'),
          ('wzh_tailRedPanda','panda','PandaController'),
          ('wzh_dinosaur','dinosaur','DinosaurController')]
JSON_DIR = ROOT/'managed_components/espressif__cjson/cJSON'


class WzhProductReadinessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler=shutil.which('g++')
        cc=shutil.which('gcc')
        if not cls.compiler or not cc or not (JSON_DIR/'cJSON.c').exists():
            raise unittest.SkipTest('host compilers and the installed project cJSON component required')
        cls.temp=tempfile.TemporaryDirectory()
        cls.json_object=Path(cls.temp.name)/'cjson.o'
        subprocess.run([cc,'-c',str(JSON_DIR/'cJSON.c'),'-o',str(cls.json_object)],check=True,capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_cpp(self, program):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'product.cc';source.write_text("#include <mutex>\n"+program)
            executable=source.with_suffix('')
            r=subprocess.run([self.compiler,'-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                              '-I',str(COMMON),'-I',str(JSON_DIR),str(source),str(self.json_object),
                              '-o',str(executable)],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr)
            r=subprocess.run([str(executable)],capture_output=True,text=True,timeout=10)
            self.assertEqual(r.returncode,0,r.stderr)

    def test_contact_does_not_survive_modes_or_maintenance_and_emotions_are_paced(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_pet_behavior.h"
#include "wzh_speech_intent.h"
using namespace wzh;
int main(){
    for(auto control:{PetControl::kStop,PetControl::kRest,PetControl::kQuiet}){
        PetBehavior pet;pet.Touch(1,100);pet.PollTouch(100);
        assert(pet.PollTouch(1300)==TouchResponse::kNone);
        pet.Control(control,1400);pet.Control(PetControl::kWake,5000);
        pet.Touch(1,8000);pet.Touch(3,8050); // A mode change does not create a press.
        assert(pet.PollTouch(8050)==TouchResponse::kNone&&!pet.Get(8050).gentle);
        pet.Touch(0,8100);assert(pet.PollTouch(8100)==TouchResponse::kNone);
        pet.Touch(1,8200);assert(pet.PollTouch(8200)==TouchResponse::kAffection);
    }
    PetBehavior pet;pet.Touch(1,100);pet.PollTouch(100);
    pet.SetMaintenance(true);pet.Touch(1,1000);pet.SetMaintenance(false);pet.Touch(1,9999);
    assert(pet.PollTouch(10000)==TouchResponse::kNone);
    pet.Touch(0,10020);pet.Touch(2,10040);
    assert(pet.PollTouch(10040)==TouchResponse::kAffection);
    assert(pet.Offer(PetPriority::kEmotion,UINT32_MAX-1000));
    assert(!pet.Offer(PetPriority::kEmotion,500));
    assert(pet.Offer(PetPriority::kEmotion,800));
    assert(pet.Offer(PetPriority::kExplicit,900));
}
''')

    def test_action_feedback_distinguishes_dispatch_and_physical_completion(self):
        self.run_cpp(r'''
#include <cassert>
#include <cstring>
#include "wzh_pet_feedback.h"
using namespace wzh;
int main(){
    for(auto fault:{kLowBattery,kOverheat,kFalling,kShutdown,kBatteryPending,kPetPaused,kMaintenance,kMotionTimeout}){
        auto* result=PetStatus(PetMode::kNormal,fault,false,false,false,0);
        assert(cJSON_IsFalse(cJSON_GetObjectItem(result,"accepted")));
        assert(std::strcmp(cJSON_GetObjectItem(result,"reason")->valuestring,MotionFaultName(fault))==0);
        assert(cJSON_IsFalse(cJSON_GetObjectItem(result,"physical_completion_verified")));
        cJSON_Delete(result);
    }
    auto* result=PetStatus(PetMode::kQuiet,0,true,true,true,1);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"accepted")));
    assert(!std::strcmp(cJSON_GetObjectItem(result,"mode")->valuestring,"quiet"));
    assert(!std::strcmp(cJSON_GetObjectItem(result,"result")->valuestring,"request_accepted"));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result,"physical_completion_verified")));
    cJSON_Delete(result);
}
''')

    def test_all_three_primary_wakes_resume_without_a_flash_show(self):
        harness=r'''
#include <atomic>
#include <cassert>
#include "wzh_pet_behavior.h"
#include "wzh_speech_intent.h"
#include "wzh_protection.h"
#include "wzh_sound_choreography.h"
#define ESP_LOGI(...) ((void)0)
int64_t esp_timer_get_time(){return 10000000;}
unsigned esp_random(){return 0;}
struct Application{int stops=0;static Application& GetInstance(){static Application a;return a;}void StopInteraction(){++stops;}};
enum class Action{kDiscover};using PandaAction=Action;using DinoAction=Action;
struct Output{uint32_t faults=wzh::kLowBattery;void SetFault(wzh::MotionFault f,bool on){if(on)faults|=f;else faults&=~f;}};
struct Controller{
    wzh::PetBehavior pet_;std::atomic<uint32_t>intent_generation_{0};std::atomic<int>pending_expression_{-1};
    std::atomic<bool>stop_audio_requested_{false},conversation_active_{false};Output servo_output_;int shows=0;
    wzh::AutoplayCadence autoplay_;
    bool TriggerAction(Action,bool,wzh::PetPriority){++shows;return true;}
    bool HandlePetControl(wzh::PetControl,bool respond=true);
};
__METHOD__
int main(){
    Controller c;c.pet_.Control(wzh::PetControl::kRest,100);
    assert(c.HandlePetControl(wzh::PetControl::kWake,false));
    assert(c.pet_.Get(10000).mode==wzh::PetMode::kNormal);
    assert(c.shows==0&&!c.stop_audio_requested_&&(c.servo_output_.faults&wzh::kLowBattery));
    assert(c.HandlePetControl(wzh::PetControl::kWake));assert(c.shows==1&&c.stop_audio_requested_);
    assert(c.HandlePetControl(wzh::PetControl::kStop));
    assert(c.servo_output_.faults&wzh::kPetPaused);assert(Application::GetInstance().stops==1);
}
'''
        for board,stem,cls in BOARDS:
            source=(ROOT/'main/boards'/board/(stem+'_controller.cc')).read_text()
            method=queue_function(source,'bool '+cls+'::HandlePetControl(').replace(cls+'::','Controller::')
            with self.subTest(board=board):self.run_cpp(harness.replace('__METHOD__',method))

    def test_local_product_name_routes_to_conversation_and_rejects_echo(self):
        harness = r'''
#include <cassert>
#include <cstring>
#include <string>
#include "wzh_pet_behavior.h"
#include "wzh_onboarding.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
namespace wzh {
struct Info { const char* wake_phrase="__WAKE__"; };
Info Product(){return {};}
void NotifyOnboarding(OnboardingEvent){}
bool CaptureOnboardingInteraction(OnboardingEvent){return false;}
bool onboarding_block=false;
bool OnboardingBlocksConversation(){return onboarding_block;}
int interactions=0;
void NotifyIdleInteraction(){++interactions;}
}
enum class Action{kHappy,kAffection,kEat};using PandaAction=Action;using DinoAction=Action;
struct Audio{bool echo=false;int stops=0;bool LocalCommandEchoRisk(){return echo;}void StopPlayback(){++stops;}};
struct Controller{
 int wake=0;bool HandlePetControl(wzh::PetControl c,bool=false){if(c==wzh::PetControl::kWake)++wake;return false;}
 bool IsConversationActive(){return false;}bool HandleBodyCommand(const std::string&){return false;}
 void TriggerAction(Action,bool){}
};
struct Application{std::string word;static Application& GetInstance(){static Application a;return a;}
 void WakeWordInvoke(const std::string& s){word=s;}};
struct TestBoard{Audio local_audio_;Controller controller_;
 bool HandleLocalVoiceCommand(const std::string& command);};
__METHOD__
int main(){
 TestBoard b;assert(b.HandleLocalVoiceCommand("local:chat_wake"));
 assert(b.controller_.wake==1&&b.local_audio_.stops==1);
 assert(Application::GetInstance().word=="__WAKE__");
 b.local_audio_.echo=true;Application::GetInstance().word.clear();
 assert(b.HandleLocalVoiceCommand("local:chat_wake"));
 assert(Application::GetInstance().word.empty()&&b.controller_.wake==1);
 // Offline tutorial chapter: the name is recognized but no channel is opened,
 // because an unsaved network would fail silently.
 b.local_audio_.echo=false;Application::GetInstance().word.clear();
 wzh::onboarding_block=true;assert(b.HandleLocalVoiceCommand("local:chat_wake"));
 assert(Application::GetInstance().word.empty()&&b.controller_.wake==1&&b.local_audio_.stops==1);
 wzh::onboarding_block=false;
}
'''
        for board, wake in [('wzh_crawlPanda', '小华'),
                            ('wzh_tailRedPanda', '小武'),
                            ('wzh_dinosaur', '小威')]:
            board_dir = ROOT / 'main/boards' / board
            source = (board_dir / (board + '.cc')).read_text()
            method = queue_function(source, 'bool HandleLocalVoiceCommand(')
            method = method.replace('bool HandleLocalVoiceCommand(',
                                    'bool TestBoard::HandleLocalVoiceCommand(', 1)
            method = method.replace(') override {', ') {', 1)
            with self.subTest(board=board):
                self.assertIn(f'.wake_phrase = "{wake}",', (board_dir / 'wzh_product.cc').read_text())
                self.run_cpp(harness.replace('__METHOD__', method).replace('__WAKE__', wake))

    def test_all_three_shutdowns_park_only_active_healthy_servos_with_a_deadline(self):
        harness=r'''
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cmath>
#include "wzh_protection.h"
#include "wzh_motion_profile.h"
#include "wzh_quiet_center.h"
uint32_t now=0;int writes=0;bool stuck=false,fail_midway=false;
int64_t esp_timer_get_time(){return int64_t(now)*1000;}
void vTaskDelay(uint32_t ms){now+=ms;}
#define pdMS_TO_TICKS(ms) (ms)
constexpr int SERVO_HEAD_DEFAULT=90,SERVO_LEFT_ARM_DEFAULT=90,SERVO_RIGHT_ARM_DEFAULT=90;
constexpr int SERVO_TAIL_LR_DEFAULT=90,SERVO_TAIL_UD_DEFAULT=150;
constexpr int SERVO_NECK_TILT_DEFAULT=70,SERVO_NECK_LEAN_DEFAULT=90,SERVO_HEAD_TURN_DEFAULT=90;
struct Pose{__POSE__};
namespace wzh_panda{using Pose=::Pose;}
struct Output{bool on=true,fault=false;bool powered(){return on;}bool blocked(){return fault||(fail_midway&&now>=80);}
    void SetFault(wzh::MotionFault,bool enabled){fault=enabled;on=false;}};
struct Controller{
    Output servo_output_;Pose last_output_pose_;wzh::QuietCenter<5> quiet_center_;std::atomic<bool>request_active_{true},speaking_{false},speech_tail_{true};
    bool clip_active_=true;
    std::atomic<bool>flash_performance_active_{true},sky_roar_active_{true};
    void OutputPose(const Pose& target){
        ++writes;assert(!servo_output_.blocked());if(stuck)return;
        __FIELDS__
    }
    void SetPose(const Pose& target){OutputPose(target);}
    void ParkForShutdown();
};
__METHOD__
int main(){
    Controller c;c.ParkForShutdown();assert(now<=2200&&writes>1&&!c.servo_output_.on&&c.servo_output_.fault);
    __ARRIVED__
    now=0;writes=0;Controller idle;idle.servo_output_.on=false;idle.ParkForShutdown();assert(writes==0&&now==0);
    Controller faulty;faulty.servo_output_.fault=true;faulty.ParkForShutdown();assert(writes==0&&now==0);
    Controller stalled;stuck=true;stalled.ParkForShutdown();assert(now>=900&&now<=2200&&stalled.servo_output_.fault);
    now=0;writes=0;stuck=false;fail_midway=true;Controller interrupted;interrupted.ParkForShutdown();
    assert(now==80&&writes==4&&interrupted.servo_output_.fault);
}
'''
        defaults={'wzh_crawlPanda':{'head':90,'left_arm':90,'right_arm':90},
                  'wzh_tailRedPanda':{'head':90,'tail_lr':90,'tail_ud':150},
                  'wzh_dinosaur':{'neck_tilt':70,'neck_lean':90,'head_turn':90,'tail_ud':150,'tail_lr':90}}
        for board,stem,cls in BOARDS:
            source=(ROOT/'main/boards'/board/(stem+'_controller.cc')).read_text()
            method=queue_function(source,'void '+cls+'::ParkForShutdown(').replace(cls+'::','Controller::')
            fields=defaults[board]
            # Match the actual product defaults in this combined test harness.
            program=harness.replace('__METHOD__',method)
            initial={'head':170,'left_arm':8,'right_arm':172,'tail_lr':174,'tail_ud':10,'neck_tilt':180,'neck_lean':0,'head_turn':180}
            program=program.replace('__POSE__','float '+','.join(f+'='+str(initial[f]) for f in fields)+';')
            if stem=='dinosaur':program=program.replace('SERVO_TAIL_UD_DEFAULT=150','SERVO_TAIL_UD_DEFAULT=90');fields=fields|{'tail_ud':90}
            step='\n'.join('last_output_pose_.'+f+'+=std::clamp(target.'+f+'-last_output_pose_.'+f+',-3.2f,3.2f);' for f in fields)
            arrived='\n'.join('assert(std::abs(c.last_output_pose_.'+f+'-'+str(v)+')<=1.5f);' for f,v in fields.items())
            with self.subTest(board=board):self.run_cpp(program.replace('__FIELDS__',step).replace('__ARRIVED__',arrived))

    def test_sound_replacement_preserves_old_file_on_errors_and_never_locks_network_reads(self):
        source=(COMMON/'wzh_audio_store.cc').read_text()
        methods='\n'.join(queue_function(source,m) for m in ['bool LooksLikeOggOpus(', 'uint32_t AlignUp('])
        write=queue_function(source,'bool AudioStore::WriteFile(')
        self.run_cpp(r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
template<class... T>void log_stub(const T&...){}
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
const char* TAG="test";constexpr int ESP_OK=0;constexpr size_t kReadChunk=4096;
std::array<uint8_t,65536> flash;
int wzh_flash_unit_ok(uint32_t,bool* ok){*ok=true;return ESP_OK;}
int wzh_flash_erase_unit(uint32_t address){assert(address>=512);std::fill_n(flash.data()+address,256,0xFF);return ESP_OK;}
struct AudioFileInfo{char name[64]={};uint32_t offset=0,size=0,sample_rate=0,duration_ms=0;char category[16]={};};
struct WritingGuard{std::atomic<bool>&flag;~WritingGuard(){flag=false;}};
struct AudioStore{
    static constexpr int kMaxFiles=32;std::atomic<bool>writing_{false};std::mutex mutex_;
    std::vector<AudioFileInfo>files_;uint32_t erase_size_=256,program_size_=16,data_start_=256;
    uint64_t capacity_=65536;bool commit_fail=false;
    uint64_t UsedBytesLocked(){uint64_t n=0;for(auto& f:files_)n=std::max(n,uint64_t(f.offset)+f.size);return n;}
    bool ProgramWithVerify(uint32_t address,const uint8_t* bytes,size_t count){assert(address>=512);std::memcpy(flash.data()+address,bytes,count);return true;}
    bool CommitToc(const std::vector<AudioFileInfo>& entries){if(commit_fail)return false;files_=entries;return true;}
    std::vector<AudioFileInfo> Snapshot(){std::lock_guard<std::mutex>lock(mutex_);return files_;}
    bool WriteFile(const std::string&,uint32_t,uint32_t,uint32_t,const std::string&,
                   const std::function<size_t(uint8_t*,size_t)>&,std::string*);
};
'''+methods+'\n'+write+r'''
int main(){
    AudioStore store;AudioFileInfo old;std::strcpy(old.name,"call");old.size=64;store.files_.push_back(old);
    std::fill(flash.begin(),flash.end(),0x5A);
    std::array<uint8_t,64> audio{};std::memcpy(audio.data(),"OggS",4);std::memcpy(audio.data()+8,"OpusHead",8);
    size_t read=0;bool fail=false,oversized=false;
    auto reader=[&](uint8_t* out,size_t size)->size_t{
        auto snapshot=store.Snapshot();assert(snapshot[0].offset==0); // Must not wait on the network's catalogue lock.
        if(oversized)return size+1;
        if(fail&&read>0)return SIZE_MAX;
        size_t n=std::min(size,audio.size()-read);if(fail)n=std::min(n,size_t(24));
        std::memcpy(out,audio.data()+read,n);read+=n;return n;
    };
    for(int failure=0;failure<3;++failure){
        read=0;fail=failure==0;store.commit_fail=failure==1;oversized=failure==2;
        assert(!store.WriteFile("call",64,48000,1000,"animal",reader,nullptr));
        assert(!store.writing_&&store.files_.size()==1&&store.files_[0].offset==0);
        for(int i=256;i<320;++i)assert(flash[i]==0x5A);
    }
    // A full directory still permits replacing an existing item, without a delete first.
    store.files_.resize(32);read=0;fail=false;oversized=false;store.commit_fail=false;
    assert(store.WriteFile("call",64,48000,1000,"animal",reader,nullptr));
    assert(store.files_.size()==32&&store.files_[0].offset==256);
    for(int i=256;i<320;++i)assert(flash[i]==0x5A);
    assert(std::memcmp(flash.data()+512,audio.data(),64)==0);
    assert(!store.WriteFile("new",64,48000,1000,"animal",reader,nullptr));
}
''')

    def test_real_manifest_parser_rejects_bad_catalogues_without_changing_outputs(self):
        source=(COMMON/'wzh_audio_hub.cc').read_text()
        method=queue_function(source,'bool AudioHub::ParseManifest(')
        self.run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <cJSON.h>
#define ESP_LOGE(...) ((void)0)
constexpr size_t kMaxManifestResponse=65536;
struct ProductInfo{const char* hub_product_id="dinosaur";};
ProductInfo Product(){return {};}
struct AudioFileInfo{char name[64];};
struct Store{uint64_t capacity(){return 128ULL*1024*1024;}};
struct AudioHub{
    mutable std::mutex metadata_mutex_;
    struct RemoteFile{std::string name,base_name;uint32_t size=0;std::string category;};
    Store store;Store* store_=&store;
    bool ParseManifest(const std::string&,std::vector<RemoteFile>*,std::string*);
};
'''+method+r'''
int main(){
    AudioHub hub;std::vector<AudioHub::RemoteFile> files;std::string revision="old";
    auto manifest=[](std::string items, const char* product="dinosaur"){
        return "{\"product\":\""+std::string(product)+"\",\"revision\":\"v1\",\"files\":["+items+"]}";
    };
    auto file=[](std::string name,std::string size){return "{\"name\":\""+name+"\",\"size\":"+size+",\"category\":\"animal\"}";};
    auto valid=file("call.opus","64");
    assert(hub.ParseManifest(manifest(valid),&files,&revision));assert(files.size()==1&&files[0].size==64);
    revision="unchanged";
    assert(!hub.ParseManifest(manifest(valid,"wzh-crawlpanda"),&files,&revision));
    assert(!hub.ParseManifest("{\"revision\":\"v1\",\"files\":["+valid+"]}",&files,&revision));
    assert(revision=="unchanged"&&files.size()==1&&files[0].size==64);
    for(auto bad:{"0","-1","1.5","4294967296","33554433","null","1e300"}){
        assert(!hub.ParseManifest(manifest(file("call.opus",bad)),&files,&revision));
        assert(revision=="unchanged"&&files.size()==1&&files[0].size==64);
    }
    assert(!hub.ParseManifest(manifest(""),&files,&revision));
    assert(!hub.ParseManifest(manifest(valid+","+valid),&files,&revision));
    for(auto name:{"../call.opus","dir/call.opus","dir\\\\call.opus","bad\\ncall.opus",".opus"})
        assert(!hub.ParseManifest(manifest(file(name,"64")),&files,&revision));
    assert(!hub.ParseManifest(manifest(file(std::string(64,'a')+".opus","64")),&files,&revision));
    std::string list;
    for(int i=0;i<33;++i){if(i)list+=",";list+=file(std::to_string(i)+".opus","1");}
    assert(!hub.ParseManifest(manifest(list),&files,&revision));
    list.clear();for(int i=0;i<5;++i){if(i)list+=",";list+=file(std::to_string(i)+".opus","33554432");}
    assert(!hub.ParseManifest(manifest(list),&files,&revision)); // Larger than physical capacity.
    assert(!hub.ParseManifest(std::string(65537,' '),&files,&revision));
}
''')

    def test_http_response_bounds_and_incomplete_body_are_enforced(self):
        source=(COMMON/'wzh_audio_hub.cc').read_text()
        method=queue_function(source,'bool ReadResponse(')
        self.run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
using esp_http_client_handle_t=int;
std::string response="hello";size_t offset=0;bool complete=true,failed=false;int64_t declared=5;
int64_t esp_http_client_get_content_length(int){return declared;}
bool esp_http_client_is_complete_data_received(int){return complete;}
int esp_http_client_read(int,char* buffer,size_t size){
    if(failed)return -1;
    size=std::min(size,response.size()-offset);
    std::memcpy(buffer,response.data()+offset,size);offset+=size;return size;
}
'''+method+r'''
int main(){
    std::string body;assert(ReadResponse(1,8,&body)&&body=="hello");
    offset=0;declared=9;assert(!ReadResponse(1,8,&body)&&body.empty()&&offset==0);
    declared=-1;response=std::string(9,'a');assert(!ReadResponse(1,8,&body)&&body.empty());
    offset=0;response="hello";complete=false;assert(!ReadResponse(1,8,&body)&&body.empty());
    complete=true;failed=true;assert(!ReadResponse(1,8,&body)&&body.empty());
}
''')

    def test_factory_update_writes_only_selected_audio_and_can_retry(self):
        source = (COMMON/'wzh_audio_hub.cc').read_text()
        methods = '\n'.join(queue_function(source, signature) for signature in (
            'bool FactoryNetworkAvailable(', 'void AudioHub::DoUpdate(',
        ))
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <string>
#include <vector>

bool FactoryNetworkAvailable();
struct Board {
    bool wifi = true;
    bool cellular = false;
    static Board& GetInstance() { static Board board; return board; }
    bool HasSystemNetwork() const { return wifi; }
    bool UsesCellularNetwork() const { return cellular; }
};
std::string stored_revision = "v1";
struct Settings {
    Settings(const char*, bool) {}
    void SetString(const char*, const std::string& value) { stored_revision = value; }
};
struct AudioHub {
    enum class Status { kWaitingNetwork, kSyncing, kActive, kError };
    struct RemoteFile { std::string base_name; };
    std::mutex job_mutex_, metadata_mutex_;
    std::vector<std::string> update_names_{"call"};
    std::vector<RemoteFile> remote_files_{{"call"}, {"other"}};
    std::vector<int> diff_{1};
    std::string checked_revision_ = "v2", revision_ = "v1", message;
    std::atomic<Status> status_{Status::kActive};
    std::atomic<bool> job_ok_{false};
    bool busy = false, interrupted = true;
    int downloads = 0, local_call_size = 32, local_only_size = 16;
    void SetJobMessage(const std::string& value) { message = value; }
    bool CanDownload() const { return !busy && FactoryNetworkAvailable(); }
    bool DownloadFile(int index, const RemoteFile& file) {
        assert(index == 0 && file.base_name == "call");
        ++downloads;
        if (interrupted) return false;
        local_call_size = 64;
        return true;
    }
    void DoUpdate();
};

''' + methods + r'''
int main() {
    AudioHub hub;
    auto& board = Board::GetInstance();
    board.wifi = false;
    hub.DoUpdate();
    assert(hub.status_ == AudioHub::Status::kWaitingNetwork && hub.downloads == 0);
    board.wifi = true;
    board.cellular = true;
    hub.DoUpdate();
    assert(hub.downloads == 0);
    board.cellular = false;
    hub.busy = true;
    hub.DoUpdate();
    assert(hub.downloads == 0);
    hub.busy = false;
    hub.DoUpdate();
    assert(hub.downloads == 1 && !hub.job_ok_ && hub.local_call_size == 32);
    assert(hub.revision_ == "v1" && stored_revision == "v1");
    hub.interrupted = false;
    hub.DoUpdate();
    assert(hub.downloads == 2 && hub.job_ok_ && hub.local_call_size == 64);
    assert(hub.local_only_size == 16 && hub.revision_ == "v2" && stored_revision == "v2");
    assert(hub.diff_.empty());
}
''')

    def test_factory_hub_serializes_manual_work_and_automatic_presence(self):
        source = (COMMON/'wzh_audio_hub.cc').read_text()
        methods = '\n'.join(queue_function(source, signature) for signature in (
            'void AudioHub::ConfigureForFactory(', 'void AudioHub::TaskEntry(',
            'bool AudioHub::RequestCheck(', 'bool AudioHub::RequestUpdate(',
            'bool AudioHub::RequestJob(', 'bool AudioHub::BeginLocalMaintenance(',
        ))
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <string>
#include <utility>
#include <vector>

constexpr int pdPASS = 1;
constexpr int64_t kPresenceRetryUs = 10000000;
int64_t clock_us = 1;
int64_t esp_timer_get_time() { return clock_us; }
using TaskEntryPoint = void (*)(void*);
TaskEntryPoint queued = nullptr;
void* queued_arg = nullptr;
int created = 0, released = 0;
bool allocation_fails = false;
int xTaskCreate(TaskEntryPoint fn, const char*, int, void* arg, int, void*) {
    if (allocation_fails) return 0;
    assert(!queued);
    queued = fn;
    queued_arg = arg;
    ++created;
    return pdPASS;
}
void vTaskDelete(void* current) { assert(current == nullptr); ++released; }
void run_pending_task() {
    assert(queued);
    std::exchange(queued, nullptr)(queued_arg);
}

struct AudioStore {};
struct AudioHub {
    enum class Job { kCheck, kUpdate, kPresence };
    enum class Status { kIdle, kError };
    AudioStore* store_ = nullptr;
    std::function<bool()> interaction_busy_;
    std::mutex job_mutex_;
    Job pending_job_ = Job::kCheck;
    std::string offered_server_;
    int64_t next_presence_us_ = 0;
    std::vector<std::string> update_names_;
    std::string job_message_;
    std::atomic<Status> status_{Status::kIdle};
    std::atomic<bool> job_ok_{false}, job_running_{false};
    bool local_maintenance_ = false;
    int checks = 0, updates = 0, announcements = 0;
    void ConfigureForFactory(AudioStore*, std::function<bool()>);
    static void TaskEntry(void*);
    bool RequestCheck();
    bool RequestUpdate(const std::vector<std::string>&);
    bool RequestJob(Job, const std::vector<std::string>&, const std::string& = {});
    bool BeginLocalMaintenance();
    void DoCheck() { assert(job_running_); ++checks; }
    void DoUpdate() { assert(job_running_ && update_names_ == std::vector<std::string>{"call"}); ++updates; }
    void DoPresence(const std::string& server) {
        assert(job_running_ && server == "http://192.168.1.7:5000");
        ++announcements;
    }
};

''' + methods + r'''
int main() {
    AudioStore store;
    AudioHub hub;
    hub.ConfigureForFactory(&store, [] { return false; });
    assert(created == 0 && !hub.job_running_ && hub.checks == 0);
    assert(!hub.RequestUpdate({}));
    assert(hub.RequestCheck());
    assert(created == 1 && hub.job_running_);
    assert(!hub.RequestCheck() && !hub.RequestUpdate({"call"}) && !hub.BeginLocalMaintenance());
    run_pending_task();
    assert(hub.checks == 1 && hub.updates == 0 && !hub.job_running_ && released == 1);
    allocation_fails = true;
    assert(!hub.RequestUpdate({"call"}) && !hub.job_running_ && !queued);
    allocation_fails = false;
    assert(hub.RequestUpdate({"call"}));
    run_pending_task();
    assert(hub.checks == 1 && hub.updates == 1 && !hub.job_running_ && released == created);
    const std::string server = "http://192.168.1.7:5000";
    assert(hub.RequestJob(AudioHub::Job::kPresence, {}, server));
    assert(!hub.RequestCheck());
    run_pending_task();
    assert(hub.announcements == 1 && hub.updates == 1);
    assert(!hub.RequestJob(AudioHub::Job::kPresence, {}, server));
    clock_us += kPresenceRetryUs;
    assert(hub.RequestJob(AudioHub::Job::kPresence, {}, server));
    run_pending_task();
    assert(hub.announcements == 2);
    assert(hub.BeginLocalMaintenance());
    assert(!hub.RequestCheck() && !hub.RequestUpdate({"call"}));
    assert(!hub.RequestJob(AudioHub::Job::kPresence, {}, server));
}
''')
        for board, _, _ in BOARDS:
            board_source = (ROOT/'main/boards'/board/f'{board}.cc').read_text()
            self.assertIn('audio_hub_.ConfigureForFactory(', board_source)
            self.assertNotIn('RegisterHubMcpTools', board_source)
        console = (COMMON/'wzh_web_console.cc').read_text()
        for handler, method in [('HandleHubCheck', 'RequestCheck'), ('HandleHubUpdate', 'RequestUpdate')]:
            body = queue_function(console, f'esp_err_t WebConsole::{handler}(')
            self.assertLess(body.index('RequireDeveloper(req)'), body.index('->'+method+'('))

    def test_registration_accepts_created_response_and_validates_direct_token(self):
        source=(COMMON/'wzh_audio_hub.cc').read_text()
        method=queue_function(source,'bool AudioHub::Register(')
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <cJSON.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
constexpr int kHttpTimeoutMs=10000,ESP_OK=0,HTTP_METHOD_POST=1;
constexpr size_t kMaxControlResponse=8192;
struct esp_http_client_config_t{const char*url;int method,timeout_ms,buffer_size;};
using esp_http_client_handle_t=void*;
int response_status=201;std::string response;
void ConfigureHttpTrust(esp_http_client_config_t*){}
esp_http_client_handle_t esp_http_client_init(esp_http_client_config_t*){return reinterpret_cast<void*>(1);}
void esp_http_client_set_header(esp_http_client_handle_t,const char*,const char*){}
int esp_http_client_open(esp_http_client_handle_t,size_t){return ESP_OK;}
void esp_http_client_write(esp_http_client_handle_t,const char*,size_t){}
int esp_http_client_fetch_headers(esp_http_client_handle_t){return 0;}
int esp_http_client_get_status_code(esp_http_client_handle_t){return response_status;}
void esp_http_client_close(esp_http_client_handle_t){}void esp_http_client_cleanup(esp_http_client_handle_t){}
bool ReadResponse(esp_http_client_handle_t,size_t,std::string*body){*body=response;return true;}
struct Settings{
    static std::map<std::string,std::string>values;
    Settings(const char*,bool){}
    void SetString(const char*key,const std::string&value){values[key]=value;}
    void EraseKey(const char*key){values.erase(key);}
};
std::map<std::string,std::string>Settings::values;
struct ProductInfo{const char*hub_product_id="panda";};
ProductInfo Product(){return {};}
std::string FirmwareVersion(){return "1.0";}
struct AudioHub{
    mutable std::mutex metadata_mutex_;
    enum class Status{kRegistering};std::atomic<Status>status_{Status::kRegistering};
    std::string device_id_="id",api_token_,claim_token_,activation_code_;std::atomic<bool>activated_{false};
    std::string ServerUrl(){return "https://example.invalid";}
    bool Register();
};
'''+method+r'''
int main(){
    AudioHub hub;response="{\"claim_token\":\"claim\",\"activation_code\":\"1234\"}";
    assert(hub.Register()&&hub.claim_token_=="claim"&&!hub.activated_);
    response="{\"api_token\":\"token\"}";
    assert(hub.Register()&&hub.activated_&&hub.api_token_=="token"&&hub.claim_token_.empty());
    assert(Settings::values["api_token"]=="token"&&Settings::values.count("claim_token")==0);
    response_status=200;assert(hub.Register());
    response="{\"api_token\":\"\"}";assert(!hub.Register());
    response="{\"api_token\":\""+std::string(513,'a')+"\"}";assert(!hub.Register());
}
''')

    def test_all_three_power_key_adc_fault_requires_a_new_verified_release(self):
        harness=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
namespace wzh {bool OnboardingBlocksConversation(){return false;}void PrepareOnboardingDoubleClick(){}void NotifyIdleInteraction(){}}
template<class... T>void log_stub(const T&...){}
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
constexpr const char* TAG="key";
constexpr int ESP_OK=0,POWER_BUTTON_ADC_CHANNEL=5,POWER_DEBOUNCE_MS=30,POWER_LONG_PRESS_MS=1500;
constexpr int WZH_DOUBLE_CLICK_MS=650;
constexpr int POWER_BUTTON_PRESS_MV=1000,POWER_BUTTON_RELEASE_MV=1200,POWER_MAX_SAMPLE_GAP_MS=250;
using esp_err_t=int;
const char* esp_err_to_name(int){return "error";}
constexpr int kDeviceStateIdle=0;
namespace Lang{namespace Sounds{constexpr int OGG_SUCCESS=1,OGG_POPUP=2;}}
using TickType_t=uint32_t;
uint32_t clock_ms=0;int voltage=3300;bool valid=true;
uint32_t esp_log_timestamp(){return clock_ms;}uint32_t xTaskGetTickCount(){return clock_ms;}
#define pdMS_TO_TICKS(ms) (ms)
int adc_oneshot_read(int,int,int*raw){*raw=voltage;return valid?ESP_OK:-1;}
int AdcToMv(int,int raw){return raw;}
struct Service{void PlaySound(int){}};
struct Application{
    Service service;int toggles=0;
    static Application& GetInstance(){static Application a;return a;}
    Service& GetAudioService(){return service;}int GetDeviceState(){return kDeviceStateIdle;}
    void ToggleConversation(){++toggles;}
};
struct Power{
    enum class BtnState{kIdle,kDebouncePress,kPressed,kDebounceRelease};
    BtnState state=BtnState::kIdle;int adc_=0,power_adc_cali_=0;
    uint32_t state_since_ms=0,press_start_ms=0;int hold_tip=0;
    uint32_t last_sample_ms=0;
    bool armed=false,boot_hold_logged=false,power_adc_fault=false,previous_gray_sample=false;
    TickType_t last_click_tick=0;int click_count=0,shutdowns=0;
    int feedbacks=0;
    std::atomic<bool> hold_feedback_ready_{true};
    std::function<void()> hold_feedback_hook_=[this](){++feedbacks;};
    void Shutdown(){++shutdowns;}
    void Step(){__KEY__}
};
int main(){
    auto sample=[&](Power& target,int mv,bool success,int count){
        voltage=mv;valid=success;for(int i=0;i<count;++i){clock_ms+=20;target.Step();}
    };
    Power p;
    sample(p,3300,true,10);sample(p,100,true,20); // A real press starts, then ADC becomes invalid.
    sample(p,0,false,100);sample(p,100,true,100); // Restored held-high input is not another press.
    assert(p.shutdowns==0&&Application::GetInstance().toggles==0);
    sample(p,3300,true,10);sample(p,100,true,5);sample(p,3300,true,5);
    sample(p,100,true,5);sample(p,3300,true,5);
    assert(Application::GetInstance().toggles==1&&p.shutdowns==0);
    sample(p,1100,true,20); // Ambiguous mid-band voltage is unknown, never a press.
    assert(p.shutdowns==0&&Application::GetInstance().toggles==1);
    // One click, an unknown sample, then another click must not pair.
    sample(p,3300,true,5);sample(p,100,true,5);sample(p,3300,true,5);
    sample(p,1100,true,5);
    sample(p,100,true,5);sample(p,3300,true,5);
    assert(Application::GetInstance().toggles==1&&p.shutdowns==0);
    sample(p,100,true,85);assert(p.shutdowns>=1); // Valid long press still works.
    // A held key gives feedback once and releasing it is not a double click.
    Power held;
    sample(held,3300,true,10);sample(held,100,true,58);
    assert(held.feedbacks==1&&held.shutdowns==0);
    sample(held,3300,true,8);
    assert(Application::GetInstance().toggles==1);
    // A sampler stall must not join the two sides of one long press.
    Power q;
    sample(q,3300,true,10);sample(q,100,true,30); // 600 ms on the pressed band
    clock_ms+=400;voltage=100;q.Step();           // Gap > POWER_MAX_SAMPLE_GAP_MS
    assert(q.shutdowns==0);
    sample(q,3300,true,10);                       // Stable release re-arms shutdown
    sample(q,100,true,85);assert(q.shutdowns>=1);
    // Two short taps with a pause longer than the old 400 ms, and one gray
    // ADC sample on each transition, still make just one double-click.
    Power spaced;
    sample(spaced,3300,true,5);sample(spaced,100,true,3);sample(spaced,1100,true,1);
    sample(spaced,3300,true,24);sample(spaced,1100,true,1);sample(spaced,100,true,4);
    sample(spaced,1100,true,1);sample(spaced,3300,true,4);
    assert(Application::GetInstance().toggles==2&&spaced.shutdowns==0);
    Power separate;
    sample(separate,3300,true,5);sample(separate,100,true,3);sample(separate,3300,true,38);
    sample(separate,100,true,3);sample(separate,3300,true,4);
    assert(Application::GetInstance().toggles==2&&separate.shutdowns==0);
}
'''
        for board,stem,_ in BOARDS:
            source=(ROOT/'main/boards'/board/(stem+'_power.cc')).read_text()
            start=source.index('        int raw = 4095;',source.index('::Task()'))
            end=source.index('        // IO5 上',start)
            with self.subTest(board=board):self.run_cpp(harness.replace('__KEY__',source[start:end]))

    def test_conversation_close_cancels_pending_motion_on_all_three_products(self):
        harness=r'''
#include <atomic>
#include <cassert>
#include "wzh_pet_behavior.h"
#include "wzh_speech_intent.h"
#define ESP_LOGI(...) ((void)0)
enum class Action{kListen};using PandaAction=Action;using DinoAction=Action;
struct Audio{int stops=0;void StopPlayback(){++stops;}};
struct Controller{
    std::atomic<bool>conversation_active_{false},speaking_{false},speech_tail_{false};
    std::atomic<wzh::SpeechIntent>speech_intent_{wzh::SpeechIntent::kNeutral};
    std::atomic<int>pending_expression_{-1};std::atomic<uint32_t>intent_generation_{0};
    wzh::PetBehavior pet_;
    Audio audio;Audio* audio_=&audio;int listens=0;
    void TriggerAction(Action,bool,wzh::PetPriority){++listens;}
    void SetConversationActive(bool);
};
__METHOD__
int main(){
    Controller c;c.pet_.Touch(1,200);c.SetConversationActive(true);c.SetConversationActive(true);
    c.pet_.Touch(3,300);
    assert(c.pet_.PollTouch(300)==wzh::TouchResponse::kNone);
    assert(c.listens==0&&c.audio.stops==1);
    c.pending_expression_=2;c.speaking_=true;c.SetConversationActive(false);
    assert(!c.speaking_&&c.pending_expression_==-1&&c.intent_generation_==1);
    c.SetConversationActive(false);assert(c.intent_generation_==1&&c.listens==0);
    c.SetConversationActive(true);assert(c.listens==0&&c.audio.stops==2);
}
'''
        for board,stem,cls in BOARDS:
            source=(ROOT/'main/boards'/board/(stem+'_controller.cc')).read_text()
            method=queue_function(source,'void '+cls+'::SetConversationActive(').replace(cls+'::','Controller::')
            with self.subTest(board=board):self.run_cpp(harness.replace('__METHOD__',method))

    def test_factory_audio_write_waits_for_thirty_seconds_of_quiet_on_all_three_products(self):
        harness=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
int64_t now=0;int64_t esp_timer_get_time(){return now;}
struct Output{bool on=false;bool powered()const{return on;}};
struct Audio{bool active=false;bool Playing(){return active;}};
struct Controller{
    Audio audio; Audio* audio_=&audio;
    std::atomic<int64_t>last_request_us_{0};std::atomic<bool>request_active_{false},maintenance_{false};Output servo_output_;
    bool IsMaintenanceBusy()const;
};
__METHOD__
int main(){
    Controller c;assert(c.IsMaintenanceBusy());now=30000000;assert(!c.IsMaintenanceBusy());
    c.last_request_us_=now;now+=29999999;assert(c.IsMaintenanceBusy());
    ++now;assert(!c.IsMaintenanceBusy());c.request_active_=true;assert(c.IsMaintenanceBusy());
    c.request_active_=false;c.servo_output_.on=true;assert(c.IsMaintenanceBusy());
    c.servo_output_.on=false;c.audio.active=true;assert(c.IsMaintenanceBusy());
    c.audio.active=false;assert(!c.IsMaintenanceBusy());
}
'''
        for board,stem,cls in BOARDS:
            source=(ROOT/'main/boards'/board/(stem+'_controller.cc')).read_text()
            method=queue_function(source,'bool '+cls+'::IsMaintenanceBusy(').replace(cls+'::','Controller::')
            with self.subTest(board=board):self.run_cpp(harness.replace('__METHOD__',method))

    def test_all_twelve_action_callbacks_keep_legacy_bool_and_offer_opt_in_details(self):
        harness=r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include "wzh_pet_feedback.h"
#define ESP_LOGI(...) ((void)0)
using ReturnValue=std::variant<bool,int,std::string,cJSON*>;
int64_t esp_timer_get_time(){return 1000000;}
struct Property{std::variant<bool,std::string>v;template<class T>T value()const{return std::get<T>(v);}};
struct PropertyList{std::map<std::string,Property>p;
    const Property& operator[](const std::string&name)const{return p.at(name);}};
enum class Action{kHappy};using PandaAction=Action;using DinoAction=Action;
enum class Body{kLookLeft};using PandaBodyAction=Body;using DinoBodyAction=Body;
struct Controller{
    bool permitted=true;std::atomic<bool>conversation_active_{true};wzh::PetBehavior pet_;
    bool ParseAction(const std::string&,Action& a){a=Action::kHappy;return true;}
    bool ParseBodyAction(const std::string&,Body& a){a=Body::kLookLeft;return true;}
    Action EmotionToAction(const std::string&){return Action::kHappy;}
    bool TriggerAction(Action,bool,wzh::PetPriority=wzh::PetPriority::kExplicit){return permitted;}
    bool TriggerBodyAction(Body){return permitted;}
    bool HandlePetControl(wzh::PetControl){return permitted;}
    cJSON* RequestResult(bool accepted){return wzh::PetStatus(wzh::PetMode::kNormal,
                accepted?0U:static_cast<uint32_t>(wzh::kLowBattery),false,false,true,accepted);}
    auto Callback(){return __CALLBACK__;}
};
int main(){
    Controller c;auto callback=c.Callback();PropertyList props;
    props.p={{"action",{std::string("happy")}}, {"command",{std::string("wake")}},
             {"emotion",{std::string("happy")}}, {"details",{false}}};
    auto result=callback(props);assert(std::holds_alternative<bool>(result)&&std::get<bool>(result));
    c.permitted=false;result=callback(props);
    assert(std::holds_alternative<bool>(result)&&!std::get<bool>(result));
    props.p["details"]={true};result=callback(props);assert(std::holds_alternative<cJSON*>(result));
    auto* json=std::get<cJSON*>(result);assert(cJSON_IsFalse(cJSON_GetObjectItem(json,"accepted")));
    assert(std::string(cJSON_GetObjectItem(json,"reason")->valuestring)=="low_battery");
    cJSON_Delete(json);
}
'''
        for board,stem,cls in BOARDS:
            source=(ROOT/'main/boards'/board/(stem+'_controller.cc')).read_text()
            prefix='dinosaur' if stem=='dinosaur' else 'panda'
            for name in ['pet_control','react_to_user_emotion','action','body']:
                start=source.index('"self.'+prefix+'.'+name+'"')
                callback=queue_function(source[start:],'[this](const PropertyList&')
                with self.subTest(board=board,tool=name):
                    self.run_cpp(harness.replace('__CALLBACK__',callback))
