"""Read components from the raw image in a standard WZH release ZIP."""

from dataclasses import dataclass
from hashlib import md5, sha256
from pathlib import Path
import struct
import zipfile
import zlib


PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_LENGTH = 0xC00
OTADATA_OFFSET = 0xD000
APP_OFFSET = 0x20000
ASSETS_OFFSET = 0x880000

_FLASH_SIZE = 0x1000000
_OTADATA_SIZE = 0x2000
_APP_SLOT_SIZE = 0x430000
_ASSETS_SIZE = _FLASH_SIZE - ASSETS_OFFSET
VARIANTS = (
    "wzh-crawlpanda",
    "wzh-tailredpanda",
    "wzh-dinosaur-270-servo",
)


@dataclass(frozen=True)
class FirmwareImage:
    partition_table: bytes
    app: bytes
    assets: bytes
    otadata: bytes
    version: str


def _merged_binary(archive: Path) -> bytes:
    try:
        with zipfile.ZipFile(archive) as package:
            entries = package.infolist()
            if len(entries) != 1 or entries[0].filename != "merged-binary.bin":
                raise ValueError("expected a ZIP containing only merged-binary.bin")
            if not ASSETS_OFFSET + 12 <= entries[0].file_size <= _FLASH_SIZE:
                raise ValueError("merged image size is outside the 16 MiB flash layout")
            return package.read(entries[0])
    except (OSError, zipfile.BadZipFile, RuntimeError, zlib.error) as exc:
        raise ValueError(f"cannot read release ZIP {archive}") from exc


def _partition_table(image: bytes) -> bytes:
    # partitions/v2/16m.csv in ESP-IDF partition-table binary format.
    partitions = (
        (1, 2, 0x9000, 0x4000, b"nvs"),
        (1, 0, OTADATA_OFFSET, _OTADATA_SIZE, b"otadata"),
        (1, 1, 0xF000, 0x1000, b"phy_init"),
        (0, 0x10, APP_OFFSET, _APP_SLOT_SIZE, b"ota_0"),
        (0, 0x11, APP_OFFSET + _APP_SLOT_SIZE, _APP_SLOT_SIZE, b"ota_1"),
        (1, 0x82, ASSETS_OFFSET, _ASSETS_SIZE, b"assets"),
    )
    entries = b"".join(struct.pack("<HBBII16sI", 0x50AA, *part, 0) for part in partitions)
    records = entries + b"\xEB\xEB" + b"\xFF" * 14 + md5(entries).digest()
    table = image[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_LENGTH]
    if table != records.ljust(PARTITION_TABLE_LENGTH, b"\xFF"):
        raise ValueError("partition table does not match partitions/v2/16m.csv")
    return table


def _esp_image(image: bytes, start: int, limit: int, label: str) -> bytes:
    header = image[start:start + 24]
    if (len(header) != 24 or header[0] != 0xE9 or not 1 <= header[1] <= 16
            or header[12:14] != b"\x09\x00" or header[23] != 1):
        raise ValueError(f"{label} is not an ESP32-S3 image with an appended digest")
    checksum = 0xEF
    pos = start + 24
    for _ in range(header[1]):
        if pos + 8 > limit:
            raise ValueError(f"{label} segment header exceeds its partition")
        _, size = struct.unpack_from("<II", image, pos)
        end = pos + 8 + size
        if end > limit:
            raise ValueError(f"{label} segment exceeds its partition")
        for byte in memoryview(image)[pos + 8:end]:
            checksum ^= byte
        pos = end
    # ESP images put the XOR checksum at the end of the padded 16-byte block.
    checksum_offset = pos | 15
    hash_offset = checksum_offset + 1
    end = hash_offset + 32
    if end > limit or image[checksum_offset] != checksum:
        raise ValueError(f"{label} checksum is invalid")
    if sha256(memoryview(image)[start:hash_offset]).digest() != image[hash_offset:end]:
        raise ValueError(f"{label} SHA-256 digest is invalid")
    return image[start:end]


def _app_version(app: bytes, variant: str) -> str:
    # SystemInfo emits BOARD_NAME followed by '/' as the user-agent prefix.
    # Unlike BOARD_TYPE, this literal includes the full build variant.
    identities = [name for name in VARIANTS if name.encode("ascii") + b"/\0" in app]
    if identities != [variant]:
        raise ValueError(f"application build identity does not match {variant}")
    # The esp_app_desc_t lives at the beginning of the first image segment.
    if len(app) < 32 + 256 or struct.unpack_from("<I", app, 32)[0] != 0xABCD5432:
        raise ValueError("application descriptor is missing")
    version, project = struct.unpack_from("<32s32s", app, 48)
    version = version.split(b"\0", 1)[0].decode("ascii")
    if not version or project.split(b"\0", 1)[0] != b"xiaozhi":
        raise ValueError("application descriptor has an invalid version or project")
    return version


def _assets(image: bytes) -> bytes:
    count, checksum, length = struct.unpack_from("<III", image, ASSETS_OFFSET)
    end = ASSETS_OFFSET + 12 + length
    if count == 0 or length < count * 44 or end > len(image):
        raise ValueError("assets header has an invalid file count or length")
    if any(byte != 0xFF for byte in memoryview(image)[end:]):
        raise ValueError("merged image has unexpected data after the declared assets")
    if sum(memoryview(image)[ASSETS_OFFSET + 12:end]) & 0xFFFF != checksum:
        raise ValueError("assets checksum is invalid")
    return image[ASSETS_OFFSET:end]


def read_firmware(archive: Path, variant: str) -> FirmwareImage:
    """Validate and split a standard release image for the requested WZH variant."""
    if variant not in VARIANTS:
        raise ValueError(f"unsupported WZH build variant: {variant}")
    image = _merged_binary(archive)
    partition_table = _partition_table(image)
    _esp_image(image, 0, PARTITION_TABLE_OFFSET, "bootloader")
    app = _esp_image(image, APP_OFFSET, APP_OFFSET + _APP_SLOT_SIZE, "application")
    # Standard releases include an unsigned OTA0 image with no OTA1 data.
    if any(byte != 0xFF for byte in memoryview(image)[APP_OFFSET + len(app):ASSETS_OFFSET]):
        raise ValueError("merged image has unexpected data between application and assets")
    version = _app_version(app, variant)
    otadata = image[OTADATA_OFFSET:OTADATA_OFFSET + _OTADATA_SIZE]
    if otadata != b"\xFF" * _OTADATA_SIZE:
        raise ValueError("archive does not contain initial OTA selection data")
    return FirmwareImage(partition_table, app, _assets(image), otadata, version)
