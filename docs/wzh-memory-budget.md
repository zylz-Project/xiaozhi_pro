# WZH runtime memory budget and validation

The crawling panda, tail red panda and dinosaur share these policies. A build
still selects one board and must retain that board's identity and flash layout.

## Memory classes

* Internal RAM is required for DMA, radio internals and flash-writing task
  stacks. Track both free bytes and the largest allocatable block.
* The ESP32-S3 devices examined have 8 MiB PSRAM. Large, bounded audio buffers
  and suitable task stacks can use it after checking cache and DMA constraints.
* The W25N01GV is 128 MiB SPI NAND, with 125 MiB in the configured user area.
  It persists compressed audio and is not addressable as heap or task stack.

The dinosaur 0AD0 diagnostic log recorded a minimum of 3,095 internal bytes.
Voice enrollment failed with 44,583 internal bytes free but only 8,704 bytes
in its largest free block; its task requests 10,240 bytes. These are individual
observations, not a guaranteed worst-case bound for other devices.

## Admission and experiments

The optional LAN developer console is lower priority than normal use. Its
bootstrap waits for at least 48 KiB internal free memory and a 12 KiB largest
block. An HTTP start failure is not retried within that boot, since ESP-IDF
6.0.2 failure paths can leave sockets without an accessible server handle.
BLE management remains available. Restart the device to retry the developer
console; it may still remain unavailable until memory is sufficient.

`CONFIG_WZH_OFFLINE_AUDIO_PSRAM_EXPERIMENT` is disabled by default. It keeps
the offline worker's 14,336-byte stack in PSRAM and a 4,096-byte SPI DMA read
buffer in internal RAM, falling back to the original internal-stack worker if
either allocation fails. The expected nominal internal reduction is about
10 KiB, not a measured guarantee. The worker is device-lifetime, and its
external-stack path must not acquire NVS or internal flash-write operations.
Do not enable it for release until tested on all products, including concurrent
external NAND access, NVS writes, OTA success and failure, shutdown and restart.

Do not interpret SPI NAND capacity as runtime free RAM, or move arbitrary
tasks to PSRAM: flash cache-disabled operations cannot safely run from an
external stack. Keep Wi-Fi/BLE DMA allocations internal as required by IDF.
See Espressif's [ESP-IDF 6.0.2 external RAM restrictions](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-guides/external-ram.html).
The crawling-panda board configuration explicitly enables external BSS for
supported network libraries; the dinosaur and tail-red-panda configurations
do not explicitly enable it. Do not assume this difference can be removed
without separately testing each board's radio, NVS and OTA behavior.

For the dinosaur 0AD0 diagnostic build on 2026-11-17, a controlled linker-map
comparison with external BSS enabled moved 15,216 bytes of BSS to PSRAM and
reduced total DIRAM usage by 15,152 bytes. Both builds also enabled the offline
worker experiment and used the same assets and partition table. This is a
static build comparison, not a runtime low-water measurement or proof that
the setting is stable on physical hardware. It has not been enabled in the
dinosaur or red-panda production configurations.

## Hardware acceptance

Capture free internal, minimum-ever internal, largest internal and DMA blocks,
PSRAM and task stack high-water marks at boot, after network/BLE startup, and
under simultaneous conversation and phone control. Compare against the same
device and firmware baseline; test cold boot and long soak, fragmented heap,
poor Wi-Fi/reconnect, ordinary and cloned voices, long replies, interruption,
voice enrollment/selection, local sounds, onboarding, management, reset, NVS,
OTA (including failure), and power cycling on all three products. Record audio
dropouts, latency and functional outcomes alongside memory snapshots. A build
or a single successful conversation does not establish a no-allocation-failure
or no-stutter guarantee.
