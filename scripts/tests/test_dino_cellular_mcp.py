"""Exercise the cellular MCP handoff out of the modem receive callback."""
from pathlib import Path
import unittest
from test_wzh_action_queue import queue_function
import test_wzh_motion_output as output

ROOT = Path(__file__).resolve().parents[2]

class CellularMcpTest(unittest.TestCase):
    def test_bounded_handoff_preserves_order_and_protocol_generation(self):
        source = (ROOT / 'main/application.cc').read_text()
        payload_pos = source.index('auto payload = cJSON_GetObjectItem(root, "payload");')
        branch_start = source.rfind('if (strcmp(type->valuestring, "mcp") == 0)', 0, payload_pos)
        branch = queue_function(source[branch_start:], 'if (strcmp(type->valuestring, "mcp") == 0)')
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
struct cJSON{const char* valuestring;};
cJSON payload{"message"};
cJSON* cJSON_GetObjectItem(cJSON*,const char*){return &payload;}
bool cJSON_IsObject(cJSON* p){return p!=nullptr;}
char* cJSON_PrintUnformatted(cJSON* p){return ::strdup(p->valuestring);}
void cJSON_free(void* p){std::free(p);}
struct Board{bool cellular=true;static Board& GetInstance(){static Board b;return b;}
 bool UsesCellularNetwork(){return cellular;}};
struct McpServer{std::vector<std::string> received;
 static McpServer& GetInstance(){static McpServer s;return s;}
 void ParseMessage(const std::string& s){received.push_back(s);}
 void ParseMessage(cJSON* p){received.push_back(p->valuestring);}};
struct App{std::atomic<unsigned> cellular_pending_mcp_{0};std::atomic<unsigned> protocol_generation_{1};
 std::vector<std::function<void()>> scheduled;
 void Schedule(std::function<void()> f){scheduled.push_back(std::move(f));}
 void Handle(cJSON* root,cJSON* type){auto protocol_generation=protocol_generation_.load();
''' + branch + r'''
 }
 void Run(){auto tasks=std::move(scheduled);scheduled.clear();for(auto& f:tasks)f();}
};
int main(){App a;cJSON root{""},type{"mcp"};auto& server=McpServer::GetInstance();
 for(int i=0;i<17;i++)a.Handle(&root,&type);
 assert(a.scheduled.size()==16&&a.cellular_pending_mcp_==16&&server.received.empty());
 a.Run();assert(server.received.size()==16&&a.cellular_pending_mcp_==0);
 a.Handle(&root,&type);a.protocol_generation_++;a.Run();assert(server.received.size()==16);
 Board::GetInstance().cellular=false;a.Handle(&root,&type);assert(server.received.size()==17&&a.scheduled.empty());
}
''')
