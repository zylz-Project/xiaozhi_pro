"""Exercise the real idle policy at boundaries and after interruptions."""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('g++'), 'g++ required')
class IdlePowerTest(unittest.TestCase):
    def test_all_product_variants_enable_the_countdown(self):
        for board in ('wzh_dinosaur', 'wzh_crawlPanda', 'wzh_tailRedPanda'):
            config = json.loads((ROOT / 'main/boards' / board / 'config.json').read_text())
            for variant in config['builds']:
                with self.subTest(board=board, variant=variant['name']):
                    for key in ('POWER_ENABLE', 'AUTO_OFF', 'MISS_PROMPT'):
                        self.assertIn(f'CONFIG_WZH_IDLE_{key}=y', variant['sdkconfig_append'])

    def test_deadlines_interruption_and_timer_wrap(self):
        source = r'''
#include "wzh_idle_power.h"
#include <cassert>
int main() {
    wzh::IdlePower p;
    // Five minutes for the reminder; 7 minutes 30 seconds for power-off.
    assert(wzh::IdlePower::kMissMs == 300000U && wzh::IdlePower::kOffMs == 450000U);
    assert(!p.MissDue(300000) && !p.OffDue(600000));
    p.Interaction(100);
    assert(!p.MissDue(300099) && p.MissDue(300100));
    auto old = p.BeginMiss();
    assert(!p.MissDue(300101) && !p.Quiet());
    p.Interaction(300200);
    p.FinishMiss(old, true);
    assert(!p.Quiet() && !p.OffDue(600100));
    assert(p.MissDue(600200));
    auto current = p.BeginMiss();
    p.FinishMiss(current, false);
    assert(!p.Quiet() && p.MissDue(600200));
    current = p.BeginMiss();
    p.FinishMiss(current, true);
    assert(p.Quiet() && !p.MissDue(900000));
    // The power-off deadline runs from the last interaction, not the reminder.
    assert(!p.OffDue(300200 + 449999) && p.OffDue(300200 + 450000));
    p.Interaction(0xfffffff0U);
    assert(p.MissDue(uint32_t(0xfffffff0U + 300000U)));
    assert(p.OffDue(uint32_t(0xfffffff0U + 450000U)));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'test.cc'
            binary = Path(directory) / 'test'
            cpp.write_text(source)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/boards/common/wzh'), str(cpp),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
