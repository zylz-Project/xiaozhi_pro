# AGENTS.md — WZH Tail Red Panda

These instructions apply to `main/boards/wzh_tailRedPanda/` in addition to the repository instructions. Start with [the board README](README.md), [config.h](config.h), and the nearest implementation.

## Board contract

- Keep ESP32-S3 board ID `wzh-tailredpanda`, Kconfig selector `CONFIG_BOARD_TYPE_WZH_TAILREDPANDA`, and the sole factory `DECLARE_BOARD(WzhTailRedPanda)`. Product metadata is in `wzh_product.cc`.
- Servo order is head, tail left/right, tail up/down. The head uses IO18, center 90°, and 30–150° limits. Tail left/right uses IO15 with center 90° and 6–174° limits. Tail up/down uses IO16 with 0–180° limits and a 150° sitting pose. A smaller tail up/down angle lifts the tail. Keep the output clamps and 50 Hz PWM.
- Keep the exact axes of explicit body commands and keep directional commands out of random mirroring. Preserve the dedicated `startled_flinch` family. Inspect full tail arcs and combined poses for clearance.
- SW4 defaults to isolation. Preserve the existing NVS isolation key in `panda_power.*` and cancel the old contact when the isolation setting changes.

## Ownership

- `wzh_tailRedPanda.cc` composes power, ES8311, local audio, motion, sensors, buttons, and product tools. Keep the order of board initialization and deferred setup in `OnAudioServiceReady()`.
- `panda_controller.*` runs interpolation and owns movement in `panda_motion`. Callbacks submit bounded requests. Interruptions return from the last commanded pose. Stop, maintenance and protection take priority, with immediate release when a protection trips.
- `panda_power.*` owns ADC sampling and power transitions. Let the motion task park healthy powered servos during normal shutdown. The power task owns release, the goodbye prompt and latch cut. Critical undervoltage cuts the latch directly.
- `panda_audio.*` selects local audio. Share ES8311 I2S and speaker PCM via AudioService. Select stored audio from `AudioStore::Snapshot()`.
- Keep action enums, clip and family tables, parsers, keyword mappings, and `self.panda.*` tools aligned. Preserve NVS keys, audio formats and device identity. Double-click of either key uses the Application conversation toggle. BOOT single click opens provisioning during start or configuration.

## Validation

Use ESP-IDF v6.0.2 and `python3 scripts/build.py wzh_tailRedPanda --name wzh-tailredpanda`. Changes to shared WZH modules also require the other two WZH builds and host tests per the root instructions. On the device check the 150° tail rest pose, assembly directions, extreme and combined poses, SW4 isolation, servo current, capture, playback, and keyword barge-in.
