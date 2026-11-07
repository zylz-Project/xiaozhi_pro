"""Verify device-scoped Ali prefixes and model-specific built-in voices."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class VoiceIdentityTest(unittest.TestCase):
    def test_prefix_is_fixed_width_and_preserves_all_mac_bits(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = r'''
#include "voice_identity.h"
#include <cassert>
#include <cstdint>
#include <set>
#include <string>
int main() {
 using namespace voice_tts;
 uint8_t zero[6]={}, high[6]={255,255,255,255,255,255};
 assert(VoicePrefixFromMac(nullptr,6).empty());
 assert(VoicePrefixFromMac(zero,5).empty());
 assert(VoicePrefixFromMac(zero,6)=="0000000000");
 assert(VoicePrefixFromMac(high,6).size()==10);
 std::set<std::string> all;
 for (int i=0;i<65536;++i) {
  uint8_t m[6]={0x14,0xc1,0x9f,0xc6,static_cast<uint8_t>(i>>8),static_cast<uint8_t>(i)};
  auto id=VoicePrefixFromMac(m,6);assert(id.size()==10 && all.insert(id).second);
  assert(id.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyz")==std::string::npos);
 }
 assert(kBuiltinVoiceCount>=4);
 for(const auto& voice:kBuiltinVoices) assert(*voice.name && *voice.id && *voice.description);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'test.cc'
            cpp.write_text(source)
            exe = cpp.with_suffix('')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/boards/common/wzh/voice_tts'),
                            str(cpp), '-o', str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, timeout=10)
