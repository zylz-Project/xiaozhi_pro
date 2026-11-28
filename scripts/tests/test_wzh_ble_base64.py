import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class BleBase64Test(unittest.TestCase):
    def test_long_audio_chunks_and_invalid_input(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/boards/common/wzh/wzh_ble_service.cc').read_text()
        digit = queue_function(source, 'int Base64Value(')
        decode = queue_function(source, 'bool Base64Decode(')
        program = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
''' + digit + '\n' + decode + r'''
int main() {
 const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
 std::vector<uint8_t> original;
 for (int i=0;i<2048;++i) original.push_back((i*131+42)&255);
 std::string encoded;
 for (size_t i=0;i<original.size();i+=3) {
  int a=original[i],b=i+1<original.size()?original[i+1]:0,c=i+2<original.size()?original[i+2]:0;
  encoded+=alphabet[a>>2];encoded+=alphabet[((a&3)<<4)|(b>>4)];
  encoded+=i+1<original.size()?alphabet[((b&15)<<2)|(c>>6)]:'=';
  encoded+=i+2<original.size()?alphabet[c&63]:'=';
 }
 std::vector<uint8_t> decoded(2049, 0xDE);
 size_t written = 0;
 assert(Base64Decode(encoded, decoded.data(), 2048, written));
 assert(written==2048 && decoded.back()==0xDE);
 assert(std::vector<uint8_t>(decoded.begin(),decoded.begin()+written)==original);
 assert(!Base64Decode(encoded, decoded.data(), 2046, written));
 assert(written<=2046 && decoded.back()==0xDE);
 for (std::string_view bad : {"", "AAAA?", "AAAA====", "AA=A", "Aa==", "AAB=", "AAAA AAA"})
  assert(!Base64Decode(bad, decoded.data(), 2048, written));
 for (std::string_view good : {"AA==", "AAA=", "AAAA", "AAAAAA=="})
  assert(Base64Decode(good, decoded.data(), 2048, written));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'base64.cc'
            binary = Path(directory) / 'base64'
            cpp.write_text(program)
            subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            str(cpp), '-o', str(binary)], check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
