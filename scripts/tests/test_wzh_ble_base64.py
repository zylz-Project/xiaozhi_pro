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
 std::vector<uint8_t> decoded;
 assert(Base64Decode(encoded,decoded));assert(decoded==original);
 assert(!Base64Decode("AAAA?",decoded));
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
