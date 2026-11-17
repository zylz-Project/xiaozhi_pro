"""Validate product voice packs and the BLE provisioning recordings."""
import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    'onboarding_audio', ROOT / 'scripts/audio/validate_wzh_onboarding.py')
audio = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audio)


class OnboardingAudioTest(unittest.TestCase):
    def test_language_header_excludes_product_sounds_for_other_boards(self):
        with tempfile.TemporaryDirectory() as directory:
            assets = Path(directory) / 'main/assets'
            (assets / 'common').mkdir(parents=True)
            for language in ('en-US', 'zh-CN'):
                folder = assets / 'locales' / language
                folder.mkdir(parents=True)
                (folder / 'language.json').write_text(json.dumps({'language': language, 'strings': {}}))
            for name in ('intro', 'success'):
                (assets / 'common' / (name + '.ogg')).touch()
            header = assets / 'lang_config.h'
            command = ['python3', str(ROOT / 'scripts/gen_lang.py'), '--language', 'zh-CN',
                       '--output', str(header)]
            subprocess.run(command + ['--exclude-common', 'intro'], check=True, capture_output=True)
            self.assertNotIn('OGG_INTRO', header.read_text())
            self.assertIn('OGG_SUCCESS', header.read_text())
            subprocess.run(command, check=True, capture_output=True)
            self.assertIn('OGG_INTRO', header.read_text())

    def test_all_profiles_have_valid_ble_instructions_and_matching_hashes(self):
        root = ROOT / 'main/assets/wzh_onboarding_voices'
        manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
        for profile in ('an', 'xing', 'long'):
            with self.subTest(profile=profile):
                audio.validate(profile)
                connect = manifest['products'][profile]['ap']['text']
                network = manifest['products'][profile]['page']['text']
                self.assertIn('蓝牙', connect)
                self.assertIn('小程序', connect)
                self.assertIn('更改网络', network)
                self.assertNotIn('热点', connect + network)
                self.assertNotIn('192.168.4.1', connect + network)

    def test_confirmed_shared_panda_steps_and_common_fallback(self):
        root = ROOT / 'main/assets/wzh_onboarding_voices'
        manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
        for name in ('ap', 'page'):
            an = (root / 'an' / (name + '.ogg')).read_bytes()
            self.assertEqual(manifest['products']['an'][name]['source_sha256'],
                             manifest['products']['xing'][name]['source_sha256'])
            self.assertEqual(an, (ROOT / 'main/assets/common' / (name + '.ogg')).read_bytes())


if __name__ == '__main__':
    unittest.main()
