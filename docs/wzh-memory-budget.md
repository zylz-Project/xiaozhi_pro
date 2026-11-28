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

## BLE allocation boundary

The BLE worker uses one reusable 4,097-byte PSRAM frame buffer rather than
keeping a growable partial line and copying queued frames. GATT writes are
queued atomically and carry a connection generation. Voice uploads decode
chunks into the preallocated PSRAM upload buffer and do not fall back to the
general heap for the full 160–640 kB request. BLE JSON notifications reuse the
serialized buffer for the trailing newline. These are source-level allocation
reductions, not observed changes in audio-task or internal heap low-water marks.

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
The three default board configurations now enable external BSS for eligible
libraries. The table compares ESP-IDF v6.0.2 linker maps from an intermediate
build before the external-BSS changes and the final build. Additional changes
landed between the two maps, so the total internal delta includes those changes.

| Board | Internal DIRAM before | Internal DIRAM after | Change | External BSS after |
| --- | ---: | ---: | ---: | ---: |
| Crawling panda | 156,447 B | 156,495 B | +48 B | 15,216 B |
| Tail red panda | 171,647 B | 156,559 B | −15,088 B | 15,216 B |
| Dinosaur | 171,599 B | 156,495 B | −15,104 B | 15,216 B |

Crawling panda already placed the same BSS in PSRAM. The other two now free
about 14.7 KiB of static internal RAM. No runtime heap measurement follows from
the linker map alone. The old maps are under `tmp/parity-audit/pre-idle/` and
new maps are under `tmp/parity-audit/verified/`.

A manual Audio Hub job returns its 8 KiB task stack on completion. The BLE
voice-record polling path omits the 3 KiB status worker used by legacy clients.
Onboarding releases its 4 KiB worker when it is waiting for a phone or the
first conversation. These savings apply to different phases and should not be
added to predict a runtime minimum. PSRAM stack experiments remain disabled.

The 2026-11-27 22:54–23:06 TailRedPanda trace recorded a 9,331 B internal
heap low-water mark. The follow-up halves the 1,024-entry echo reference
history from 4,096 to 2,048 B and places Ali Opus payloads in PSRAM. The
144-packet Ali limit fits the existing queue storage. The decoded PCM
limit continues at six frames. MQTT and WebSocket discard replaced audio
before constructing payload buffers. Periodic heap logs include the largest
contiguous internal block. Runtime savings require a same-device comparison.

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
