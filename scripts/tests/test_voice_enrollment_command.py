import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class EnrollmentCommandTest(unittest.TestCase):
    def test_exact_commands_with_asr_punctuation(self):
        code = r'''
#include "main/audio/voice_enrollment.h"
#include <cassert>
int main() {
    for (const char* text : {"复刻音色", "我想复刻音色！", "帮我复刻音色!", "  复刻音色。  ", "　我想复刻音色？"})
        assert(IsVoiceEnrollmentRequest(text));
    for (const char* text : {"", "不要复刻音色", "听说有人复刻音色", "复刻音色好吗", "\"复刻音色\""})
        assert(!IsVoiceEnrollmentRequest(text));
}
'''
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'test.cc'
            source.write_text(code)
            exe = Path(temp) / 'test'
            subprocess.run(['c++', '-std=c++17', '-I', str(ROOT), str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_all_products_provide_enrollment(self):
        for board in ('wzh_crawlPanda', 'wzh_tailRedPanda', 'wzh_dinosaur'):
            text = (ROOT / 'main/boards' / board / (board + '.cc')).read_text()
            self.assertIn('ProductVoiceEnrollment()', text)
        self.assertIn('IsVoiceEnrollmentRequest(recognized)', (ROOT / 'main/application.cc').read_text())

if __name__ == '__main__':
    unittest.main()
