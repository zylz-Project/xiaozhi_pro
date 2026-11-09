"""Check IPv4 and IPv4-mapped IPv6 HTTP clients retain network isolation."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WebPeerTest(unittest.TestCase):
    def test_ipv4_mapped_ipv6_and_native_ipv6(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = r'''
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cassert>
#include <cstring>
sockaddr_storage peer{};
socklen_t peer_len;
int test_getpeername(int, sockaddr* out, socklen_t* len) {
    if (*len < peer_len) return -1;
    std::memcpy(out, &peer, peer_len); *len = peer_len; return 0;
}
#define CONFIG_LWIP_IPV6 1
#define getpeername test_getpeername
#include "wzh_web_peer.h"
#include "wzh_user_web_policy.h"
int main() {
    uint32_t ip = 0;
    sockaddr_in v4{}; v4.sin_family = AF_INET; v4.sin_addr.s_addr = htonl(0xc0a80422);
    std::memcpy(&peer, &v4, sizeof(v4)); peer_len = sizeof(v4);
    assert(wzh::WebPeerIpv4(1, ip) && ip == 0xc0a80422);
    assert(wzh::UserWebPolicy::IsApClient(ip));
    sockaddr_in6 v6{}; v6.sin6_family = AF_INET6;
    auto* b = v6.sin6_addr.s6_addr; b[10] = b[11] = 0xff;
    b[12] = 192; b[13] = 168; b[14] = 4; b[15] = 34;
    std::memcpy(&peer, &v6, sizeof(v6)); peer_len = sizeof(v6);
    assert(wzh::WebPeerIpv4(1, ip) && ip == 0xc0a80422);
    assert(wzh::UserWebPolicy::IsApClient(ip));
    b[14] = 1; std::memcpy(&peer, &v6, sizeof(v6));
    assert(wzh::WebPeerIpv4(1, ip) && !wzh::UserWebPolicy::IsApClient(ip));
    b[0] = 0x20; std::memcpy(&peer, &v6, sizeof(v6));
    assert(!wzh::WebPeerIpv4(1, ip));
    peer_len = 2; assert(!wzh::WebPeerIpv4(1, ip));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp, binary = Path(directory) / 'peer.cc', Path(directory) / 'peer'
            cpp.write_text(source)
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(ROOT / 'main/boards/common/wzh'),
                                     str(cpp), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=5)
