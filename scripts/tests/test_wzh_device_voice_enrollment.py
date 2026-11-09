"""Run the production device-enrollment scheduling and status paths."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_application_interaction import function

ROOT = Path(__file__).resolve().parents[2]


class DeviceVoiceEnrollmentTest(unittest.TestCase):
    def test_start_capture_completion_failure_and_cancel(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required')
        source = (ROOT / 'main/application.cc').read_text()
        start = function(source, 'bool Application::RequestDeviceVoiceEnrollment(')
        status = function(source, 'const char* Application::DeviceVoiceEnrollmentStatus(')
        code = r'''
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "main/audio/voice_enrollment.h"
enum DeviceState { kDeviceStateIdle, kDeviceStateSpeaking, kDeviceStateListening, kDeviceStateConnecting };
constexpr int kAbortReasonNone=0;
struct Protocol { bool opened=true; int stops=0, closes=0; bool IsAudioChannelOpened(){return opened;}
 void SendStopListening(){++stops;} void CloseAudioChannel(){++closes;opened=false;} };
namespace Lang { namespace Sounds { constexpr std::string_view OGG_POPUP="popup"; } }
struct Enrollment : VoiceEnrollment {
 bool start_ok=true; std::string name; std::function<void(bool)> capture, done;
 bool Start(std::function<void(bool)>,std::function<void(bool)>) override { return false; }
 bool StartNamed(const std::string& n,std::function<void(bool)> c,std::function<void(bool)> d) override {
   name=n; capture=c; done=d; return start_ok; }
 bool Capture(const int16_t*,size_t,bool) override { return true; }
 void Cancel() override {}
 const char* Status() const override { return "recording"; }
};
struct Board {
 Enrollment enrollment; bool recording=false; bool maintenance=false;
 static Board& GetInstance(){static Board b;return b;}
 VoiceEnrollment* GetVoiceEnrollment(){return &enrollment;}
 bool IsAudioMaintenanceActive(){return maintenance;}
 void SetVoiceEnrollmentRecording(bool v){recording=v;}
};
struct Audio {
 bool enabled=false; void ResetDecoder(){} void EnableWakeWordDetection(bool){}
 void EnableVoiceProcessing(bool v){enabled=v;} void PlaySound(std::string_view){}
};
struct Application {
 std::atomic<bool> voice_enrollment_reconnecting_{false},barge_in_reconnecting_{false};
 std::atomic<bool> external_voice_preview_active_{false},voice_enrollment_active_{false},interaction_stopped_{false};
 std::atomic<long> voice_enrollment_resume_guard_until_us_{0}; Protocol protocol;Protocol* protocol_=&protocol;
 int aborted=0,cleared=0,resumed=0;
 std::atomic<int> device_voice_enrollment_status_{0};std::atomic<unsigned> interaction_generation_{0},device_voice_enrollment_generation_{0};
 DeviceState state=kDeviceStateIdle; Audio audio_service_;std::vector<std::function<void()>> tasks;
 DeviceState GetDeviceState() const{return state;} void SetDeviceState(DeviceState v){state=v;}
 void AbortSpeaking(int){++aborted;} void ClearEarlyAudio(){++cleared;}
 void ResumeAfterVoiceEnrollment(unsigned){++resumed;voice_enrollment_active_.store(false);state=kDeviceStateListening;}
 void Schedule(std::function<void()> f){tasks.push_back(std::move(f));}
 void Drain(){auto batch=std::move(tasks);tasks.clear();for(auto& f:batch)f();}
 bool RequestDeviceVoiceEnrollment(const std::string&);
 const char* DeviceVoiceEnrollmentStatus() const;
};
''' + start + '\n' + status + r'''
int main() {
 Application app; auto& board=Board::GetInstance();
 assert(!app.RequestDeviceVoiceEnrollment(""));
 app.state=kDeviceStateConnecting;assert(!app.RequestDeviceVoiceEnrollment("name"));app.state=kDeviceStateIdle;
 assert(app.RequestDeviceVoiceEnrollment("妈妈"));assert(!std::strcmp(app.DeviceVoiceEnrollmentStatus(),"starting"));
 assert(!app.RequestDeviceVoiceEnrollment("other"));app.Drain();assert(board.enrollment.name=="妈妈");
 assert(!std::strcmp(app.DeviceVoiceEnrollmentStatus(),"recording"));
 board.enrollment.capture(true);app.Drain();assert(board.recording&&app.audio_service_.enabled);
 board.enrollment.capture(false);app.Drain();assert(!board.recording&&!app.audio_service_.enabled);
 board.enrollment.done(true);app.Drain();assert(app.state==kDeviceStateIdle);
 assert(!std::strcmp(app.DeviceVoiceEnrollmentStatus(),"success"));
 board.enrollment.start_ok=false;assert(app.RequestDeviceVoiceEnrollment("test"));app.Drain();
 assert(!std::strcmp(app.DeviceVoiceEnrollmentStatus(),"failed"));assert(app.state==kDeviceStateIdle);
 board.enrollment.start_ok=true;
 for(auto mode:{kDeviceStateListening,kDeviceStateSpeaking}){
  app.state=mode;app.protocol.opened=true;int oldstops=app.protocol.stops,oldresumed=app.resumed;
  assert(app.RequestDeviceVoiceEnrollment("conversation"));app.Drain();
  assert(app.protocol.stops==oldstops+1&&!app.protocol.opened&&app.aborted&&app.cleared);
  board.enrollment.done(true);app.Drain();assert(app.resumed==oldresumed+1&&app.state==kDeviceStateListening);
 }
 app.state=kDeviceStateListening;board.enrollment.start_ok=false;app.protocol.opened=true;
 int before=app.resumed;assert(app.RequestDeviceVoiceEnrollment("fail"));app.Drain();
 assert(app.resumed==before+1&&!std::strcmp(app.DeviceVoiceEnrollmentStatus(),"failed"));
 board.enrollment.start_ok=true;app.state=kDeviceStateListening;app.protocol.opened=true;
 assert(app.RequestDeviceVoiceEnrollment("cancel"));app.Drain();
 app.voice_enrollment_active_.store(false);app.interaction_generation_.fetch_add(1);
 before=app.resumed;board.enrollment.done(true);app.Drain();
 assert(app.resumed==before&&!std::strcmp(app.DeviceVoiceEnrollmentStatus(),"cancelled"));
 app.state=kDeviceStateIdle;assert(app.RequestDeviceVoiceEnrollment("test"));
 app.voice_enrollment_active_.store(false);app.Drain();assert(!std::strcmp(app.DeviceVoiceEnrollmentStatus(),"cancelled"));
 app.interaction_generation_.fetch_add(1);app.voice_enrollment_active_.store(true);
 assert(!std::strcmp(app.DeviceVoiceEnrollmentStatus(),"cancelled"));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / 'test.cc'; cpp.write_text(code)
            exe = Path(tmp) / 'test'
            subprocess.run([compiler, '-std=c++17', '-I', str(ROOT), str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
