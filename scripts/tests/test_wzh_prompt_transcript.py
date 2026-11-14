"""Keep logged tutorial transcripts aligned with each bundled voice pack."""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'main/boards/common/wzh/wzh_onboarding_transcript.h'


class PromptTranscriptTest(unittest.TestCase):
    def test_system_prompt_log_text_matches_both_audio_packs(self):
        source = ((ROOT / 'main/boards/common/wzh/wzh_system_prompts.cc').read_text() +
                  (ROOT / 'main/application.cc').read_text())
        for profile in ('panda', 'dinosaur'):
            pack = json.loads((ROOT / 'main/assets/wzh_system_voices' / profile / 'pack.json').read_text())
            for key in ('wificonfig', 'success', 'low_battery', 'power_off', 'upgrade', 'activation'):
                with self.subTest(profile=profile, key=key):
                    self.assertIn(pack['files'][key]['text'], source)

    def test_each_product_logged_text_matches_its_audio_manifest(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ is required')
        manifest = json.loads((ROOT / 'main/assets/wzh_onboarding_voices/manifest.json').read_text())
        keys = ('intro', 'touch', 'motion', 'cmd1', 'net_invite', 'wake', 'ap', 'page',
                'connected', 'ability', 'interrupt', 'clone', 'farewell', 'idle_miss')
        for product, config in (('an', 'CONFIG_BOARD_TYPE_WZH_CRAWLPANDA'),
                                ('xing', 'CONFIG_BOARD_TYPE_WZH_TAILREDPANDA'),
                                ('long', 'CONFIG_BOARD_TYPE_WZH_DINOSAUR')):
            with self.subTest(product=product), tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / 'test.cc'
                source.write_text('#include <iostream>\n#include "' + str(HEADER) + '"\n'
                                  'int main(){for(unsigned i=0;i<14;++i)'
                                  'std::cout<<wzh::OnboardingTranscript(i)<<"\\n";}\n')
                binary = source.with_suffix('')
                subprocess.run([compiler, '-std=c++17', '-D' + config + '=1', str(source),
                                '-o', str(binary)], check=True, capture_output=True)
                result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
                self.assertEqual(result.stdout.splitlines(),
                                 [manifest['products'][product][key]['text'] for key in keys])
