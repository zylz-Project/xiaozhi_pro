"""Exercise the actual CMake guard used by both IDE and scripted builds."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
CHECK = ROOT / 'main/boards/common/wzh/wake_model_check.cmake'


class WakeModelCheckTest(unittest.TestCase):
    def run_check(self, board, model, *, enabled=True, allow=False):
        args = ['cmake', f'-DCONFIG_BOARD_TYPE_WZH_{board}=y',
                f'-DCONFIG_SR_WN_{model}=y',
                f'-DCONFIG_USE_AFE_WAKE_WORD={"y" if enabled else "n"}',
                f'-DCONFIG_WZH_ALLOW_CROSS_PRODUCT_WAKE_MODEL={"y" if allow else "n"}',
                '-P', str(CHECK)]
        return subprocess.run(args, capture_output=True, text=True)

    def test_product_models_and_cross_product_mismatches(self):
        pairs = {'CRAWLPANDA': 'WN9_NIHAOXIAOAN_TTS2',
                 'TAILREDPANDA': 'WN9L_NI3HAO3XING1BAO3_TTS3',
                 'DINOSAUR': 'WN9_XIAOLONGXIAOLONG_TTS'}
        for board, expected in pairs.items():
            for model in pairs.values():
                with self.subTest(board=board, model=model):
                    result = self.run_check(board, model)
                    self.assertEqual(result.returncode == 0, model == expected, result.stderr)
                    if model != expected:
                        self.assertIn('WZH wake model mismatch', result.stderr)
                        self.assertEqual(self.run_check(board, model, allow=True).returncode, 0)
                        self.assertEqual(self.run_check(board, model, enabled=False).returncode, 0)
            self.assertEqual(self.run_check(board, 'WN9_NIHAOXIAOZHI_TTS').returncode, 0)

    def test_main_build_includes_guard(self):
        self.assertIn('boards/common/wzh/wake_model_check.cmake',
                      (ROOT / 'main/CMakeLists.txt').read_text())
