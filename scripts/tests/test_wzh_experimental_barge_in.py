"""Exercise bounded software reference and sustained-speech interrupt gate."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ExperimentalBargeInTest(unittest.TestCase):
    def test_codec_forwards_post_processed_speaker_reference(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        codec = r'''
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
struct Filter { void Reset(){} void Process(std::vector<int16_t>& v){for(auto& x:v)x*=2;} };
struct AudioCodec {
 bool input_reference_=false,speaker_protection_enabled_=true;int input_channels_=1;
 std::atomic<bool> output_processing_reset_pending_{true};Filter speaker_protection_;
 virtual ~AudioCodec()=default;
 virtual bool InputData(std::vector<int16_t>& data){for(auto& x:data)x=7;return true;}
 virtual void OutputData(std::vector<int16_t>&){}
};
struct Es8311AudioCodec:AudioCodec {
 explicit Es8311AudioCodec(int){}
 std::vector<int16_t> written;
protected:
 int Write(const int16_t* p,int n){written.assign(p,p+n);return n;}
};
'''
        program = r'''
#include <cassert>
#include "panda_software_aec_codec.h"
int64_t test_clock=1000000;
int main(){
 PandaSoftwareAecCodec codec(1);
 assert(codec.input_reference_&&codec.input_channels_==2);
 std::vector<int16_t> audio(1200,3);codec.OutputData(audio);
 assert(codec.written.size()==1200 && codec.written[0]==6);
 std::vector<int16_t> input(2400);assert(codec.InputData(input));
 for(size_t i=0;i<1200;++i){assert(input[2*i]==7);assert(input[2*i+1]==(i<960?0:6));}
 test_clock+=300000;std::vector<int16_t> silence(480);assert(codec.InputData(silence));
 for(size_t i=0;i<240;++i)assert(silence[2*i+1]==0);
 std::vector<int16_t> odd(3);assert(!codec.InputData(odd));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'codecs').mkdir()
            (path / 'codecs/es8311_audio_codec.h').write_text(codec)
            (path / 'esp_timer.h').write_text('#pragma once\n#include <cstdint>\nextern int64_t test_clock;\ninline int64_t esp_timer_get_time(){return test_clock;}\n')
            (path / 'test.cc').write_text(program)
            result = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-I', str(path), '-I', str(ROOT / 'main/boards/wzh_crawlPanda'),
                str(path / 'test.cc'), '-o', str(path / 'test')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / 'test')], check=True)

    def test_reference_alignment_overflow_and_guard(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        program = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include "panda_echo_reference.h"
#include "direct_barge_guard.h"
int main(){
 PandaEchoReference ref;
 std::vector<int16_t> in(2000), out(4000,123);
 for(size_t i=0;i<in.size();++i)in[i]=int16_t(i+1);
 ref.Push(in.data(),in.size());ref.PullInterleaved(out.data(),in.size());
 for(size_t i=0;i<in.size();++i){
  assert(out[2*i]==123);
  assert(out[2*i+1]==(i<960?0:in[i-960]));
 }
 ref.Clear();std::vector<int16_t> large(5000);
 for(size_t i=0;i<large.size();++i)large[i]=int16_t(i+1);
 ref.Push(large.data(),large.size());
 std::vector<int16_t> wrap(2*1000,0);ref.PullInterleaved(wrap.data(),1000);
 assert(wrap[2*960+1]==201);
 DirectBargeGuard g;
 assert(!g.Update(true,true,100000)); // old voice tail does not arm
 assert(!g.Update(true,true,2000000));
 assert(!g.Update(true,false,2100000));
 assert(!g.Update(true,true,2200000));
 assert(!g.Update(true,true,2800000));
 assert(g.Update(true,true,2900000));
 assert(!g.Update(true,true,4000000)); // no retrigger without silence
 assert(!g.Update(false,false,4100000));
 assert(!g.Update(true,false,4200000));
 assert(!g.Update(true,true,4300000));
 assert(!g.Update(true,false,4400000)); // brief sound cancels
 assert(!g.Update(true,true,4500000));
 assert(!g.Update(true,true,5000000));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cc').write_text(program)
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'main/boards/wzh_crawlPanda'),
                '-I', str(ROOT / 'main/audio'), str(path / 'test.cc'), '-o', str(path / 'test')],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / 'test')], check=True)
