#!/usr/bin/env python3
"""Flash a prebuilt WZH release over USB without rebuilding or touching NVS."""

import argparse
import subprocess
import sys
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path
from tempfile import TemporaryDirectory

from wzh_firmware_image import (
    APP_OFFSET,
    ASSETS_OFFSET,
    OTADATA_OFFSET,
    PARTITION_TABLE_OFFSET,
    VARIANTS,
    FirmwareImage,
    read_firmware,
)


RELEASES = Path(__file__).resolve().parent.parent / 'releases'


def latest_archive(releases: Path, variant: str) -> Path:
    archives = [path for path in releases.rglob(f'v*_{variant}.zip') if path.is_file()]
    if not archives:
        raise ValueError(f'找不到 {variant} 的固件 ZIP。请先用 build.py --zip 构建一次。')
    return max(archives, key=lambda path: (path.stat().st_mtime_ns, path.as_posix()))


def flash_firmware(firmware: FirmwareImage, port: str, baud: int) -> None:
    try:
        legacy = int(version('esptool').split('.')[0]) < 5
    except PackageNotFoundError as exc:
        raise ValueError('找不到 esptool。请使用 VS Code 的 ESP-IDF 环境运行。') from exc

    after_hard_reset = 'hard_reset' if legacy else 'hard-reset'
    after_no_reset = 'no_reset' if legacy else 'no-reset'
    read_flash = 'read_flash' if legacy else 'read-flash'
    write_flash = 'write_flash' if legacy else 'write-flash'
    esptool = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', port,
               '--baud', str(baud)]

    with TemporaryDirectory(prefix='wzh-flash-') as temporary:
        work = Path(temporary)
        device_table = work / 'device-partitions.bin'
        subprocess.run(esptool + ['--after', after_hard_reset, read_flash,
                                  hex(PARTITION_TABLE_OFFSET), hex(len(firmware.partition_table)),
                                  str(device_table)], check=True)
        if device_table.read_bytes() != firmware.partition_table:
            raise ValueError('设备分区表与固件包不一致，已停止烧录。新设备或旧布局需要先烧录完整固件包。')

        app = work / 'app.bin'
        assets = work / 'assets.bin'
        otadata = work / 'otadata.bin'
        app.write_bytes(firmware.app)
        assets.write_bytes(firmware.assets)
        otadata.write_bytes(firmware.otadata)
        subprocess.run(esptool + ['--after', after_no_reset, write_flash, '-fs', 'detect',
                                  hex(APP_OFFSET), str(app), hex(ASSETS_OFFSET), str(assets)],
                       check=True)
        # The bootloader selects ota_0 after otadata is reset. Switch only after
        # esptool has written and verified the matching app and assets.
        subprocess.run(esptool + ['--after', after_hard_reset, write_flash,
                                  hex(OTADATA_OFFSET), str(otadata)], check=True)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('variant', choices=VARIANTS)
    parser.add_argument('--port', help='ESP-IDF 串口，例如 /dev/ttyACM0')
    parser.add_argument('--baud', type=int, default=460800)
    parser.add_argument('--archive', type=Path, help='指定固件 ZIP，默认使用 releases 中最近构建的对应型号')
    parser.add_argument('--dry-run', action='store_true', help='验证固件包，不访问串口')
    args = parser.parse_args(argv)
    if not args.dry_run and not args.port:
        parser.error('请先设置 ESP-IDF 串口或提供 --port')
    if args.baud <= 0:
        parser.error('烧录速率必须大于 0')

    try:
        archive = args.archive or latest_archive(RELEASES, args.variant)
        firmware = read_firmware(archive, args.variant)
        print(f'{args.variant} v{firmware.version} · {archive}', flush=True)
        print(f'应用 {len(firmware.app):,} B，语音资源 {len(firmware.assets):,} B', flush=True)
        if args.dry_run:
            return 0
        flash_firmware(firmware, args.port, args.baud)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'烧录停止：{error}', file=sys.stderr)
        return 1

    print('烧录完成，设备已重启。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
