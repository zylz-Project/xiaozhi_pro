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
#include <cassert>
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
struct Http {Reply reply;std::string content;explicit Http(Reply r):reply(r){assert(live==0);++live;}
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
 std::string ReadAll(){return reply.body;}void Close(){++closed;}};
struct Ota {std::unique_ptr<Http> SetupHttp(){if(factory_fails)return nullptr;assert(created<(int)replies.size());
 return std::make_unique<Http>(replies[created++]);}
 esp_err_t FetchVersionResponse(const std::string&,const std::string&,cJSON*&);};
''' + method + r'''
void reset(bool cellular,std::vector<Reply> list){auto& b=Board::GetInstance();b.cellular=cellular;
 b.available=true;disconnect_on_delay=false;factory_fails=false;replies=std::move(list);
 created=closed=close_headers=0;delays.clear();}
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
