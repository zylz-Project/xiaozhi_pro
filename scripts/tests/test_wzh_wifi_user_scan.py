import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class WifiUserScanTest(unittest.TestCase):
    def test_scan_result_ownership_timeout_and_retry(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'project_components/esp-wifi-connect/wifi_station.cc').read_text()
        scan = queue_function(source, 'esp_err_t WifiStation::ScanNetworks(')
        complete = queue_function(source, 'void WifiStation::CompleteUserScan(')
        self.assertIn('this_->CompleteUserScan();', source)
        program = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=1, ESP_ERR_INVALID_STATE=2, ESP_ERR_TIMEOUT=3;
constexpr int WIFI_EVENT_USER_SCAN_DONE=8, WIFI_EVENT_STOPPED=2;
constexpr int pdFALSE=0;
#define pdMS_TO_TICKS(v) (v)
using EventBits_t = int;
struct wifi_ap_record_t { int id; };
struct Events { std::mutex mutex; std::condition_variable cv; int bits=0; };
void xEventGroupClearBits(Events* e,int bits) { std::lock_guard<std::mutex> lock(e->mutex);e->bits &= ~bits; }
void xEventGroupSetBits(Events* e,int bits) { {std::lock_guard<std::mutex> lock(e->mutex);e->bits |= bits;} e->cv.notify_all(); }
int xEventGroupWaitBits(Events* e,int bits,int,int,int ms) {
 std::unique_lock<std::mutex> lock(e->mutex);
 e->cv.wait_for(lock,std::chrono::milliseconds(ms),[&]{return (e->bits & bits)!=0;});
 return e->bits & bits;
}
std::atomic<int> started{0}; int clear_count=0; int raw_count=40;
int esp_wifi_scan_start(void*,bool) { ++started; return ESP_OK; }
int esp_wifi_scan_get_ap_num(uint16_t* n) { *n=raw_count;return ESP_OK; }
int esp_wifi_scan_get_ap_records(uint16_t* n,wifi_ap_record_t* out) {
 for(int i=0;i<*n;i++) out[i].id=i;return ESP_OK;
}
void esp_wifi_clear_ap_list() { ++clear_count; }
struct WifiStation {
 Events events; Events* event_group_=&events;
 std::mutex user_scan_mutex_,user_scan_result_mutex_;
 std::atomic<bool> user_scan_pending_{false};
 std::vector<wifi_ap_record_t> user_scan_results_;
 esp_err_t user_scan_error_=ESP_FAIL;
 esp_err_t ScanNetworks(std::vector<wifi_ap_record_t>&,int);
 void CompleteUserScan();
};
''' + scan + '\n' + complete + r'''
int main() {
 WifiStation s;std::vector<wifi_ap_record_t> records;int result=-1;
 std::thread t([&]{result=s.ScanNetworks(records,1000);});
 while(!started.load()) std::this_thread::yield();
 std::vector<wifi_ap_record_t> second;
 assert(s.ScanNetworks(second,10)==ESP_ERR_INVALID_STATE);
 s.CompleteUserScan(); t.join();
 assert(result==ESP_OK && records.size()==32 && records[31].id==31);
 assert(s.ScanNetworks(records,5)==ESP_ERR_TIMEOUT);
 assert(s.user_scan_pending_.load());
 assert(s.ScanNetworks(second,5)==ESP_ERR_INVALID_STATE);
 raw_count=0;s.CompleteUserScan();
 assert(!s.user_scan_pending_.load() && clear_count==1);
 std::thread t2([&]{result=s.ScanNetworks(records,1000);});
 while(started.load()<3) std::this_thread::yield();
 raw_count=1;s.CompleteUserScan();t2.join();
 assert(result==ESP_OK && records.size()==1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'scan.cc'
            binary = Path(directory) / 'scan'
            cpp.write_text(program)
            subprocess.run(['g++', '-std=c++17', '-pthread', str(cpp), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
