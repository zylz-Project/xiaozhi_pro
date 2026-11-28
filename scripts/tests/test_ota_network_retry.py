"""Exercise production OTA request retries with controlled network failures."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class OtaNetworkRetryTest(unittest.TestCase):
    def compile_run(self, program, *, json_parser=False):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cc').write_text(program)
            json_args = []
            if json_parser:
                cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
                if not (cjson / 'cJSON.c').exists() or not shutil.which('gcc'):
                    self.skipTest('gcc and managed cJSON required')
                obj = path / 'cJSON.o'
                subprocess.run(['gcc', '-std=c99', '-c', str(cjson / 'cJSON.c'),
                                '-o', str(obj)], check=True)
                json_args = ['-I', str(cjson), str(obj)]
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(ROOT / 'scripts/tests/support'),
                                     *json_args,
                                     str(path / 'test.cc'), '-o', str(path / 'test')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / 'test')], check=True, timeout=5)

    def test_cellular_retry_is_bounded_and_checks_complete_json(self):
        source = (ROOT / 'main/ota.cc').read_text()
        method = queue_function(source, 'esp_err_t Ota::FetchVersionResponse(')
        program = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include "cJSON.h"
#include "network_error.h"
using esp_err_t=int;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_RESPONSE 264
#define TAG "Ota"
template<class... T> void log_stub(T...){}
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define pdMS_TO_TICKS(x) (x)
struct Board {bool cellular=false,available=true;static Board& GetInstance(){static Board b;return b;}
 bool UsesCellularNetwork(){return cellular;}bool IsNetworkAvailable(){return available;}};
std::vector<int> delays;bool disconnect_on_delay=false;
void vTaskDelay(int ms){delays.push_back(ms);if(disconnect_on_delay)Board::GetInstance().available=false;}
struct Reply {bool opens;int status;int error;std::string body=R"({"mqtt":{}})";size_t length=0;};
std::vector<Reply> replies;int created=0,live=0,closed=0,close_headers=0;bool factory_fails=false;
size_t bytes_read=0;
struct Http {Reply reply;std::string content;size_t offset=0;explicit Http(Reply r):reply(r){assert(live==0);++live;}
 ~Http(){--live;} void SetContent(std::string&& value){content=std::move(value);}
 void SetHeader(const std::string& name,const std::string& value){
  assert(name=="Connection"&&value=="close");++close_headers;}
 NetworkResult<> Open(const std::string& method,const std::string& url){assert(url=="https://ota");
  assert(method==(content.empty()?"GET":"POST"));
  if(!reply.opens)return std::unexpected(NetworkError::ConnectFailed(reply.error));
  return {};}
 NetworkResult<int> GetStatusCode(){if(reply.status<0)return std::unexpected(NetworkError::Timeout());
  return reply.status;}
 size_t GetBodyLength(){return reply.length;}
 NetworkResult<int> Read(char* dest,size_t size){
  const auto count=std::min({size,reply.body.size()-offset,size_t{137}});
  std::memcpy(dest,reply.body.data()+offset,count);
  offset+=count;bytes_read+=count;return static_cast<int>(count);}
 void Close(){++closed;}};
struct Ota {std::unique_ptr<Http> SetupHttp(){if(factory_fails)return nullptr;assert(created<(int)replies.size());
 return std::make_unique<Http>(replies[created++]);}
 esp_err_t FetchVersionResponse(const std::string&,const std::string&,cJSON*&);};
''' + method + r'''
void reset(bool cellular,std::vector<Reply> list){auto& b=Board::GetInstance();b.cellular=cellular;
 b.available=true;disconnect_on_delay=false;factory_fails=false;replies=std::move(list);
 created=closed=close_headers=0;bytes_read=0;delays.clear();}
int main(){Ota ota;cJSON* response=nullptr;
 reset(true,{});factory_fails=true;assert(ota.FetchVersionResponse("https://ota","",response)==ESP_FAIL);
 reset(true,{{true,-1,0},{true,200,0}});
 assert(ota.FetchVersionResponse("https://ota","payload",response)==0);
 assert(cJSON_IsObject(cJSON_GetObjectItem(response,"mqtt")));
 cJSON_Delete(response);response=nullptr;
 assert(created==2&&closed==1&&close_headers==2&&delays==std::vector<int>{1500}&&live==0);
 reset(true,{{true,200,0,"{}",469},{true,200,0,"[1,2]",5},{true,200,0,"{\"mqtt\":{}} \n",0}});
 assert(ota.FetchVersionResponse("https://ota","",response)==ESP_OK);
 assert(cJSON_IsObject(cJSON_GetObjectItem(response,"mqtt")));
 cJSON_Delete(response);response=nullptr;
 assert(created==3&&closed==3&&(delays==std::vector<int>{1500,3000})&&live==0);
 reset(true,{{true,200,0,"",469},{true,200,0,"{}junk",6},{true,200,0,std::string("{}\0junk",7),7}});
 assert(ota.FetchVersionResponse("https://ota","",response)==ESP_ERR_INVALID_RESPONSE);
 assert(!response&&created==3&&closed==3&&(delays==std::vector<int>{1500,3000})&&live==0);
 const std::string max_body="{}"+std::string(16*1024-2,' ');
 reset(false,{{true,200,0,max_body,max_body.size()}});
 assert(ota.FetchVersionResponse("https://ota","",response)==ESP_OK);
 cJSON_Delete(response);response=nullptr;
 reset(false,{{true,200,0,max_body+" ",0}});
 assert(ota.FetchVersionResponse("https://ota","",response)==ESP_ERR_INVALID_RESPONSE);
 assert(!response&&bytes_read==max_body.size()+1);
 reset(false,{{true,200,0,"{}",max_body.size()+1}});
 assert(ota.FetchVersionResponse("https://ota","",response)==ESP_ERR_INVALID_RESPONSE);
 assert(!response&&bytes_read==0);
 reset(true,{{false,0,0},{true,-1,0},{true,-1,0}});
 assert(ota.FetchVersionResponse("https://ota","",response)==-1);
 assert(created==3&&(delays==std::vector<int>{1500,3000})&&live==0);
 reset(false,{{true,-1,0}});
 assert(ota.FetchVersionResponse("https://ota","",response)==-1&&created==1&&delays.empty()&&close_headers==0);
 reset(true,{{true,503,0}});
 assert(ota.FetchVersionResponse("https://ota","",response)==503&&created==1&&delays.empty());
 reset(true,{{true,-1,0}});disconnect_on_delay=true;
 assert(ota.FetchVersionResponse("https://ota","",response)==-1&&created==1&&delays.size()==1);
}
'''
        self.compile_run(program, json_parser=True)

    def test_protocol_settings_filter_invalid_values_and_handle_nvs_errors(self):
        source = (ROOT / 'main/ota.cc').read_text()
        methods = '\n'.join(queue_function(source, signature) for signature in (
            'bool IsUsableProtocolAddress(', 'bool StoreProtocolSettings(',
        ))
        self.compile_run(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "cJSON.h"
using esp_err_t=int;
using nvs_handle_t=unsigned;
constexpr int ESP_OK=0,NVS_READWRITE=1,NVS_KEY_NAME_MAX_SIZE=16;
#define ESP_LOGW(...) ((void)0)
int opens=0,commits=0,closes=0;
esp_err_t open_result=ESP_OK,write_result=ESP_OK,commit_result=ESP_OK;
std::vector<std::pair<std::string,std::string>> strings;
std::vector<std::pair<std::string,int32_t>> integers;
std::map<std::pair<nvs_handle_t,std::string>,std::string> stored;
esp_err_t nvs_open(const char* name,int mode,nvs_handle_t* handle){
 assert(std::string(name)=="mqtt"||std::string(name)=="websocket");
 assert(mode==NVS_READWRITE);++opens;*handle=std::string(name)=="mqtt" ? 1 : 2;return open_result;
}
esp_err_t nvs_set_str(nvs_handle_t handle,const char* key,const char* value){
 strings.emplace_back(key,value);
 if(write_result==ESP_OK)stored[{handle,key}]=value;
 return write_result;
}
esp_err_t nvs_set_i32(nvs_handle_t,const char* key,int32_t value){
 integers.emplace_back(key,value);return write_result;
}
esp_err_t nvs_get_str(nvs_handle_t handle,const char* key,char* dest,size_t* length){
 auto it=stored.find({handle,key});
 if(it==stored.end())return -1;
 if(dest){assert(*length>=it->second.size()+1);std::strcpy(dest,it->second.c_str());}
 *length=it->second.size()+1;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t){++commits;return commit_result;}
void nvs_close(nvs_handle_t){++closes;}
''' + methods + r'''
int main(){
 assert(!StoreProtocolSettings("mqtt","endpoint",nullptr)&&opens==0);
 auto* config=cJSON_Parse(R"({
   "endpoint":"mqtt.test:8883", "":"bad", "1234567890123456":"bad", "future_int":42, "fraction":1.25,
   "max":2147483647, "min":-2147483648, "too_high":2147483648, "too_low":-2147483649,
   "flag":true, "list":[], "negative":-3
 })");
 assert(config);
 const std::string maximum(3999,'x');
 cJSON_AddStringToObject(config,"123456789012345",maximum.c_str());
 cJSON_AddStringToObject(config,"too_long",(maximum+"x").c_str());
 assert(StoreProtocolSettings("mqtt","endpoint",config));
 assert(opens==1&&commits==1&&closes==1);
 assert(strings.size()==2&&strings[1].first=="123456789012345"&&strings[1].second==maximum);
 assert((integers==std::vector<std::pair<std::string,int32_t>>{
     {"future_int",42},{"max",INT32_MAX},{"min",INT32_MIN},{"negative",-3}}));
 cJSON_Delete(config);
 config=cJSON_Parse(R"({"url":"wss://test","version":3})");
 open_result=1;
 assert(!StoreProtocolSettings("websocket","url",config)&&opens==2&&commits==1&&closes==1);
 open_result=ESP_OK;
 write_result=2;
 assert(!StoreProtocolSettings("websocket","url",config)&&opens==3&&commits==1&&closes==2);
 assert(integers.size()==4);
 write_result=ESP_OK;
 commit_result=3;
 assert(!StoreProtocolSettings("websocket","url",config)&&opens==4&&commits==2&&closes==3);
 commit_result=ESP_OK;
 assert(StoreProtocolSettings("websocket","url",config));
 cJSON_Delete(config);
 config=cJSON_Parse("{}");
 assert(!StoreProtocolSettings("mqtt","endpoint",config));
 cJSON_Delete(config);
 for(const auto* partial:{R"({"username":"new"})",R"({"endpoint":3,"keepalive":90})",
                          R"({"endpoint":"","keepalive":90})",R"({"endpoint":":8883","keepalive":90})"}){
  config=cJSON_Parse(partial);
  assert(StoreProtocolSettings("mqtt","endpoint",config));
  assert((stored[{1,"endpoint"}]=="mqtt.test:8883"));
  cJSON_Delete(config);
 }
 stored.erase({1,"endpoint"});
 config=cJSON_Parse(R"({"endpoint":"mqtt://bad","keepalive":90})");
 assert(!StoreProtocolSettings("mqtt","endpoint",config));
 assert(stored.count({1,"endpoint"})==0);
 cJSON_Delete(config);
}
''', json_parser=True)

    def test_server_time_rejects_invalid_values_and_checks_system_call(self):
        source = (ROOT / 'main/ota.cc').read_text()
        method = queue_function(source, 'bool ApplyServerTime(')
        self.compile_run(r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <sys/time.h>
#include "cJSON.h"
timeval applied{};
int calls=0,system_result=0;
int test_settimeofday(const timeval* time, const void*){applied=*time;++calls;return system_result;}
#define settimeofday test_settimeofday
''' + method + r'''
bool Apply(const char* input){
 auto* root=cJSON_Parse(input);
 assert(root);
 const bool success=ApplyServerTime(root);
 cJSON_Delete(root);
 return success;
}
int main(){
 for(const char* bad:{"[]","{}",R"({"timestamp":-1})",R"({"timestamp":1e309})",
                      R"({"timestamp":1700000000123,"timezone_offset":2147483647})",
                      R"({"timestamp":1700000000123,"timezone_offset":1.5})",
                      R"({"timestamp":15,"timezone_offset":-1})",
                      R"({"timestamp":253402300799999,"timezone_offset":1})"}){
  assert(!Apply(bad)&&calls==0);
 }
 assert(Apply(R"({"timestamp":1700000000123,"timezone_offset":480})"));
 assert(calls==1&&applied.tv_sec==1700028800&&applied.tv_usec==123000);
 system_result=-1;
 assert(!Apply(R"({"timestamp":1700000000456})"));
 assert(calls==2&&applied.tv_sec==1700000000&&applied.tv_usec==456000);
}
''', json_parser=True)

    def test_firmware_short_reads_and_truncated_streams_preserve_ota_state(self):
        method = queue_function((ROOT / 'main/ota.cc').read_text(), 'bool Ota::Upgrade(')
        self.compile_run(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include "network_error.h"
#define TAG "Ota"
template<class... T> void log_stub(T...){}
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
constexpr int ESP_OK=0, ESP_ERR_OTA_VALIDATE_FAILED=1, OTA_WITH_SEQUENTIAL_WRITES=2;
constexpr int MALLOC_CAP_INTERNAL=0, ESP_IMAGE_HEADER_MAGIC=0xe9;
struct esp_image_header_t { char bytes[24]; };
struct esp_image_segment_header_t { char bytes[8]; };
struct esp_app_desc_t { char bytes[256]; };
using esp_err_t=int;
using esp_ota_handle_t=unsigned;
struct Partition { unsigned long address=0x10000; const char* label="ota_1"; size_t size=8192; } partition;
const Partition* esp_ota_get_next_update_partition(const void*){return &partition;}
int begins=0,aborts=0,ends=0,boots=0;
bool live=false,read_error=false;
std::string body,flashed;
size_t advertised=0,chunk_size=17,limit=0;
esp_err_t esp_ota_begin(const Partition*,int,esp_ota_handle_t* handle){
 assert(!live);++begins;live=true;*handle=1;return ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle){assert(handle==1&&live);live=false;++aborts;return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t handle,const char* data,size_t size){
 assert(handle==1&&live);flashed.append(data,size);return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle){assert(handle==1&&live);live=false;++ends;return ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const Partition*){++boots;return ESP_OK;}
const char* esp_err_to_name(int){return "error";}
int esp_timer_get_time(){return 0;}
void* heap_caps_malloc(size_t size,int){return std::malloc(size);}
void heap_caps_free(void* ptr){std::free(ptr);}
struct Http {
 size_t offset=0;
 NetworkResult<> Open(const std::string&,const std::string&){return {};}
 NetworkResult<int> GetStatusCode(){return 200;}
 size_t GetBodyLength(){return advertised;}
 NetworkResult<int> Read(char* dest,size_t requested){
  if(offset==limit&&read_error)return std::unexpected(NetworkError::Timeout());
  const auto count=std::min({requested,chunk_size,limit-offset});
  std::memcpy(dest,body.data()+offset,count);offset+=count;return static_cast<int>(count);
 }
 void Close(){}
};
struct Network {std::unique_ptr<Http> CreateHttp(int){return std::make_unique<Http>();}};
struct Board {
 Network network;
 static Board& GetInstance(){static Board board;return board;}
 Network* GetNetwork(){return &network;}
};
struct Ota {static bool Upgrade(const std::string&,std::function<void(int,size_t)>);};
''' + method + r'''
void reset(size_t size,size_t available){
 advertised=size;limit=available;read_error=false;flashed.clear();
 begins=aborts=ends=boots=0;assert(!live);
}
int main(){
 body.assign(5200,'\x35');body[0]=static_cast<char>(ESP_IMAGE_HEADER_MAGIC);
 reset(body.size(),body.size());
 unsigned percent=0;
 assert(Ota::Upgrade("firmware",[&](int progress,size_t){percent=progress;}));
 assert(flashed==body&&begins==1&&ends==1&&boots==1&&percent==100);
 reset(body.size(),150);assert(!Ota::Upgrade("firmware",{}));
 assert(begins==0&&ends==0&&boots==0);
 for(bool error:{false,true}){
  reset(body.size(),4400);read_error=error;assert(!Ota::Upgrade("firmware",{}));
  assert(flashed.size()==4096&&begins==1&&aborts==1&&ends==0&&boots==0);
 }
 reset(partition.size+1,0);assert(!Ota::Upgrade("firmware",{}));assert(begins==0);
 reset(body.size(),body.size());body[0]=0;
 assert(!Ota::Upgrade("firmware",{}));assert(begins==0&&flashed.empty());
}
''')

    def test_assets_preserve_existing_mapping_until_data_arrives_and_commit_header_last(self):
        method = queue_function((ROOT / 'main/assets.cc').read_text(), 'bool Assets::Download(')
        self.compile_run(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "network_error.h"
#define TAG "Assets"
template<class... T> void log_stub(T...){}
#define ESP_LOGD(...) log_stub(__VA_ARGS__)
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
constexpr int ESP_OK=0,MALLOC_CAP_INTERNAL=0;
using esp_err_t=int;
struct Partition {unsigned long size=8192;} partition;
std::string image(50,'a');
size_t available=0;
std::vector<size_t> offsets;
int erases=0;
size_t esp_partition_get_main_flash_sector_size(){return 4096;}
esp_err_t esp_partition_erase_range(Partition*,size_t,size_t){++erases;return ESP_OK;}
esp_err_t esp_partition_write(Partition*,size_t offset,const void* data,size_t size){
 assert(std::memcmp(data,image.data()+offset,size)==0);offsets.push_back(offset);return ESP_OK;
}
const char* esp_err_to_name(int){return "error";}
int64_t esp_timer_get_time(){return 0;}
void* heap_caps_malloc(size_t size,int){return std::malloc(size);}
void heap_caps_free(void* ptr){std::free(ptr);}
struct Http {
 size_t offset=0;
 NetworkResult<> Open(const std::string&,const std::string&){return {};}
 NetworkResult<int> GetStatusCode(){return 200;}
 size_t GetBodyLength(){return image.size();}
 NetworkResult<int> Read(char* dest,size_t requested){
  const size_t size=std::min({requested,size_t{5},available-offset});
  std::memcpy(dest,image.data()+offset,size);offset+=size;return static_cast<int>(size);
 }
};
struct Network {std::unique_ptr<Http> CreateHttp(int){return std::make_unique<Http>();}};
struct Board {
 Network network;
 static Board& GetInstance(){static Board board;return board;}
 Network* GetNetwork(){return &network;}
};
struct Assets {
 Partition* partition_=&partition;
 int unmapped=0,initialized=0;
 void UnApplyPartition(){++unmapped;}
 bool InitializePartition(){++initialized;return true;}
 bool Download(std::string,std::function<void(int,size_t)>);
};
''' + method + r'''
int main(){
 Assets assets;
 available=10;assert(!assets.Download("assets",{}));
 assert(assets.unmapped==0&&erases==0&&offsets.empty());
 available=30;assert(!assets.Download("assets",{}));
 assert(assets.unmapped==1&&erases==1&&assets.initialized==0);
 assert(!offsets.empty()&&std::find(offsets.begin(),offsets.end(),0)==offsets.end());
 available=image.size();offsets.clear();assert(assets.Download("assets",{}));
 assert(assets.unmapped==2&&erases==2&&assets.initialized==1);
 assert(offsets.front()==12&&offsets.back()==0);
}
''')
