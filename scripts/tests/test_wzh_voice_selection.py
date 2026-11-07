"""Exercise the production two-turn voice selection logic with a simulated speaker."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function
ROOT=Path(__file__).resolve().parents[2]

class VoiceSelectionTest(unittest.TestCase):
    def test_listing_selection_and_timeout(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        voice=ROOT/'main/boards/common/wzh/voice_tts'
        source=(voice/'ali_voice_enrollment.cc').read_text()
        methods='\n'.join(queue_function(source, marker) for marker in [
            'bool AliVoiceEnrollment::SelectionPending(', 'void AliVoiceEnrollment::RunSelection('])
        program=r'''
#include "voice_identity.h"
#include "voice_names.h"
#include "voice_manager.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
int64_t now_us=100;int64_t esp_timer_get_time(){return now_us;}
namespace voice_tts {
std::string current="old";bool save_ok=true,load_ok=true;std::vector<std::string> spoken;
bool LoadVoiceNames(std::vector<VoiceName>& out){out={{"voice-mum","妈妈"},{"voice-long","妈妈的声音"}};return load_ok;}
bool SaveAliVoice(const std::string& id){if(!save_ok)return false;current=id;return true;}
VoiceOperation SelectKnownVoice(const std::string& id,const std::function<bool()>& cancelled){if(cancelled())return VoiceOperation::kCancelled;return SaveAliVoice(id)?VoiceOperation::kOk:VoiceOperation::kSave;}
class AliVoiceEnrollment {
public:
 std::mutex mutex_;std::atomic<int64_t> selection_deadline_us_{0};std::atomic<uint32_t> generation_{1};
 std::atomic<bool> active_{true};bool selection_task_=true;std::string selection_text_;void* task_=(void*)1;
 std::function<void(bool)> complete_;bool answer=true;
 bool Speak(const char* t,uint32_t){spoken.emplace_back(t);return answer;}
 bool SelectionPending()const;void RunSelection(uint32_t,const std::string&);
};
''' + methods + r'''
}
int main(){using namespace voice_tts;
 AliVoiceEnrollment a;int finished=0;a.complete_=[&](bool ok){assert(ok);++finished;};
 a.RunSelection(1,"切换音色");assert(finished==1 && a.SelectionPending());
 assert(spoken.size()==1 && spoken[0].find("妈妈")!=std::string::npos && spoken[0].find("龙安欢")!=std::string::npos);
 a.active_=true;a.complete_=[&](bool ok){assert(ok);++finished;};
 a.RunSelection(1,"请使用妈妈的声音");assert(current=="voice-long"&&!a.SelectionPending()&&finished==2);
 assert(spoken.back().find("切换音色完成")!=std::string::npos);
 a.active_=true;a.complete_=[&](bool ok){assert(ok);};a.RunSelection(1,"切换音色");
 now_us+=60000001;assert(!a.SelectionPending());
 a.active_=true;a.complete_=[&](bool ok){assert(ok);};a.RunSelection(1,"没有这个人");
 assert(current=="voice-long"&&spoken.back().find("没有找到")!=std::string::npos);
}
'''
        with tempfile.TemporaryDirectory() as d:
            cpp=Path(d)/'test.cc';cpp.write_text(program);exe=cpp.with_suffix('')
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-I',str(voice),str(cpp),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(exe)],check=True,timeout=10)
