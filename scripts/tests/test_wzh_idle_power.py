"""Exercise the real idle policy at boundaries and after interruptions."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('g++'), 'g++ required')
class IdlePowerTest(unittest.TestCase):
    def test_deadlines_interruption_and_timer_wrap(self):
        source = r'''
#include "wzh_idle_power.h"
#include <cassert>
int main() {
    wzh::IdlePower p;
    // Product decision 2026-11-12: 5 min sentence, 10 min power-off.
    assert(wzh::IdlePower::kMissMs == 300000U && wzh::IdlePower::kOffMs == 600000U);
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
    // The 10-minute deadline runs from the last interaction, not from the miss.
    assert(!p.OffDue(300200 + 599999) && p.OffDue(300200 + 600000));
    p.Interaction(0xfffffff0U);
    assert(p.MissDue(uint32_t(0xfffffff0U + 300000U)));
    assert(p.OffDue(uint32_t(0xfffffff0U + 600000U)));
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
