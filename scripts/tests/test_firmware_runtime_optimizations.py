"""Exercise upstream backports and bounded audio storage with production C++."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / 'main/audio'
JSON = ROOT / 'managed_components/espressif__cjson/cJSON'


class FirmwareRuntimeOptimizationsTest(unittest.TestCase):
    def run_cpp(self, program, sources=(), use_json=False):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / 'esp_log.h').write_text(
                '#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGD(...) ((void)0)\n')
            (directory / 'esp_system.h').write_text(
                '#include <cstdlib>\n[[noreturn]] inline void esp_system_abort(const char*){std::abort();}\n')
            source = directory / 'test.cc'
            source.write_text(program)
            objects = []
            if use_json:
                obj = directory / 'cjson.o'
                subprocess.run(['gcc', '-c', str(JSON / 'cJSON.c'), '-o', str(obj)],
                               check=True, capture_output=True)
                objects.append(str(obj))
            result = subprocess.run(
                [compiler, '-std=c++17', '-fno-exceptions', '-pthread', '-Wall', '-Wextra', '-Werror',
                 '-I', str(directory), '-I', str(AUDIO), '-I', str(JSON),
                 str(source), *map(str, sources), *objects, '-o', str(directory / 'test')],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(directory / 'test')], capture_output=True,
                                    text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_ota_rejects_malformed_and_overflowing_versions_without_exceptions(self):
        source = (ROOT / 'main/ota.cc').read_text()
        self.run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <charconv>
#include <optional>
#include <string>
#include <vector>
#define ESP_LOGW(...) ((void)0)
struct Ota {
 std::optional<std::vector<int>> ParseVersion(const std::string&);
 bool IsNewVersionAvailable(const std::string&,const std::string&);
};
''' + queue_function(source, 'std::optional<std::vector<int>> Ota::ParseVersion(')
            + queue_function(source, 'bool Ota::IsNewVersionAvailable(') + r'''
int main(){
 Ota ota;
 for(const char* bad:{"",".","2..5","2.5.","v2.5.2","2.x.1","2.-1.0","2.+1.0",
                      "2.-0.1","2.5.2suffix"," 2.5.2","2.5.2 ","99999999999999999999999.1"}){
  assert(!ota.ParseVersion(bad));
  assert(!ota.IsNewVersionAvailable("2.4.1",bad));
  assert(!ota.IsNewVersionAvailable(bad,"2.5.2"));
 }
 assert(ota.IsNewVersionAvailable("2.4.1","2.5.2"));
 assert(!ota.IsNewVersionAvailable("2.5.2","2.5.2"));
 assert(!ota.IsNewVersionAvailable("2.5.2","2.4.99"));
 assert(!ota.IsNewVersionAvailable("10.0.0","9.99.99"));
 assert(ota.IsNewVersionAvailable("2.5","2.5.1"));
}
''')

    def test_websocket_hello_rejects_non_string_transport(self):
        source = (ROOT / 'main/protocols/websocket_protocol.cc').read_text()
        self.run_cpp(r'''
#include <cassert>
#include <cstring>
#include <string>
#include "cJSON.h"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr unsigned WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT=1;
void xEventGroupSetBits(unsigned* bits,unsigned value){*bits|=value;}
struct WebsocketProtocol {
 unsigned bits=0;unsigned*event_group_handle_=&bits;
 std::string session_id_;int server_sample_rate_=16000,server_frame_duration_=60;
 void ParseServerHello(const cJSON*);
};
''' + queue_function(source, 'void WebsocketProtocol::ParseServerHello(') + r'''
int main(){
 for(const char* bad:{"{}","{\"transport\":null}","{\"transport\":5}",
                      "{\"transport\":true}","{\"transport\":{}}","{\"transport\":\"udp\"}"}){
  auto* json=cJSON_Parse(bad);assert(json);
  WebsocketProtocol protocol;protocol.ParseServerHello(json);
  assert(protocol.bits==0&&protocol.session_id_.empty());cJSON_Delete(json);
 }
 auto*json=cJSON_Parse("{\"transport\":\"websocket\",\"session_id\":\"new\","
                        "\"audio_params\":{\"sample_rate\":24000,\"frame_duration\":20}}");
 WebsocketProtocol protocol;protocol.ParseServerHello(json);
 assert(protocol.bits==1&&protocol.session_id_=="new"&&protocol.server_sample_rate_==24000);
 assert(protocol.server_frame_duration_==20);cJSON_Delete(json);
}
''', use_json=True)

    def test_mqtt_send_releases_channel_lock_and_keeps_transport_alive(self):
        source = (ROOT / 'main/protocols/mqtt_protocol.cc').read_text()
        self.run_cpp(r'''
#include <arpa/inet.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#define ESP_LOGE(...) ((void)0)
struct AudioStreamPacket{uint32_t timestamp=42;std::vector<uint8_t>payload={1,2,3};};
struct Udp{
 std::function<int(const std::string&)>send;
 int Send(const std::string& bytes){return send(bytes);}
};
struct MqttProtocol{
 std::mutex channel_mutex_;std::shared_ptr<Udp>udp_;
 std::string aes_nonce_=std::string(16,0);uint32_t local_sequence_=0;
 bool close_during_crypto=false;
 bool CryptAesCtr(const uint8_t*in,size_t n,const uint8_t*,uint8_t*out){
  assert(channel_mutex_.try_lock());channel_mutex_.unlock();
  if(close_during_crypto){udp_.reset();}
  std::memcpy(out,in,n);return true;
 }
 bool SendAudio(std::unique_ptr<AudioStreamPacket>);
};
''' + queue_function(source, 'bool MqttProtocol::SendAudio(') + r'''
int main(){
 MqttProtocol protocol;
 assert(!protocol.SendAudio(std::make_unique<AudioStreamPacket>()));
 int sends=0;
 protocol.udp_=std::make_shared<Udp>();std::weak_ptr<Udp>alive=protocol.udp_;
 protocol.udp_->send=[&](const std::string&bytes){
  assert(!alive.expired());assert(protocol.channel_mutex_.try_lock());protocol.channel_mutex_.unlock();
  assert(bytes.size()==19&&bytes.substr(16)==std::string("\1\2\3",3));
  uint16_t size;uint32_t timestamp,sequence;
  std::memcpy(&size,bytes.data()+2,2);std::memcpy(&timestamp,bytes.data()+8,4);
  std::memcpy(&sequence,bytes.data()+12,4);
  assert(ntohs(size)==3&&ntohl(timestamp)==42&&ntohl(sequence)==1);++sends;return 19;
 };
 protocol.close_during_crypto=true;
 assert(protocol.SendAudio(std::make_unique<AudioStreamPacket>()));
 assert(sends==1&&alive.expired()&&!protocol.udp_);
}
''')

    def test_fixed_queue_wraparound_overflow_swap_and_move_only_lifetimes(self):
        self.run_cpp(r'''
#include <cassert>
#include <memory>
#include "fixed_queue.h"
int main(){
 FixedQueue<std::unique_ptr<int>,3>a,b;
 assert(a.empty()&&a.capacity()==3);
 for(int i=0;i<3;++i)assert(a.push_back(std::make_unique<int>(i)));
 assert(a.full()&&!a.push_back(std::make_unique<int>(99)));
 for(int round=0;round<1000;++round){
  int next=*a.front()+3;a.pop_front();assert(a.push_back(std::make_unique<int>(next)));
  assert(*a[2]==next&&a.size()==3);
 }
 int first=*a.front();a.swap(b);assert(a.empty()&&*b.front()==first);
 auto owned=std::move(b.front());b.pop_front();b.clear();assert(*owned==first&&b.empty());
 // A recorded test can be transferred in full, beyond the 96-packet live budget.
 FixedQueue<std::unique_ptr<int>,166>recorded,decode;
 for(int i=0;i<166;++i)assert(recorded.push_back(std::make_unique<int>(i)));
 decode.swap(recorded);assert(recorded.empty()&&decode.size()==166);
 for(int i=0;i<166;++i){assert(*decode.front()==i);decode.pop_front();}
}
''')

    def test_ogg_zero_length_terminal_lace_survives_every_chunk_boundary(self):
        self.run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "demuxer/ogg_demuxer.h"
void Page(std::vector<uint8_t>&out,const std::vector<uint8_t>&laces,
          const std::vector<uint8_t>&body){
 size_t pos=out.size();out.resize(pos+27,0);std::memcpy(out.data()+pos,"OggS",4);
 out[pos+26]=laces.size();out.insert(out.end(),laces.begin(),laces.end());
 out.insert(out.end(),body.begin(),body.end());
}
int main(){
 std::vector<uint8_t>head(19,0);std::memcpy(head.data(),"OpusHead",8);
 head[9]=1;head[12]=0xc0;head[13]=0x5d;
 std::vector<uint8_t>tags={'O','p','u','s','T','a','g','s'};
 for(bool continued:{false,true}){
  std::vector<uint8_t>stream,body=head;body.insert(body.end(),tags.begin(),tags.end());
  body.insert(body.end(),255,42);Page(stream,continued?std::vector<uint8_t>{19,8,255}:
                                                       std::vector<uint8_t>{19,8,255,0},body);
  if(continued)Page(stream,{255,0},std::vector<uint8_t>(255,42));
  for(size_t chunk=1;chunk<=stream.size();++chunk){
   OggDemuxer parser;int packets=0;
   parser.OnDemuxerFinished([&](const uint8_t*data,int rate,size_t size){
    assert(rate==24000&&size==(continued?510U:255U));
    for(size_t i=0;i<size;++i){assert(data[i]==42);}
    ++packets;
   });
   for(size_t i=0;i<stream.size();i+=chunk){
    auto size=std::min(chunk,stream.size()-i);assert(parser.Process(stream.data()+i,size)==size);
   }
   assert(packets==1);
  }
 }
}
''', sources=(AUDIO / 'demuxer/ogg_demuxer.cc',))

    def test_custom_message_serializations_are_freed_before_scheduled_use(self):
        source = (ROOT / 'main/application.cc').read_text()
        start = source.index('            auto payload =', source.index('strcmp(type->valuestring, "custom")'))
        end = source.index('\n#endif', start)
        body = source[start:end]
        self.run_cpp(r'''
#include <cassert>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "cJSON.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
int live=0;
void* Allocate(size_t n){void*p=std::malloc(n);if(p)++live;return p;}
void Free(void*p){if(p)--live;std::free(p);}
struct Display{std::string text;void SetChatMessage(const char*,const char*s){text=s;}};
struct Application{
 Display output;std::vector<std::function<void()>>work;
 void Schedule(std::function<void()>f){work.push_back(std::move(f));}
 void Custom(const cJSON*root){auto*display=&output;
''' + body + r'''
 }
};
int main(){
 cJSON_Hooks hooks{Allocate,Free};cJSON_InitHooks(&hooks);
 for(int i=0;i<100;++i){
  Application app;auto*root=cJSON_Parse("{\"payload\":{\"text\":\"hello\"}}");
  int before=live;app.Custom(root);assert(live==before);
  cJSON_Delete(root);assert(live==0);
  assert(app.work.size()==1);app.work.front()();assert(app.output.text=="{\"text\":\"hello\"}");
 }
 cJSON_InitHooks(nullptr);
}
''', use_json=True)

    def test_decoder_format_limits_and_resampler_lifecycle(self):
        source = (AUDIO / 'audio_service.cc').read_text()
        self.run_cpp(r'''
#include <cassert>
#include <mutex>
#include <vector>
#include <deque>
#include <functional>
#include <condition_variable>
#define ESP_LOGW(...) ((void)0)
#define TAG "test"
template<typename...Args>void TestLog(const char*,const char*,Args&&...){}
#define ESP_LOGE(...) TestLog(__VA_ARGS__)
#define ESP_LOGI(...) ((void)0)
#define AS_OPUS_GET_FRAME_DRU_ENUM(ms) ((ms)==5||(ms)==10||(ms)==20||(ms)==40||(ms)==60||(ms)==80||(ms)==100||(ms)==120?0:-1)
using esp_opus_dec_cfg_t=int;
using esp_ae_rate_cvt_cfg_t=int;
constexpr int ESP_AUDIO_MONO=1;
#define OPUS_DEC_CFG(rate,duration) ((rate)+(duration))
#define RATE_CVT_CFG(src,dst,channels,quality) ((src)+(dst)+(channels)+(quality))
int opens=0,closes=0,converters=0,releases=0,resets=0,converter_resets=0;
void esp_opus_dec_reset(void*){++resets;}
void esp_ae_rate_cvt_reset(void*){++converter_resets;}
bool fail_converter=false;
int esp_opus_dec_open(const int*,int,void**out){++opens;*out=&opens;return 0;}
void esp_opus_dec_close(void*){++closes;}
void esp_ae_rate_cvt_close(void*){++releases;}
int esp_ae_rate_cvt_open(const int*,void**out){
 ++converters;*out=fail_converter?nullptr:&converters;return fail_converter?-1:0;
}
struct Codec{int output_sample_rate(){return 24000;}};
struct AudioService{
 Codec codec;Codec*codec_=&codec;std::mutex decoder_mutex_;
 void*opus_decoder_=nullptr;void*output_resampler_=nullptr;
 int decoder_sample_rate_=0,decoder_duration_ms_=0,decoder_frame_size_=0;
 std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 unsigned playback_generation_=0;
 std::deque<int>timestamp_queue_,audio_decode_queue_,audio_playback_queue_,audio_testing_queue_;
 struct{std::function<void()>on_playback_drained;}callbacks_;
 bool MarkPlaybackDrainedLocked(){
  assert(timestamp_queue_.empty()&&audio_decode_queue_.empty()&&
         audio_playback_queue_.empty()&&audio_testing_queue_.empty());return true;
 }
 void ResetDecoder();
 bool SetDecodeSampleRate(int,int);
};
''' + queue_function(source, 'bool AudioService::SetDecodeSampleRate(')
            + queue_function(source, 'void AudioService::ResetDecoder(') + r'''
int main(){
 AudioService audio;
 for(int rate:{-1,0,1,44100,96000,2147483647}){
  assert(!audio.SetDecodeSampleRate(rate,60));
 }
 for(int duration:{-1,0,1,9,121,2147483647}){
  assert(!audio.SetDecodeSampleRate(24000,duration));
 }
 assert(opens==0&&converters==0);
 assert(audio.SetDecodeSampleRate(16000,60));assert(opens==1&&converters==1);
 assert(audio.SetDecodeSampleRate(16000,60));assert(opens==1);
 assert(audio.SetDecodeSampleRate(24000,60));assert(releases==1&&!audio.output_resampler_);
 assert(audio.decoder_frame_size_==1440);
 fail_converter=true;assert(!audio.SetDecodeSampleRate(48000,20));
 int attempted=opens;fail_converter=false;
 assert(audio.SetDecodeSampleRate(48000,20));assert(opens==attempted+1&&audio.output_resampler_);
 assert(audio.decoder_frame_size_==960&&closes==opens-1);
 audio.audio_decode_queue_.push_back(1);audio.audio_playback_queue_.push_back(2);
 audio.audio_testing_queue_.push_back(3);audio.timestamp_queue_.push_back(4);
 int notified=0;audio.callbacks_.on_playback_drained=[&]{++notified;};
 audio.ResetDecoder();assert(audio.playback_generation_==1&&notified==1);
 assert(resets==1&&converter_resets==1);
}
''')
