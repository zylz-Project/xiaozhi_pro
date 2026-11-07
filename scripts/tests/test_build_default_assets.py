import importlib.util
import json
import tempfile
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "build_default_assets", ROOT / "scripts" / "build_default_assets.py"
)
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class BuildDefaultAssetsTest(unittest.TestCase):
    def test_wakenet_and_local_commands_package_both_models_without_custom_wake(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            sdkconfig = directory / 'sdkconfig'
            sdkconfig.write_text('\n'.join([
                'CONFIG_USE_AFE_WAKE_WORD=y', 'CONFIG_WZH_OFFLINE_COMMANDS=y',
                'CONFIG_WZH_OFFLINE_COMMAND_THRESHOLD=50',
                'CONFIG_SR_WN_WN9_XIAOLONGXIAOLONG_TTS=y',
                'CONFIG_SR_MN_CN_MULTINET7_QUANT=y',
                # Stale settings must not add the former wake phrase.
                'CONFIG_CUSTOM_WAKE_WORD="ni hao xiao zhi"',
                'CONFIG_CUSTOM_WAKE_WORD_DISPLAY="你好小智"',
                'CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=50',
            ]))
            for name in ['wakenet_model/wn9_xiaolongxiaolong_tts',
                         'multinet_model/mn7_cn', 'multinet_model/fst']:
                (directory / name).mkdir(parents=True)
            args = ['build_default_assets.py', '--sdkconfig', str(sdkconfig),
                    '--esp_sr_model_path', str(directory),
                    '--noto_fonts_path', str(directory),
                    '--output', str(directory / 'assets.bin')]
            with mock.patch('sys.argv', args), mock.patch.object(
                    BUILD, 'build_assets_integrated', return_value=True) as pack:
                BUILD.main()
            wn_paths, mn_paths = pack.call_args.args[:2]
            info = pack.call_args.args[6]
            self.assertEqual([Path(p).name for p in wn_paths], ['wn9_xiaolongxiaolong_tts'])
            self.assertEqual({Path(p).name for p in mn_paths}, {'mn7_cn', 'fst'})
            self.assertEqual(info['threshold'], 0.5)
            self.assertEqual(info['commands'], BUILD.wzh_offline_commands())

    def test_custom_wake_remains_compatible_with_local_commands(self):
        custom = dict(wake_word='ni hao xiao zhi', display='你好小智', threshold=0.6)
        info = BUILD.make_multinet_model_info(['mn7_cn'], custom, {'threshold': 0.5})
        self.assertEqual(len(info['commands']), 1 + len(BUILD.wzh_offline_commands()))
        self.assertEqual(info['commands'][0]['text'], '你好小智')
        self.assertEqual(info['threshold'], 0.6)
        self.assertEqual(info['commands'][1:], BUILD.wzh_offline_commands())

    def test_local_commands_reject_english_model_and_invalid_threshold(self):
        with self.assertRaises(ValueError):
            BUILD.make_multinet_model_info(['mn7_en'], None, {'threshold': 0.5})
        with tempfile.TemporaryDirectory() as directory:
            sdkconfig = Path(directory) / 'sdkconfig'
            sdkconfig.write_text('CONFIG_WZH_OFFLINE_COMMANDS=y\n'
                                 'CONFIG_WZH_OFFLINE_COMMAND_THRESHOLD=0\n')
            with self.assertRaises(ValueError):
                BUILD.read_local_command_config(sdkconfig)

    def test_each_wzh_variant_selects_one_distinct_wakenet_model(self):
        selected = []
        for board in ['wzh_dinosaur', 'wzh_crawlPanda', 'wzh_tailRedPanda']:
            data = json.loads((ROOT / 'main/boards' / board / 'config.json').read_text())
            config = data['builds'][0]['sdkconfig_append']
            self.assertIn('CONFIG_USE_AFE_WAKE_WORD=y', config)
            self.assertIn('CONFIG_USE_CUSTOM_WAKE_WORD=n', config)
            self.assertIn('CONFIG_WZH_OFFLINE_COMMANDS=y', config)
            selected.append(data['builds'][0]['default_wake_word'])
        self.assertEqual(len(set(selected)), 3)

    def test_text_font_metadata_uses_bundle_charset_size_and_bpp(self):
        with tempfile.TemporaryDirectory() as directory:
            assets = Path(directory)
            BUILD.generate_index_json(
                str(assets),
                None,
                "font_noto_sans_common_20_4.bin",
                None,
                font_bundle_id="noto-v1",
            )
            index = json.loads((assets / "index.json").read_text(encoding="utf-8"))
            self.assertEqual(
                index["text_font_meta"],
                {"charset": "common", "size": 20, "bpp": 4, "bundle": "noto-v1"},
            )

    def test_wzh_commands_are_packaged_for_local_dispatch(self):
        commands = BUILD.wzh_offline_commands()
        with tempfile.TemporaryDirectory() as directory:
            BUILD.generate_index_json(
                directory, None, None, None,
                multinet_model_info={"language": "cn", "commands": commands},
            )
            index = json.loads((Path(directory) / "index.json").read_text())
        packaged = index["multinet_model"]["commands"]
        self.assertEqual(len({entry["command"] for entry in packaged}), len(commands))
        self.assertTrue(
            {"local:happy", "local:affection", "local:eat", "local:sleepy",
             "local:shake_head", "local:look_left", "local:look_right",
             "local:stop", "local:quiet", "local:rest", "local:wake"}
            <= {entry["text"] for entry in packaged},
        )
        self.assertTrue(all(entry["action"] == "wake" for entry in packaged))

    def test_text_font_requires_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                BUILD.generate_index_json(
                    directory, None, "font_noto_sans_common_20_4.bin", None
                )


if __name__ == "__main__":
    unittest.main()
