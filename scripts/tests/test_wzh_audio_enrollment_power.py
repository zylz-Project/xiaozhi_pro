"""The clone's wait keeps the existing output open, without keeping input on."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function
ROOT=Path(__file__).resolve().parents[2]

class EnrollmentAudioPowerTest(unittest.TestCase):
    def test_output_hold_and_input_timeout(self):
        if not shutil.which('g++'): self.skipTest('g++ required')
        body=queue_function((ROOT/'main/audio/audio_service.cc').read_text(), 'void AudioService::CheckAndUpdateAudioPowerState(')
        code=r'''
#include <cassert>
#include <chrono>
constexpr int AUDIO_POWER_TIMEOUT_MS=15000,AS_EVENT_AUDIO_INPUT_STOP_REQUEST=16;
int stop_requests=0,timer_stops=0;
void xEventGroupSetBits(void*,int){++stop_requests;}
void esp_timer_stop(void*){++timer_stops;}
struct Codec {bool in=true,out=true,is_duplex=false;
 bool input_enabled(){return in;} bool output_enabled(){return out;} bool duplex(){return is_duplex;}
 void EnableOutput(bool b){out=b;}};
struct Board {bool hold=false;static Board& GetInstance(){static Board b;return b;}
 bool KeepAudioOutputEnabled(){return hold;}};
struct AudioService {Codec codec;Codec* codec_=&codec;void* event_group_=nullptr;void* audio_power_timer_=nullptr;
 std::chrono::steady_clock::time_point last_input_time_,last_output_time_;
 void CheckAndUpdateAudioPowerState();};
'''+body+r'''
int main(){AudioService s;s.last_input_time_=s.last_output_time_=std::chrono::steady_clock::now()-std::chrono::seconds(20);
 Board::GetInstance().hold=true;s.CheckAndUpdateAudioPowerState();assert(stop_requests==1&&s.codec.out);
 s.codec.in=false;Board::GetInstance().hold=false;s.CheckAndUpdateAudioPowerState();assert(!s.codec.out&&timer_stops==1);
 s.codec.in=true;s.codec.out=true;s.codec.is_duplex=true;s.CheckAndUpdateAudioPowerState();assert(s.codec.out);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'test.cc';p.write_text(code);exe=p.with_suffix('')
            result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(exe)],check=True,timeout=10)
