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
    for (const char* text : {"复刻音色", "我想复刻音色！", "帮我复刻音色!", "  复刻音色。  ",
                             "　我想复刻音色？", "复克音色。", "帮我复克一下声音", "帮我克隆一下我的声音", "可以模仿妈妈的嗓音吗"})
        assert(IsVoiceEnrollmentRequest(text));
    for (const char* text : {"", "不要复刻音色", "别复克音色", "不复克声音", "复克过声音", "听说有人复刻音色", "什么是声音复刻", "复刻声音成功了吗", "我已经复刻过声音", "\"复刻音色\"", "声音小一点"})
        assert(!IsVoiceEnrollmentRequest(text));
    for (const char* text : {"切换音色", "帮我换个声音", "更换一下嗓音", "用妈妈的声音说话", "换成别的声音", "切换已经复刻的声音"})
        assert(IsVoiceSelectionRequest(text));
    for (const char* text : {"不要换声音", "怎么切换音色", "把声音调大", "帮我克隆声音", "我喜欢你的声音", "这个声音好用吗", "别换声音"})
        assert(!IsVoiceSelectionRequest(text));
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

    def test_local_command_intercepts_even_when_configuration_is_missing(self):
        source = (ROOT / 'main/application.cc').read_text()
        stt = source.split('} else if (strcmp(type->valuestring, "stt") == 0)', 1)[1].split(
            '} else if (strcmp(type->valuestring, "llm") == 0)', 1)[0]
        self.assertLess(stt.index('ESP_LOGI(TAG, ">> %s"'), stt.index('IsVoiceEnrollmentRequest'))
        self.assertLess(stt.index('enrollment->IsConfigured()'), stt.index('voice_enrollment_active_.exchange'))
        interception = stt.split('if (enrollment &&', 1)[1].split('Schedule(', 1)[0]
        self.assertNotIn('voice_operation_configured &&', interception)
        self.assertIn('voice_selection_reply_guard_.store(true)', interception)
        self.assertIn('if (!voice_operation_configured)', stt)
        self.assertIn('ResumeAfterVoiceEnrollment(token)', stt)
        self.assertIn('Local voice operation: %s', stt)

if __name__ == '__main__':
    unittest.main()
