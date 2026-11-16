"""Exercise each board's real startup protection observer on the host."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / 'main/boards/common/wzh'


class StartupNoticeTest(unittest.TestCase):
    def test_all_boards_report_protection_and_clear_only_after_greeting(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        for board, stem in (('wzh_crawlPanda', 'panda'), ('wzh_dinosaur', 'dinosaur'),
                            ('wzh_tailRedPanda', 'panda')):
            source = (ROOT / 'main/boards' / board / f'{stem}_power.cc').read_text()
            block = queue_function(source, 'if (startup_protection_monitoring && now >= 5000)')
            program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include "wzh_protection.h"
#include "wzh_system_prompts.h"
constexpr int BATTERY_LOW_MV=3300;
bool greeting=false;
namespace wzh {
StartupProtectionNotice last=StartupProtectionNotice::kNone;
void ObserveStartupProtection(StartupProtectionNotice n){last=n;}
bool StartupGreetingFinished(){return greeting;}
}
struct Output{uint32_t mask=0;uint32_t faults()const{return mask;}};
struct Battery{bool is_ready=true,is_low=false;bool ready()const{return is_ready;}bool low()const{return is_low;}};
struct Power{
 std::atomic<Output*> servo_output_{nullptr};Battery battery_protection_;
 std::atomic<bool>battery_sample_valid_{false};std::atomic<int>battery_live_mv_{0};
 bool startup_protection_monitoring=true;
 void check(uint32_t now){__BLOCK__}
};
int main(){
 Output out;Power p;p.servo_output_=&out;
 p.check(4999);assert(wzh::last==wzh::StartupProtectionNotice::kNone);
 out.mask=wzh::kOverheat;p.check(5000);assert(wzh::last==wzh::StartupProtectionNotice::kOverheat);
 out.mask=wzh::kFalling;p.check(6000);assert(wzh::last==wzh::StartupProtectionNotice::kMotionBlocked);
 out.mask=wzh::kBatteryPending;p.check(7000);assert(wzh::last==wzh::StartupProtectionNotice::kCheckingBattery);
 out.mask=0;p.battery_protection_.is_low=true;p.check(15000);assert(wzh::last==wzh::StartupProtectionNotice::kLowBattery);
 p.battery_protection_.is_low=false;p.check(16000);assert(wzh::last==wzh::StartupProtectionNotice::kNone);
 assert(p.startup_protection_monitoring);greeting=true;p.check(16001);assert(!p.startup_protection_monitoring);
}
'''.replace('__BLOCK__', block)
            with self.subTest(board=board), tempfile.TemporaryDirectory() as directory:
                cpp = Path(directory) / 'test.cc'; cpp.write_text(program)
                exe = Path(directory) / 'test'
                result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                         '-I', str(COMMON), str(cpp), '-o', str(exe)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(exe)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
