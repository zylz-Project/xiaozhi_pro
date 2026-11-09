"""Exercise DNS responses used by phone captive-portal detection."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CaptiveDnsTest(unittest.TestCase):
    def test_dns_queries_with_edns_ipv6_and_invalid_lengths(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        program = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include "captive_dns_response.h"
int main() {
    const uint8_t ip[] = {192, 168, 4, 1};
    uint8_t q[512] = {0x12,0x34,1,0,0,1,0,0,0,0,0,1,
                      1,'a',0,0,1,0,1,
                      0,0,41,4,208,0,0,0,0,0,0};
    const size_t question = 19;
    auto n = BuildCaptiveDnsResponse(q, 30, sizeof(q), ip);
    assert(n == question + 16 && q[0] == 0x12 && q[1] == 0x34);
    assert(q[2] == 0x81 && q[3] == 0x80 && q[7] == 1 && q[11] == 0);
    assert(q[question] == 0xc0 && q[question+1] == 0x0c);
    assert(std::memcmp(q + n - 4, ip, 4) == 0);

    uint8_t ipv6[512] = {0,1,1,0,0,1,0,0,0,0,0,0,1,'a',0,0,28,0,1};
    n = BuildCaptiveDnsResponse(ipv6, question, sizeof(ipv6), ip);
    assert(n == question && ipv6[7] == 0 && ipv6[11] == 0);

    uint8_t truncated[512] = {0,1,1,0,0,1,0,0,0,0,0,0,63,'a'};
    assert(BuildCaptiveDnsResponse(truncated, 14, sizeof(truncated), ip) == 0);
    uint8_t compressed[512] = {0,1,1,0,0,1,0,0,0,0,0,0,0xc0,0x0c,0,1,0,1};
    assert(BuildCaptiveDnsResponse(compressed, 18, sizeof(compressed), ip) == 0);
    uint8_t small[] = {0,1,1,0,0,1,0,0,0,0,0,0,1,'a',0,0,1,0,1};
    assert(BuildCaptiveDnsResponse(small, sizeof(small), sizeof(small), ip) == 0);
    uint8_t unterminated[] = {0,1,1,0,0,1,0,0,0,0,0,0,4,'a','b','c',0};
    assert(BuildCaptiveDnsResponse(unterminated, sizeof(unterminated), sizeof(unterminated), ip) == 0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'dns.cc'
            binary = Path(directory) / 'dns'
            source.write_text(program)
            result = subprocess.run(
                ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                 '-I', str(ROOT / 'project_components/esp-wifi-connect/include'),
                 str(source), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=5)
