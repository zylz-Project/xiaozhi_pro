# AGENTS.md — WZH Crawl Panda

These instructions apply to `main/boards/wzh_crawlPanda/` in addition to the repository instructions. Start with [the board README](README.md), [config.h](config.h), and the nearest implementation.

## Board contract

- Keep ESP32-S3 board ID `wzh-crawlpanda`, Kconfig selector `CONFIG_BOARD_TYPE_WZH_CRAWLPANDA`, and the sole factory `DECLARE_BOARD(WzhCrawlPanda)`. Product metadata is in `wzh_product.cc`.
- Servo order is head, left front paw, right front paw. The head uses IO17, center 90°, and 30–150° limits. IO15 crawls toward smaller angles and IO16 toward larger angles. Both paw ranges are 8–172° with 90° defaults. Keep the output clamps and 50 Hz PWM.
- Preserve the separate alternating and synchronous crawl gaits and their traction. Keep the exact axes of an explicit body command and keep directional commands out of random mirroring.

## Ownership

- `wzh_crawlPanda.cc` composes power, ES8311, local audio, motion, sensors, buttons, and product tools. Keep the order of board initialization and deferred setup in `OnAudioServiceReady()`.
- `panda_controller.*` runs interpolation and owns movement in `panda_motion`. Callbacks submit bounded requests. Interruptions return from the last commanded pose. Stop, maintenance and protection take priority, with immediate release when a protection trips.
- `panda_power.*` owns ADC sampling and power transitions. Let the motion task park healthy powered servos during normal shutdown. The power task owns release, the goodbye prompt and latch cut. Critical undervoltage cuts the latch directly.
- `panda_audio.*` selects local audio. Share ES8311 I2S and speaker PCM via AudioService. Select stored audio from `AudioStore::Snapshot()`.
- Keep action enums, clip and family tables, parsers, keyword mappings, and `self.panda.*` tools aligned. Preserve NVS keys, audio formats and device identity. Double-click of either key uses the Application conversation toggle. BOOT single click opens provisioning during start or configuration.

## Validation

Use ESP-IDF v6.0.2 and `python3 scripts/build.py wzh_crawlPanda --name wzh-crawlpanda`. Changes to shared WZH modules also require the other two WZH builds and host tests per the root instructions. On the device check the mechanical directions, crawl traction, combined head/paw clearance, servo current, power behavior, touch, capture, and playback.
