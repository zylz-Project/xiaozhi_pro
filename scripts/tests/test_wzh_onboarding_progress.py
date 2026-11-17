"""Chapter completion and interrupted onboarding progress invariants."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('g++'), 'g++ required')
class OnboardingProgressTest(unittest.TestCase):
    def test_completion_interruption_timeout_and_migration(self):
        source = r'''
#include "wzh_onboarding_progress.h"
#include <cassert>
int main() {
    using P = wzh::OnboardingProgress;
    P p;
    for (int i=0;i<5;++i) p.Finish(true);
    assert(p.flags==0 && p.step==5);
    p.Finish(true);
    assert(p.flags==P::kOfflineComplete && p.step==6 && !p.Done());
    for (int i=6;i<12;++i) p.Finish(true);
    assert(p.flags==P::kOfflineComplete && p.step==12);
    p.Interrupted();p.Interrupted();
    assert(p.attempts==2 && p.step==12 && !p.Done());
    p.Interrupted();
    // Three failures skip the step for this run, but it is retried after reboot.
    assert(p.attempts==0 && p.step==13 && !p.Done());
    assert(p.skipped==(1U<<12));
    p.Resume();assert(p.step==12);
    p.Finish(true);assert(p.Done()&&p.skipped==0);
    P q;
    q.Finish(true);q.Finish(false);
    for(int i=2;i<6;++i)q.Finish(true);
    assert(!(q.flags&P::kOfflineComplete));
    q.Resume();assert(q.step==1);
    q.Finish(true);
    assert(q.step==6&&(q.flags&P::kOfflineComplete));
    P old=P::MigratedComplete();old.Resume();assert(old.Done()&&old.step==13);
    // A step that never got its interaction times out but still does not count,
    // so its chapter cannot be marked complete on that run.
    P timeout;timeout.Finish(true);timeout.Finish(true);timeout.Finish(false);
    for(int i=3;i<6;++i)timeout.Finish(true);
    assert(timeout.step==6&&!(timeout.flags&P::kOfflineComplete));
    // The next boot replays only the missed step, then the chapter can finish.
    timeout.Resume();assert(timeout.step==2);
    timeout.Finish(true);assert(timeout.step==6&&(timeout.flags&P::kOfflineComplete));
    // Three interruptions advance the cursor without ever marking the step.
    P stuck;stuck.Interrupted();stuck.Interrupted();
    assert(stuck.step==0&&stuck.attempts==2);
    stuck.Interrupted();assert(stuck.step==1&&stuck.attempts==0);
    assert(!(stuck.flags&P::kOfflineComplete));
    stuck.Resume();assert(stuck.step==0);
    stuck.Finish(true);assert(stuck.skipped==0);
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
