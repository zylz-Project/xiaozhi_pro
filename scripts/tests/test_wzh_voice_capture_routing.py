"""Exercise the production processed-audio hook before any uplink encoding."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class VoiceCaptureRoutingTest(unittest.TestCase):
    def test_locally_consumed_capture_never_reaches_encoder(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        source = (ROOT / 'main/audio/audio_service.cc').read_text()
        start = source.index('    audio_engine_->OnOutput(')
        end = source.index('    audio_engine_->OnVadStateChange(', start)
        callback = source[start:end]
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>
constexpr int kAudioTaskTypeEncodeToSendQueue=1;
struct Engine {
 std::function<void(std::vector<int16_t>&&)> output;
 void OnOutput(decltype(output) callback){output=std::move(callback);}
};
struct AudioService {
 Engine engine;Engine* audio_engine_=&engine;
 struct {std::function<bool(const int16_t*,size_t,bool)> on_processed_audio;} callbacks_;
 std::atomic<bool> voice_detected_{true};
 int encoded=0;
 void PushTaskToEncodeQueue(int type,std::vector<int16_t>&& data){
  assert(type==1&&data==std::vector<int16_t>({7,8,9}));++encoded;
 }
 void InitializeHook(){
''' + callback + r'''
 }
};
int main(){
 AudioService audio;audio.InitializeHook();
 audio.engine.output(std::vector<int16_t>{7,8,9});assert(audio.encoded==1);
 int captured=0;
 audio.callbacks_.on_processed_audio=[&](const int16_t* pcm,size_t count,bool speech){
  assert(count==3&&pcm[0]==7&&pcm[2]==9&&speech);++captured;return true;
 };
 audio.engine.output(std::vector<int16_t>{7,8,9});
 assert(captured==1&&audio.encoded==1);
 audio.callbacks_.on_processed_audio=[](const int16_t*,size_t,bool){return false;};
 audio.engine.output(std::vector<int16_t>{7,8,9});assert(audio.encoded==2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / 'capture.cc'
            source_path.write_text(program)
            executable = source_path.with_suffix('')
            result = subprocess.run(
                ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                 str(source_path), '-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(executable)], check=True, timeout=10)
