"""Exercise production OTA request retries with controlled network failures."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class OtaNetworkRetryTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_cellular_retry_is_bounded_and_wifi_is_unchanged(self):
        source = (ROOT / 'main/ota.cc').read_text()
        method = queue_function(source, 'esp_err_t Ota::FetchVersionResponse(')
        program = r'''
#include <cassert>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include "network_error.h"
using esp_err_t=int;
#define ESP_OK 0
#define ESP_FAIL -1
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
struct Reply {bool opens;int status;int error;};
std::vector<Reply> replies;int created=0,live=0,closed=0;bool factory_fails=false;
struct Http {Reply reply;std::string content;explicit Http(Reply r):reply(r){assert(live==0);++live;}
 ~Http(){--live;} void SetContent(std::string&& value){content=std::move(value);}
 NetworkResult<> Open(const std::string& method,const std::string& url){assert(url=="https://ota");
  assert(method==(content.empty()?"GET":"POST"));
  if(!reply.opens)return std::unexpected(NetworkError::ConnectFailed(reply.error));
  return {};}
 NetworkResult<int> GetStatusCode(){if(reply.status<0)return std::unexpected(NetworkError::Timeout());
  return reply.status;}
 std::string ReadAll(){return "response";}void Close(){++closed;}};
struct Ota {std::unique_ptr<Http> SetupHttp(){if(factory_fails)return nullptr;assert(created<(int)replies.size());
 return std::make_unique<Http>(replies[created++]);}
 esp_err_t FetchVersionResponse(const std::string&,const std::string&,std::string&);};
''' + method + r'''
void reset(bool cellular,std::vector<Reply> list){auto& b=Board::GetInstance();b.cellular=cellular;
 b.available=true;disconnect_on_delay=false;factory_fails=false;replies=std::move(list);created=closed=0;delays.clear();}
int main(){Ota ota;std::string response;
 reset(true,{});factory_fails=true;assert(ota.FetchVersionResponse("https://ota","",response)==ESP_FAIL);
 reset(true,{{true,-1,0},{true,200,0}});
 assert(ota.FetchVersionResponse("https://ota","payload",response)==0);
 assert(response=="response"&&created==2&&closed==1&&delays==std::vector<int>{1500}&&live==0);
 reset(true,{{false,0,0},{true,-1,0},{true,-1,0}});
 assert(ota.FetchVersionResponse("https://ota","",response)==-1);
 assert(created==3&&(delays==std::vector<int>{1500,3000})&&live==0);
 reset(false,{{true,-1,0}});
 assert(ota.FetchVersionResponse("https://ota","",response)==-1&&created==1&&delays.empty());
 reset(true,{{true,503,0}});
 assert(ota.FetchVersionResponse("https://ota","",response)==503&&created==1&&delays.empty());
 reset(true,{{true,-1,0}});disconnect_on_delay=true;
 assert(ota.FetchVersionResponse("https://ota","",response)==-1&&created==1&&delays.size()==1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cc').write_text(program)
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(ROOT / 'scripts/tests/support'),
                                     str(path / 'test.cc'), '-o', str(path / 'test')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / 'test')], check=True, timeout=5)
