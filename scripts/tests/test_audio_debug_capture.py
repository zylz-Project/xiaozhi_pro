"""Validate the real nonblocking, bounded diagnostic audio sender."""
from pathlib import Path
import importlib.util
import struct
import tempfile
import unittest
import wave

from test_wzh_action_queue import queue_function
import test_firmware_runtime_optimizations as runtime_tests

ROOT = Path(__file__).resolve().parents[2]


class AudioDebugCaptureTest(unittest.TestCase):
    run_cpp = runtime_tests.FirmwareRuntimeOptimizationsTest.run_cpp

    def program(self, packetized):
        source = (ROOT/'main/audio/audio_debugger.cc').read_text()
        methods = '\n'.join(queue_function(source, marker) for marker in (
            'AudioDebugger::AudioDebugger()', 'AudioDebugger::~AudioDebugger()',
            'bool AudioDebugger::EnsureSocket(', 'void AudioDebugger::Feed(',
            'void AudioDebugger::FeedProcessed(', 'void AudioDebugger::Send('))
        return ('#define CONFIG_USE_AUDIO_DEBUGGER 1\n'
                f'#define CONFIG_AUDIO_DEBUG_PACKETIZED {int(packetized)}\n'
                '#define CONFIG_AUDIO_DEBUG_DURATION_SECONDS 5\n' + r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>
#include <arpa/inet.h>
#define private public
''' + '#include "' + str(ROOT/'main/audio/audio_debugger.h') + '"\n' + r'''
#undef private
const char*target_address="127.0.0.1:9000";
#define CONFIG_AUDIO_DEBUG_UDP_SERVER target_address
#define ESP_LOGI(...) ((void)0)
bool network_ready=false,drop=false;int socket_calls=0,close_calls=0;
int64_t clock_us=1000;
std::vector<std::vector<uint8_t>>packets;
void*esp_netif_get_default_netif(){return network_ready?&network_ready:nullptr;}
int64_t esp_timer_get_time(){return clock_us;}
int debug_socket(int,int,int){assert(network_ready);++socket_calls;return 42;}
int debug_close(int fd){assert(fd==42);++close_calls;return 0;}
ssize_t debug_sendto(int fd,const void*data,size_t size,int flags,const sockaddr*addr,socklen_t len){
 assert(fd==42&&flags==MSG_DONTWAIT&&len==sizeof(sockaddr_in));
 auto*peer=reinterpret_cast<const sockaddr_in*>(addr);assert(ntohs(peer->sin_port)==9000);
 if(drop)return -1;
 auto*p=static_cast<const uint8_t*>(data);packets.emplace_back(p,p+size);return size;
}
#define socket debug_socket
#define close debug_close
#define sendto debug_sendto
''' + methods + r'''
uint64_t field(const std::vector<uint8_t>&p,size_t offset,size_t bytes){
 uint64_t value=0;for(size_t i=0;i<bytes;++i)value|=uint64_t(p[offset+i])<<(8*i);return value;
}
''')

    def test_packet_formats_sequence_positions_failures_and_capture_deadline(self):
        self.run_cpp(self.program(True)+r'''
int main(){
 std::vector<int16_t>raw(320);for(size_t i=0;i<raw.size();++i)raw[i]=int(i)-160;
 std::vector<int16_t>processed(960,1234);
 AudioDebugger debugger;debugger.Feed(raw,16000,2);
 assert(socket_calls==0&&packets.empty()); // No lwIP call before network initialization.
 network_ready=true;drop=true;debugger.Feed(raw,16000,2);
 assert(socket_calls==1&&debugger.started_us_==-1&&packets.empty());
 drop=false;clock_us+=1000000;debugger.Feed(raw,16000,2);
 assert(packets.size()==1&&debugger.started_us_==clock_us);
 auto&p=packets.front();assert(p.size()==672&&std::memcmp(p.data(),"XDBG",4)==0);
 assert(p[4]==1&&p[5]==0&&p[6]==2&&p[7]==0);
 assert(field(p,8,4)==1&&field(p,12,4)==16000&&field(p,16,4)==160);
 assert(field(p,20,8)==uint64_t(clock_us)&&field(p,28,2)==160&&field(p,30,2)==640);
 assert(std::memcmp(p.data()+32,raw.data(),640)==0);
 debugger.FeedProcessed(processed,true);assert(packets.size()==4);
 for(int i=0;i<3;++i){auto&q=packets[i+1];
  assert(q.size()==672&&q[5]==1&&q[6]==1&&q[7]==1);
  assert(field(q,8,4)==uint64_t(i)&&field(q,16,4)==uint64_t(i*320));
  assert(field(q,28,2)==320&&field(q,30,2)==640);
  assert(std::memcmp(q.data()+32,processed.data()+i*320,640)==0);
 }
 debugger.Feed(raw,16000,3);assert(packets.size()==4); // Reject incomplete channel frames.
 clock_us+=5000000;debugger.Feed(raw,16000,2);debugger.FeedProcessed(processed,true);
 assert(packets.size()==4&&debugger.finished_); // No audio after the hard duration bound.
}
''')

    def test_legacy_wire_format_and_invalid_configuration(self):
        self.run_cpp(self.program(False)+r'''
int main(){
 network_ready=true;std::vector<int16_t>raw(320,321);
 {AudioDebugger debugger;debugger.Feed(raw,16000,2);debugger.FeedProcessed(raw,true);
  assert(packets.size()==1&&packets[0].size()==640);
  assert(std::memcmp(packets[0].data(),raw.data(),640)==0);}
 assert(close_calls==1);int before=socket_calls;
 for(const char*bad:{"","127.0.0.1","bad:9000","127.0.0.1:-1","127.0.0.1:0",
                    "127.0.0.1:65536","127.0.0.1:9000junk","127.0.0.1:"}){
  target_address=bad;AudioDebugger debugger;debugger.Feed(raw);
 }
 assert(socket_calls==before&&packets.size()==1); // No exceptions, sockets or packets.
}
''')

    def receiver(self):
        spec = importlib.util.spec_from_file_location('audio_debug_capture', ROOT/'scripts/audio_debug_capture.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_receiver_rejects_corrupt_packets(self):
        module = self.receiver()
        valid = module.HEADER.pack(b'XDBG', 1, 0, 2, 0, 3, 16000, 0, 1000, 160, 640) + bytes(640)
        info, pcm = module.parse_packet(valid)
        self.assertEqual((info['channels'], info['frames'], len(pcm)), (2, 160, 640))
        malformed = [b'', valid[:31], valid[:-1], valid + b'x']
        for offset, value in [(0, 0), (4, 2), (5, 2), (6, 0), (7, 4), (12, 0), (28, 0)]:
            changed = bytearray(valid)
            changed[offset] = value
            malformed.append(bytes(changed))
        for data in malformed:
            with self.subTest(data=data[:32]):
                with self.assertRaises(ValueError):
                    module.parse_packet(data)

    def test_receiver_preserves_sample_positions_and_reports_loss_and_duplicates(self):
        module = self.receiver()
        with tempfile.TemporaryDirectory() as directory:
            capture = module.Capture(Path(directory)/'trace')
            payload = struct.pack('<320h', *range(320))
            def packet(position, sequence):
                return module.HEADER.pack(b'XDBG', 1, 0, 2, 0, sequence, 16000,
                                          position, 1000, 160, 640) + payload
            self.assertTrue(capture.accept(packet(1000, 10)))
            self.assertTrue(capture.accept(packet(1320, 12)))
            self.assertFalse(capture.accept(packet(1320, 12)))
            self.assertFalse(capture.accept(b'bad'))
            capture.close()
            with wave.open(str(Path(directory)/'trace/raw-mr.wav')) as audio:
                self.assertEqual((audio.getnchannels(), audio.getframerate(), audio.getnframes()),
                                 (2, 16000, 480))
                self.assertEqual(audio.readframes(480), payload + bytes(640) + payload)
            summary = capture.summary()
            self.assertEqual(summary['streams']['0']['missing_frames'], 160)
            self.assertEqual((summary['invalid_packets'], summary['reordered_packets']), (1, 1))

    def test_receiver_refuses_to_mix_audio_across_device_reboots(self):
        module = self.receiver()
        with tempfile.TemporaryDirectory() as directory:
            capture = module.Capture(Path(directory)/'trace')
            def packet(position, clock):
                return module.HEADER.pack(b'XDBG', 1, 0, 2, 0, 1, 16000,
                                          position, clock, 160, 640) + bytes(640)
            self.assertTrue(capture.accept(packet(1000, 20000000)))
            self.assertFalse(capture.accept(packet(900, 19900000)))
            self.assertFalse(capture.restarted)  # A late packet is not a reboot.
            self.assertFalse(capture.accept(packet(0, 1000000)))
            self.assertTrue(capture.restarted)
            self.assertFalse(capture.accept(packet(1160, 21000000)))
            capture.close()
            self.assertTrue(capture.summary()['device_restarted'])
        with tempfile.TemporaryDirectory() as directory:
            capture = module.Capture(Path(directory)/'trace', max_seconds=1)
            for position in range(0, 16000, 160):
                self.assertTrue(capture.accept(packet(position, position * 1000)))
            self.assertFalse(capture.accept(packet(16000, 16000000)))
            self.assertTrue(capture.limit_reached)
            capture.close()


if __name__ == '__main__':
    unittest.main()
