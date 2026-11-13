import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class ActivationTaskRecoveryTest(unittest.TestCase):
    def test_failed_allocation_returns_idle_and_retries(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/application.cc').read_text()
        method = queue_function(source, 'void Application::HandleNetworkConnectedEvent()')
        program = r'''
#include <cassert>
#include <cstddef>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define CONFIG_BOARD_TYPE_WZH_CRAWLPANDA 1
#define CONFIG_BOARD_TYPE_WZH_DINOSAUR 0
#define CONFIG_BOARD_TYPE_WZH_TAILREDPANDA 0
using BaseType_t=int; constexpr int pdPASS=1;
enum DeviceState {kDeviceStateStarting,kDeviceStateWifiConfiguring,kDeviceStateIdle,kDeviceStateActivating};
struct Display { void UpdateStatusBar(bool) {} };
struct Board { Display d; static Board& GetInstance(){ static Board b;return b; } Display* GetDisplay(){return &d;} };
int create_result=0,create_count=0;
int xTaskCreate(void (*)(void*),const char*,int,void*,int,void** out) {
 ++create_count; if(create_result==pdPASS) *out=(void*)1;return create_result;
}
void vTaskDelete(void*) {}
struct Application {
 bool network_connected_=false,activation_retry_pending_=false;void* activation_task_handle_=nullptr;
 void* protocol_=nullptr;DeviceState state=kDeviceStateStarting;
 DeviceState GetDeviceState(){return state;} void SetDeviceState(DeviceState s){state=s;}
 void ActivationTask() {} void HandleNetworkConnectedEvent();
};
''' + method + r'''
int main() {
 Application a;
 a.HandleNetworkConnectedEvent();
 assert(a.network_connected_ && a.activation_retry_pending_ && a.state==kDeviceStateIdle);
 assert(a.activation_task_handle_==nullptr && create_count==1);
 create_result=pdPASS;a.HandleNetworkConnectedEvent();
 assert(!a.activation_retry_pending_ && a.state==kDeviceStateActivating);
 assert(a.activation_task_handle_!=nullptr && create_count==2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'activation.cc'
            binary = Path(directory) / 'activation'
            cpp.write_text(program)
            subprocess.run(['g++', '-std=c++17', str(cpp), '-o', str(binary)], check=True,
                           capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
