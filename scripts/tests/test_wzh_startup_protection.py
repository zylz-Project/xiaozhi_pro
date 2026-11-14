"""Run each product's startup protection reporting branch with fake hardware."""
from pathlib import Path
import unittest
import test_wzh_motion_output as output

ROOT = Path(__file__).resolve().parents[2]


class StartupProtectionTest(unittest.TestCase):
    run_cpp = output.WzhMotionOutputTest.run_cpp

    def test_three_product_power_paths_classify_and_clear_startup_faults(self):
        for board, stem in [('wzh_crawlPanda', 'panda'),
                            ('wzh_tailRedPanda', 'panda'),
                            ('wzh_dinosaur', 'dinosaur')]:
            source = (ROOT / 'main/boards' / board / (stem + '_power.cc')).read_text()
            start = source.index('        if (startup_protection_monitoring && now >= 5000)')
            end = source.index('        vTaskDelay(pdMS_TO_TICKS(kPollMs));', start)
            branch = source[start:end]
            with self.subTest(board=board):
                self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include "wzh_protection.h"
#include "wzh_system_prompts.h"
constexpr int BATTERY_LOW_MV=3300;
namespace wzh {
StartupProtectionNotice current=StartupProtectionNotice::kNone;
bool finished=false;
void ObserveStartupProtection(StartupProtectionNotice n){current=n;}
bool StartupGreetingFinished(){return finished;}
}
struct Output { unsigned value=0; unsigned faults()const{return value;} };
struct Protection {bool valid=false,low_value=false;bool ready()const{return valid;}bool low()const{return low_value;}};
struct Power {
 Output output;std::atomic<Output*> servo_output_{&output};
 std::atomic<bool> battery_sample_valid_{false};std::atomic<int> battery_live_mv_{0};
 Protection battery_protection_;
 bool startup_protection_monitoring=true;
 void Tick(unsigned now){
''' + branch + r'''
 }
};
int main(){using N=wzh::StartupProtectionNotice;Power p;
 p.output.value=wzh::kBatteryPending;p.Tick(4999);assert(wzh::current==N::kNone);
 p.Tick(5000);assert(wzh::current==N::kCheckingBattery);
 p.battery_sample_valid_=true;p.battery_live_mv_=3200;p.Tick(5020);assert(wzh::current==N::kCheckingBattery);
 p.output.value=wzh::kLowBattery;p.battery_protection_.valid=true;p.battery_protection_.low_value=true;
 p.Tick(14999);assert(wzh::current==N::kCheckingBattery);
 p.Tick(15000);assert(wzh::current==N::kLowBattery);
 p.battery_protection_.low_value=false;p.battery_live_mv_=3800;p.output.value=wzh::kOverheat;p.Tick(15020);assert(wzh::current==N::kOverheat);
 p.output.value=wzh::kFalling;p.Tick(15040);assert(wzh::current==N::kMotionBlocked);
 p.output.value=0;wzh::finished=true;p.Tick(15060);
 assert(wzh::current==N::kNone&&!p.startup_protection_monitoring);
}
''')


if __name__ == '__main__':
    unittest.main()
