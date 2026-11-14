"""Check that the three supplied onboarding packs stay distinct and complete."""
import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    'onboarding_audio', ROOT / 'scripts/audio/validate_wzh_onboarding.py')
audio = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audio)


class OnboardingAudioTest(unittest.TestCase):
    def test_three_complete_distinct_recordings(self):
        for profile in ('an', 'xing', 'long'):
            audio.validate(profile)
        root = ROOT / 'main/assets/wzh_onboarding_voices'
        manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
        self.assertIn('小华', manifest['products']['an']['intro']['text'])
        self.assertIn('小武', manifest['products']['xing']['intro']['text'])
        self.assertIn('小威', manifest['products']['long']['intro']['text'])
        # Product names differ even when the two pandas use the same voice.
        self.assertNotEqual((root / 'an/intro.ogg').read_bytes(),
                            (root / 'xing/intro.ogg').read_bytes())
        self.assertNotEqual((root / 'xing/intro.ogg').read_bytes(),
                            (root / 'long/intro.ogg').read_bytes())

    def test_interruption_prompts_match_configured_keyword_behavior(self):
        root = ROOT / 'main/assets/wzh_onboarding_voices'
        manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
        for profile in ('an', 'xing', 'long'):
            with self.subTest(profile=profile):
                text = manifest['products'][profile]['interrupt']['text']
                self.assertIn('等一下', text)
                self.assertIn('我要说话', text)
                audio.validate(profile)
        self.assertEqual((ROOT / 'main/assets/common/interrupt.ogg').read_bytes(),
                         (root / 'an/interrupt.ogg').read_bytes())

    def test_changed_recording_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            assets = Path(directory)
            shutil.copytree(ROOT / 'main/assets/wzh_onboarding_voices',
                            assets / 'wzh_onboarding_voices')
            (assets / 'common').mkdir()
            for key in audio.KEYS:
                (assets / 'common' / (key + '.ogg')).touch()
            (assets / 'wzh_onboarding_voices/an/intro.ogg').write_bytes(b'OggSdamaged')
            with mock.patch.object(audio, 'ROOT', assets):
                with self.assertRaisesRegex(ValueError, 'invalid onboarding audio'):
                    audio.validate('an')

    def test_language_header_can_exclude_product_only_prompts(self):
        with tempfile.TemporaryDirectory() as directory:
            assets = Path(directory) / 'main/assets'
            (assets / 'common').mkdir(parents=True)
            for language in ('en-US', 'zh-CN'):
                folder = assets / 'locales' / language
                folder.mkdir(parents=True)
                (folder / 'language.json').write_text(
                    json.dumps({'language': language, 'strings': {}}))
            for name in ('intro', 'success'):
                (assets / 'common' / (name + '.ogg')).touch()
            header = assets / 'lang_config.h'
            command = ['python3', str(ROOT / 'scripts/gen_lang.py'), '--language', 'zh-CN',
                       '--output', str(header)]
            subprocess.run(command + ['--exclude-common', 'intro'], check=True,
                           capture_output=True)
            content = header.read_text()
            self.assertNotIn('OGG_INTRO', content)
            self.assertIn('OGG_SUCCESS', content)
            subprocess.run(command, check=True, capture_output=True)
            self.assertIn('OGG_INTRO', header.read_text())


if __name__ == '__main__':
    unittest.main()
