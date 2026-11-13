"""Exercise actual SSE framing across arbitrary network read boundaries."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AliSseFramingTest(unittest.TestCase):
    def test_fragmentation_limits_and_callback_failure(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        source = r'''
#include <cassert>
#include <string>
#include <vector>
#include "ali_sse.h"
int main() {
    const std::string wire = ": ping\r\ndata: first\r\ndata: second\r\n\r\n"
                             "data: third\n\ndata: [DONE]\n\n";
    for (size_t chunk = 1; chunk <= wire.size(); ++chunk) {
        std::vector<std::string> events;
        voice_tts::AliSse sse([&](const std::string& event) {
            events.push_back(event); return true;
        });
        for (size_t i = 0; i < wire.size(); i += chunk)
            assert(sse.Feed(wire.data() + i, std::min(chunk, wire.size() - i)));
        assert(sse.Complete());
        assert((events == std::vector<std::string>{"first\nsecond", "third", "[DONE]"}));
    }
    voice_tts::AliSse partial([](const std::string&) { return true; });
    assert(partial.Feed("data: incomplete", 16));
    assert(!partial.Complete());
    voice_tts::AliSse reject([](const std::string&) { return false; });
    assert(!reject.Feed("data: no\n\n", 10));
    voice_tts::AliSse too_big([](const std::string&) { return true; });
    std::string oversize(96 * 1024 + 1, 'a');
    assert(!too_big.Feed(oversize.data(), oversize.size()));
    voice_tts::AliSse multiline([](const std::string&) { return true; });
    std::string large = "data: " + std::string(60 * 1024, 'a') + "\n" +
                        "data: " + std::string(40 * 1024, 'b') + "\n\n";
    assert(!multiline.Feed(large.data(), large.size()));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.cc'
            binary = Path(directory) / 'test'
            test.write_text(source)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/boards/common/wzh/voice_tts'),
                            str(test), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
