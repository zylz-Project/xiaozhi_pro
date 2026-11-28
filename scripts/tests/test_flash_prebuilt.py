import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))

import flash_prebuilt
from wzh_firmware_image import FirmwareImage


class FlashPrebuiltTests(unittest.TestCase):
    def setUp(self):
        self.firmware = FirmwareImage(b'table', b'app', b'assets', b'ota', '2.4.1')

    def test_matching_flash_switches_boot_slot_after_writing_app_and_assets(self):
        for version, read_command, write_command in (
            ('5.3.0', 'read-flash', 'write-flash'),
            ('4.12.dev2', 'read_flash', 'write_flash'),
        ):
            with self.subTest(version=version):
                writes = []

                def esptool(args, *, check):
                    self.assertTrue(check)
                    self.assertEqual(args[:7], [sys.executable, '-m', 'esptool', '--chip',
                                                 'esp32s3', '--port', '/dev/test'])
                    if read_command in args:
                        self.assertEqual(args[-3:-1], ['0x8000', '0x5'])
                        Path(args[-1]).write_bytes(self.firmware.partition_table)
                        return
                    operands = args[args.index(write_command) + 1:]
                    if operands[:2] == ['-fs', 'detect']:
                        operands = operands[2:]
                    writes.append([(address, Path(path).read_bytes())
                                   for address, path in zip(operands[::2], operands[1::2])])

                with mock.patch.object(flash_prebuilt, 'version', return_value=version), \
                        mock.patch.object(flash_prebuilt.subprocess, 'run', side_effect=esptool):
                    flash_prebuilt.flash_firmware(self.firmware, '/dev/test', 460800)

                self.assertEqual(writes, [[('0x20000', b'app'), ('0x880000', b'assets')],
                                          [('0xd000', b'ota')]])

    def test_mismatched_partition_table_stops_before_writing(self):
        def read_table(args, **kwargs):
            Path(args[-1]).write_bytes(b'other')

        with mock.patch.object(flash_prebuilt, 'version', return_value='5.3.0'), \
                mock.patch.object(flash_prebuilt.subprocess, 'run', side_effect=read_table) as run:
            with self.assertRaisesRegex(ValueError, '分区表'):
                flash_prebuilt.flash_firmware(self.firmware, '/dev/test', 460800)
        run.assert_called_once()
        self.assertIn('read-flash', run.call_args.args[0])

    def test_failed_app_or_assets_write_does_not_reset_ota_selection(self):
        def esptool(args, **kwargs):
            if 'read-flash' in args:
                Path(args[-1]).write_bytes(self.firmware.partition_table)
                return
            raise subprocess.CalledProcessError(1, args)

        with mock.patch.object(flash_prebuilt, 'version', return_value='5.3.0'), \
                mock.patch.object(flash_prebuilt.subprocess, 'run', side_effect=esptool) as run:
            with self.assertRaises(subprocess.CalledProcessError):
                flash_prebuilt.flash_firmware(self.firmware, '/dev/test', 460800)
        self.assertEqual(run.call_count, 2)
        self.assertNotIn('0xd000', run.call_args.args[0])

    def test_latest_archive_matches_exact_variant_in_release_subdirectories(self):
        with tempfile.TemporaryDirectory() as temporary:
            releases = Path(temporary)
            child = releases / 'latest'
            child.mkdir()
            old = releases / 'v2.4.1_wzh-crawlpanda.zip'
            new = child / old.name
            other = child / 'v2.4.1_wzh-tailredpanda.zip'
            for index, package in enumerate((old, new, other), start=1):
                package.touch()
                os.utime(package, (index, index))
            self.assertEqual(flash_prebuilt.latest_archive(releases, 'wzh-crawlpanda'), new)
            self.assertEqual(flash_prebuilt.latest_archive(releases, 'wzh-tailredpanda'), other)
            with self.assertRaises(ValueError):
                flash_prebuilt.latest_archive(releases, 'wzh-dinosaur-270-servo')

    def test_vscode_picker_covers_all_supported_variants(self):
        tasks = json.loads((ROOT / '.vscode/tasks.json').read_text())
        options = tasks['inputs'][0]['options']
        self.assertCountEqual([option['value'] for option in options], flash_prebuilt.VARIANTS)
