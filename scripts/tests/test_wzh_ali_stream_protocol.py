"""Validate task isolation and ordering for the production stream parser."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AliStreamProtocolTest(unittest.TestCase):
    def test_task_and_audio_ordering(self):
        if not shutil.which('g++') or not shutil.which('gcc'):
            self.skipTest('C and C++ compilers required')
        code = r'''
#include <cassert>
#include <string>
#include "ali_stream_protocol.h"
using P = voice_tts::AliStreamProtocol;
std::string event(const char* task, const char* type, const char* output = "") {
 return std::string("{\"header\":{\"task_id\":\"") + task + "\",\"event\":\"" +
 type + "\"},\"payload\":{\"output\":{\"type\":\"" + output + "\"}}}";
}
P::Event text(P& p, const std::string& s) { return p.OnText(s.data(), s.size()); }
int main() {
 P good("task");
 assert(text(good,event("task","task-started")) == P::Event::kStarted);
 assert(text(good,event("task","result-generated","sentence-begin")) == P::Event::kOther);
 assert(text(good,event("task","result-generated","sentence-synthesis")) == P::Event::kAudioFollows);
 assert(good.OnAudioMessage());
 assert(text(good,event("task","result-generated","sentence-end")) == P::Event::kSentenceEnd);
 assert(good.MarkFinishSent());
 assert(text(good,event("task","task-finished")) == P::Event::kFinished);
 assert(text(good,event("task","task-finished")) == P::Event::kError);
 P wrong("task"); assert(text(wrong,event("other","task-started")) == P::Event::kError);
 P early("task"); assert(text(early,event("task","task-finished")) == P::Event::kError);
 P missing("task"); assert(text(missing,event("task","task-started")) == P::Event::kStarted);
 assert(text(missing,event("task","result-generated","sentence-synthesis")) == P::Event::kAudioFollows);
 assert(text(missing,event("task","result-generated","sentence-end")) == P::Event::kError);
 P unexpected("task"); assert(!unexpected.OnAudioMessage());
 P oversized("task"); std::string big(8193,'x'); assert(text(oversized,big) == P::Event::kError);
 P nested("task"); std::string deep(33,'['); deep += "0"; deep += std::string(33,']');
 assert(text(nested,deep) == P::Event::kError);
 P malformed("task"); auto s=event("task","task-started") + "x";
 assert(text(malformed,s) == P::Event::kError);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / 'test.cc').write_text(code)
            json = ROOT / 'managed_components/espressif__cjson/cJSON'
            provider = ROOT / 'main/boards/common/wzh/voice_tts'
            subprocess.run(['gcc', '-c', str(json / 'cJSON.c'), '-o', str(p / 'json.o')], check=True)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(provider), '-I', str(json),
                            str(provider / 'ali_stream_protocol.cc'), str(p / 'test.cc'),
                            str(p / 'json.o'), '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True, timeout=10)
