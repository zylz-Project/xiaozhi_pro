"""Host checks for the dinosaur ML307 test variant and its network lifecycle."""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DINO = ROOT / 'main/boards/wzh_dinosaur'


class DinosaurCellularTest(unittest.TestCase):
    def test_variant_is_opt_in_and_uses_existing_product(self):
        config = json.loads((DINO / 'config.json').read_text())
        variants = {b['name']: b for b in config['builds']}
        test = variants['wzh-dinosaur-ml307-test']['sdkconfig_append']
        self.assertFalse(any('PPP' in setting and setting.endswith('=y') for setting in test))
        for variant in variants.values():
            self.assertFalse(any(s.startswith('CONFIG_WZH_DINOSAUR_ML307=')
                                 for s in variant['sdkconfig_append']))
        self.assertEqual(config['type'], 'wzh-dinosaur')
        board_config = (DINO / 'config.h').read_text()
        self.assertRegex(board_config, r'#define DINOSAUR_ENABLE_4G [01]')
        self.assertNotIn('DINOSAUR_4G_USE_PPP', board_config)
        board_source = (DINO / 'wzh_dinosaur.cc').read_text()
        self.assertIn('return cellular_.GetNetwork();', board_source)
        kconfig = (ROOT / 'main/Kconfig.projbuild').read_text()
        self.assertIn('config WZH_DINOSAUR_ML307_DTR_GPIO', kconfig)
        self.assertNotIn('WZH_DINOSAUR_4G_PPP', kconfig)
        self.assertNotIn('WZH_DINOSAUR_PPP', kconfig)
        self.assertNotIn('wzh-dinosaur-ml307-ppp-test', variants)
        diagnostic = variants['wzh-dinosaur-ml307-at-diagnostic']['sdkconfig_append']
        self.assertIn('CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC=y', diagnostic)
        self.assertIn('CONFIG_ESP_MODEM_USE_PPP_MODE=n', diagnostic)
        self.assertIn('CONFIG_LWIP_PPP_SUPPORT=n', diagnostic)

    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_pins_retries_and_disconnect_reconnect(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            for name in ('driver', 'esp_adc', 'freertos'):
                (p / name).mkdir()
            (p / 'sdkconfig.h').write_text('#define CONFIG_WZH_DINOSAUR_ML307 1\n')
            (p / 'driver/gpio.h').write_text('''#pragma once
using gpio_num_t=int;
#define GPIO_NUM_NC -1
''' + '\n'.join(f'#define GPIO_NUM_{i} {i}' for i in range(49)) + '''
#define GPIO_IS_VALID_GPIO(n) ((n)>=0 && (n)<=48 && !((n)>=22 && (n)<=25))
#define GPIO_IS_VALID_OUTPUT_GPIO(n) (GPIO_IS_VALID_GPIO(n) && (n)!=46)
''')
            for name in ('driver/ledc.h', 'driver/spi_master.h', 'esp_adc/adc_oneshot.h'):
                (p / name).write_text('#pragma once\n')
            (p / 'freertos/FreeRTOS.h').write_text('#pragma once\n#define pdPASS 1\n#define pdMS_TO_TICKS(n) (n)\n')
            (p / 'freertos/task.h').write_text('''#pragma once
int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*);
void vTaskDelay(int); void vTaskDelete(void*);
''')
            (p / 'esp_log.h').write_text('''#pragma once
template<class... T> void log_stub(T...) {}
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
''')
            (p / 'board.h').write_text('''#pragma once
#include <functional>
#include <string>
enum class NetworkEvent {ModemDetecting,ModemErrorInitFailed,Connecting,Connected,Disconnected,ModemErrorNoSim,ModemErrorRegDenied};
using NetworkEventCallback=std::function<void(NetworkEvent,const std::string&)>;
struct NetworkInterface {virtual ~NetworkInterface()=default;};
''')
            (p / 'at_modem.h').write_text('''#pragma once
#include "board.h"
#include "driver/gpio.h"
#include <memory>
enum class NetworkStatus {Ready,ErrorInsertPin,ErrorRegistrationDenied,ErrorTimeout};
struct AtModem: NetworkInterface {
 static int detects,waits,tx,rx,baud;static AtModem* current;std::function<void(bool)> callback;
 static std::unique_ptr<AtModem> Detect(gpio_num_t tx_pin,gpio_num_t rx_pin,gpio_num_t,int rate,int) {
  tx=tx_pin;rx=rx_pin;baud=rate;
  if(++detects==1)return {};auto m=std::make_unique<AtModem>();current=m.get();return m;}
 void OnNetworkStateChanged(std::function<void(bool)> f){callback=f;}
 NetworkStatus WaitForNetworkReady(int) {++waits;return waits<=2?NetworkStatus::ErrorInsertPin:NetworkStatus::Ready;}
};
''')
            (p / 'test.cc').write_text('''
#include <cassert>
#include <vector>
#include "at_modem.h"
int AtModem::detects=0;int AtModem::waits=0;int AtModem::tx=-1;int AtModem::rx=-1;int AtModem::baud=0;AtModem* AtModem::current=nullptr;
void(*task)(void*)=nullptr;void* task_arg=nullptr;int online_delays=0,fast_retries=0;bool fail_task=false;
int xTaskCreate(void(*f)(void*),const char*,int,void* p,int,void*){if(fail_task)return 0;task=f;task_arg=p;return 1;}
struct Stop{};
void vTaskDelay(int ms){if(ms==1000&&!AtModem::current)++fast_retries;if(ms==1000&&AtModem::current&&AtModem::waits>=3){if(++online_delays==1)AtModem::current->callback(false);else throw Stop{};}}
void vTaskDelete(void*){}
#include "dinosaur_cellular.cc"
int main(){
 DinosaurCellular cell;std::vector<NetworkEvent> events;
 auto cb=[&](NetworkEvent e,const std::string&){events.push_back(e);};
#if DINOSAUR_ML307_TX_GPIO == 12 && DINOSAUR_ML307_RX_GPIO == 11 && CONFIG_WZH_DINOSAUR_ML307_DTR_GPIO == -1
 fail_task=true;assert(!cell.Start(cb)&&!task);fail_task=false;
 assert(cell.Start(cb));assert(task);assert(!cell.IsReady()&&cell.GetNetwork()==nullptr);
 assert(cell.Start(cb));try{task(task_arg);}catch(const Stop&){}
 assert(AtModem::detects==2&&AtModem::waits==4&&cell.IsReady()&&cell.GetNetwork()!=nullptr);
 assert(fast_retries==1);
 assert(AtModem::tx==12&&AtModem::rx==11&&AtModem::baud==921600);
 int connected=0,disconnected=0,nosim=0;
 for(auto e:events){connected+=e==NetworkEvent::Connected;disconnected+=e==NetworkEvent::Disconnected;nosim+=e==NetworkEvent::ModemErrorNoSim;}
 assert(connected==2&&disconnected==1&&nosim==1);
#else
 assert(!cell.Start(cb));assert(!task&&events.size()==1&&events[0]==NetworkEvent::ModemErrorInitFailed);
#endif
}
''')
            for tx, rx, dtr in [(-1, -1, -1), (11, 11, -1), (17, 11, -1), (12, 11, 11), (12, 11, -1)]:
                binary = p / 'test'
                cmd = ['g++', '-std=c++17', '-Wall', '-Wextra', '-I', str(p), '-I', str(DINO),
                       '-DDINOSAUR_ENABLE_4G=1',
                       f'-DDINOSAUR_ML307_TX_GPIO={tx}',
                       f'-DDINOSAUR_ML307_RX_GPIO={rx}',
                       f'-DCONFIG_WZH_DINOSAUR_ML307_DTR_GPIO={dtr}',
                       str(p / 'test.cc'), '-o', str(binary)]
                subprocess.run(cmd, check=True, capture_output=True, text=True)
                subprocess.run([str(binary)], check=True)
            # Wi-Fi mode must exclude the cellular implementation entirely.
            (p / 'wifi.cc').write_text('''
#include "dinosaur_cellular.cc"
#if DINOSAUR_ENABLE_4G != 0
#error Wi-Fi mode did not take effect
#endif
int main() { return 0; }
''')
            subprocess.run(['g++', '-std=c++17', '-I', str(p), '-I', str(DINO),
                            '-DDINOSAUR_ENABLE_4G=0', str(p / 'wifi.cc'),
                            '-o', str(p / 'wifi')], check=True, capture_output=True, text=True)
            subprocess.run([str(p / 'wifi')], check=True)
            (p / 'at_diagnostic.cc').write_text('''
#include "config.h"
#if DINOSAUR_ENABLE_4G != 1
#error AT diagnostic must select cellular
#endif
int main() { return 0; }
''')
            subprocess.run(['g++', '-std=c++17', '-I', str(p), '-I', str(DINO),
                            '-DCONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC=1',
                            str(p / 'at_diagnostic.cc'), '-o', str(p / 'at_diagnostic')],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(p / 'at_diagnostic')], check=True)


if __name__ == '__main__':
    unittest.main()
