"""Exercise the CMake guard that reports a stale cross-product wake model."""
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
                    # A stale model is reported but must never fail configuration:
                    # the SDK configuration editor has to stay reachable.
                    self.assertEqual(result.returncode, 0, result.stderr)
                    if model != expected:
                        self.assertIn('WZH wake model mismatch', result.stderr)
                    else:
                        self.assertNotIn('WZH wake model mismatch', result.stderr)
            self.assertNotIn('WZH wake model mismatch',
                             self.run_check(board, 'WN9_NIHAOXIAOZHI_TTS').stderr)

    def test_allow_and_disabled_suppress_the_warning(self):
        for board, model in [('CRAWLPANDA', 'WN9_XIAOLONGXIAOLONG_TTS'),
                             ('DINOSAUR', 'WN9_NIHAOXIAOAN_TTS2')]:
            with self.subTest(board=board, model=model):
                self.assertIn('WZH wake model mismatch',
                              self.run_check(board, model).stderr)
                self.assertNotIn('WZH wake model mismatch',
                                 self.run_check(board, model, allow=True).stderr)
                self.assertNotIn('WZH wake model mismatch',
                                 self.run_check(board, model, enabled=False).stderr)

    def test_main_build_includes_guard(self):
        self.assertIn('boards/common/wzh/wake_model_check.cmake',
                      (ROOT / 'main/CMakeLists.txt').read_text())


if __name__ == '__main__':
    unittest.main()