"""Check that the default host runner stays scoped to affected modules."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("host_test_runner", ROOT / "scripts/test.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class TestSelectionTests(unittest.TestCase):
    test_paths = {path.stem: path for path in runner.TESTS.glob("test_*.py")}

    def test_speaker_and_ble_changes_select_only_related_modules(self):
        modules, unmapped = runner.select_tests(["main/audio/speaker_protection.h"], self.test_paths)
        self.assertEqual(modules, [
            "test_es8311_output_gain", "test_product_audio_quality", "test_speaker_protection",
        ])
        self.assertEqual(unmapped, [])

        modules, unmapped = runner.select_tests(
            ["main/boards/common/wzh/wzh_ble_rx_stream.h", "main/audio/speaker_protection.h"],
            self.test_paths,
        )
        self.assertEqual(modules, [
            "test_es8311_output_gain", "test_product_audio_quality", "test_speaker_protection",
            "test_wzh_ble_base64", "test_wzh_ble_parity", "test_wzh_ble_rx_stream",
        ])
        self.assertEqual(unmapped, [])

    def test_miniprogram_and_wifi_keep_relevant_end_to_end_checks(self):
        modules, _ = runner.select_tests(["wzh-miniprogram/pages/device/device.js"], self.test_paths)
        self.assertIn("test_wzh_miniprogram_voice_record", modules)
        self.assertIn("test_wzh_ble_parity", modules)
        self.assertNotIn("test_wzh_motion_output", modules)

        modules, _ = runner.select_tests(["project_components/esp-wifi-connect/wifi_manager.cc"], self.test_paths)
        self.assertIn("test_wzh_wifi_startup", modules)
        self.assertIn("test_wzh_portal_credentials", modules)
        self.assertNotIn("test_speaker_protection", modules)

    def test_factory_reset_selects_firmware_policy_and_device_page(self):
        modules, unmapped = runner.select_tests(
            ["main/boards/common/wzh/wzh_factory_reset_policy.h"], self.test_paths,
        )
        self.assertEqual(modules, ["test_miniprogram_device_lifecycle", "test_wzh_factory_reset_policy"])
        self.assertEqual(unmapped, [])

    def test_docs_and_unmapped_sources_do_not_trigger_full_suite(self):
        modules, unmapped = runner.select_tests(
            ["README.md", "docs/wzh/archive/report.json", "docs/wzh/motion/motion_preview.html",
             "tmp/local-backups/source.py", "main/new_module.cc"], self.test_paths,
        )
        self.assertEqual(modules, [])
        self.assertEqual(unmapped, ["main/new_module.cc"])

    def test_changed_or_deleted_test_helper_selects_transitive_importers(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            for name, source in {
                "test_child": "from helper_wrapper import read_fixture\n",
                "helper_wrapper": "from helper import read_fixture\n",
                "test_grandchild": "import test_child\n",
                "test_unrelated": "import unittest\n",
                "test_unrelated_draft": "invalid python syntax\n",
            }.items():
                (folder / f"{name}.py").write_text(source)
            with mock.patch.object(runner, "TESTS", folder):
                test_paths = {path.stem: path for path in folder.glob("test_*.py")}
                modules, unmapped = runner.select_tests(["scripts/tests/helper.py"], test_paths)
            self.assertEqual(modules, ["test_child", "test_grandchild"])
            self.assertEqual(unmapped, [])

    def test_git_comparison_includes_commits_staging_untracked_and_renames(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def git(*args):
                return subprocess.check_output(
                    ["git", *args], cwd=root, input="", text=True, stderr=subprocess.PIPE,
                ).strip()

            git("init", "-q")
            git("config", "user.name", "Host Test")
            git("config", "user.email", "host-test@example.invalid")
            git("config", "commit.gpgsign", "false")
            (root / ".gitignore").write_text("ignored.py\n")
            for name in ("rename me.py", "staged.py", "unstaged.py"):
                (root / name).write_text("original\n")
            git("add", ".")
            git("commit", "-qm", "base")
            base = git("rev-parse", "HEAD")
            git("mv", "rename me.py", "renamed.py")
            git("commit", "-qm", "rename")
            (root / "staged.py").write_text("staged\n")
            git("add", "staged.py")
            (root / "staged.py").write_text("original\n")
            (root / "unstaged.py").write_text("unstaged\n")
            (root / "new.py").touch()
            (root / "ignored.py").touch()
            with mock.patch.object(runner, "ROOT", root):
                self.assertEqual(runner.changed_paths("HEAD"), {"staged.py", "unstaged.py", "new.py"})
                self.assertEqual(runner.changed_paths(base),
                                 {"staged.py", "unstaged.py", "new.py", "rename me.py", "renamed.py"})
                empty_tree = git("hash-object", "-w", "-t", "tree", "--stdin")
                self.assertIn(".gitignore", runner.changed_paths(empty_tree))
