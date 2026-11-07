"""A delayed MQTT goodbye must not close a newer audio channel."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class MqttGoodbyeGenerationTest(unittest.TestCase):
    def test_delayed_goodbye_does_not_close_reopened_session(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/protocols/mqtt_protocol.cc').read_text()
        start = source.index('            auto session_id = cJSON_GetObjectItem(root, "session_id");')
        end = source.index('        } else if (on_incoming_json_', start)
        branch = source[start:end]
        program = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void)0)
struct cJSON {const char* valuestring;};
cJSON* cJSON_GetObjectItem(cJSON* root,const char*){return root;}
bool cJSON_IsString(cJSON* p){return p&&p->valuestring;}
struct Application {
 std::vector<std::function<void()>> tasks;
 static Application& GetInstance(){static Application app;return app;}
 void Schedule(std::function<void()> f){tasks.push_back(f);}
 void Drain(){auto work=std::move(tasks);tasks.clear();for(auto& f:work)f();}
};
struct MqttProtocol {
 std::string session_id_="old";std::shared_ptr<std::atomic<bool>> alive_=std::make_shared<std::atomic<bool>>(true);
 std::atomic<unsigned> audio_channel_generation_{0};int closes=0;
 void CloseAudioChannel(bool send){assert(!send);++closes;}
 void Goodbye(cJSON* root){
''' + branch + r'''
 }
};
int main(){
 MqttProtocol p;cJSON msg{"old"};p.Goodbye(&msg);
 p.audio_channel_generation_.fetch_add(1);p.session_id_="new";
 Application::GetInstance().Drain();assert(p.closes==0);
 cJSON current{"new"};p.Goodbye(&current);Application::GetInstance().Drain();assert(p.closes==1);
 p.Goodbye(&current);*p.alive_=false;Application::GetInstance().Drain();assert(p.closes==1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.cc'; path.write_text(program)
            exe = path.with_suffix('')
            result = subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(exe)],check=True,timeout=10)
