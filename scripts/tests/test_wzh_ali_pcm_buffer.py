"""Exercise the Ali Ogg stream adapter using the production demuxer."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / 'main/audio'


class AliOpusPlaybackTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_fragmented_ogg_preserves_duration_trim_cancel_and_end(self):
        source = (ROOT / 'main/boards/common/wzh/voice_tts/voice_session.cc').read_text()
        wrapper = source[source.index('class AliOpusPlayback {'):source.index('}  // namespace')]
        header = r'''
#define CONFIG_BOARD_TYPE_WZH_TAILREDPANDA 1
#include <cassert>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>
#include "ogg_demuxer.h"
#define ESP_LOGI(...) ((void)0)
int64_t esp_timer_get_time(){return 1000;}
constexpr int MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
bool fail_alloc=false;
void* heap_caps_malloc(size_t size,int caps){
 assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 return fail_alloc ? nullptr : std::malloc(size);
}
struct AudioStreamPacket {
 int sample_rate=0, frame_duration=0;
 OpusStreamMetadata stream;
 std::vector<uint8_t> payload;
 struct FreeBytes{void operator()(uint8_t* bytes)const{std::free(bytes);}};
 std::unique_ptr<uint8_t, FreeBytes> external_opus;
 size_t external_opus_size=0;
};
struct AudioService {
 uint32_t generation=7;
 std::vector<std::unique_ptr<AudioStreamPacket>> packets;
 bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet,bool wait,uint32_t token) {
  assert(wait);
  if(token!=generation)return false;
  packets.push_back(std::move(packet));
  return true;
 }
};
'''
        test = r'''
std::vector<uint8_t> page(unsigned flags,unsigned granule,unsigned sequence,
                          std::vector<uint8_t> packet) {
 std::vector<uint8_t> result(packet.empty() ? 27 : 28,0);
 std::memcpy(result.data(),"OggS",4);
 result[5]=flags;
 result[6]=granule&255;result[7]=(granule>>8)&255;
 result[14]=1;result[18]=sequence;
 if (!packet.empty()) {
  result[26]=1;result[27]=packet.size();
  result.insert(result.end(),packet.begin(),packet.end());
 }
 uint32_t checksum=0;
 for (const auto byte:result) {
  checksum ^= uint32_t(byte)<<24;
  for (int bit=0;bit<8;++bit)
   checksum=(checksum<<1) ^ ((checksum&0x80000000u) ? 0x04c11db7u : 0);
 }
 for (unsigned i=0;i<4;++i) result[22+i]=checksum>>(8*i);
 return result;
}
int main(){
 AudioService svc;
 AliOpusPlayback stream(svc,7,100);
 stream.BeginRequest(200);
 const std::vector<uint8_t> head={'O','p','u','s','H','e','a','d',1,1,0x38,1,0xc0,0x5d,0,0,0,0,0};
 const auto tags=page(0,0,1,{'O','p','u','s','T','a','g','s'});
 const auto first=page(0,960,2,{0x98,1,1});
 const auto last=page(4,1760,3,{0x98,1,1});
 const auto send=[&](const std::vector<uint8_t>& bytes){
  for(auto byte:bytes)assert(stream.Write(&byte,1));
 };
 const auto begin=[&]{stream.BeginRequest(200);send(page(2,0,0,head));send(tags);};
 begin();send(first);assert(stream.Complete()&&!stream.HasEndOfStream());
 send(last);assert(stream.Complete());assert(svc.packets.size()==2);
 auto& a=*svc.packets.front();auto& b=*svc.packets.back();
 assert(a.sample_rate==48000&&a.frame_duration==20&&a.stream.start&&a.stream.skip_48k==312);
 assert(a.stream.continuous_output&&b.stream.continuous_output);
 assert(b.frame_duration==20&&!b.stream.end&&b.stream.keep_end_48k==800);
 assert(a.payload.empty()&&a.external_opus_size==3&&a.external_opus.get()[0]==0x98);
 begin();send(first);send(page(4,960,3,{}));
 assert(stream.Complete()&&stream.HasEndOfStream());
 begin();send(first);assert(stream.Write(last.data(),last.size()-1));
 assert(!stream.Complete());
 begin();send(first);auto corrupt=last;corrupt.back()^=1;send(corrupt);
 assert(!stream.Complete());
 begin();send(first);auto missing=page(4,1760,4,{0x98,1,1});
 assert(!stream.Write(missing.data(),missing.size())&&!stream.Complete());
 begin();svc.generation=8;
 const auto before=svc.packets.size();
 assert(!stream.Write(first.data(),first.size())&&!stream.Complete());
 assert(svc.packets.size()==before);
 begin();svc.generation=7;fail_alloc=true;
 assert(!stream.Write(first.data(),first.size())&&!stream.Complete());
 assert(svc.packets.size()==before);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            (temp / 'esp_log.h').write_text(
                '#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n'
                '#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGD(...) ((void)0)\n')
            (temp / 'test.cc').write_text(header + wrapper + test)
            subprocess.run(['g++', '-std=c++20', '-I', str(temp), '-I', str(AUDIO),
                            '-I', str(AUDIO / 'demuxer'), str(temp / 'test.cc'),
                            str(AUDIO / 'demuxer/ogg_demuxer.cc'), '-o', str(temp / 'test')],
                           check=True)
            subprocess.run([str(temp / 'test')], check=True, timeout=10)
