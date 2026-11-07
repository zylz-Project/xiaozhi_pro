"""Verify the production abort path cancels a local voice preview."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function
ROOT=Path(__file__).resolve().parents[2]
class VoicePreviewAbortTest(unittest.TestCase):
    def test_abort_cancels_preview_and_does_not_send_during_reconnect(self):
        if not shutil.which('g++'): self.skipTest('g++ required')
        method=queue_function((ROOT/'main/application.cc').read_text(),'void Application::AbortSpeaking(')
        program=r'''
#include <atomic>
#include <cassert>
#include <memory>
#define ESP_LOGI(...) ((void)0)
enum AbortReason{kAbortReasonNone};
struct External {int cancels=0;void Cancel(){++cancels;}};
struct Board {External external;static Board& GetInstance(){static Board b;return b;}
 External* GetExternalTts(){return &external;}};
struct Protocol {int aborts=0;void SendAbortSpeaking(AbortReason){++aborts;}};
struct Application {
 std::atomic<bool> external_voice_preview_active_{false},external_tts_selected_{false},voice_enrollment_reconnecting_{false};
 std::atomic<int> external_voice_preview_status_{0},external_tts_turn_{0};
 std::shared_ptr<Protocol> protocol_=std::make_shared<Protocol>();bool aborted_=false;
 void AbortSpeaking(AbortReason);
};
''' + method + r'''
int main(){Application a;a.external_voice_preview_active_=true;a.external_tts_selected_=true;
 a.AbortSpeaking(kAbortReasonNone);assert(!a.external_voice_preview_active_&&a.external_voice_preview_status_==4);
 assert(Board::GetInstance().external.cancels==1&&a.protocol_->aborts==1&&a.aborted_);
 a.voice_enrollment_reconnecting_=true;a.AbortSpeaking(kAbortReasonNone);assert(a.protocol_->aborts==1);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'test.cc';p.write_text(program);exe=p.with_suffix('')
            result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(exe)],check=True,timeout=10)
