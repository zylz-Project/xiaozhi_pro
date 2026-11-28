# AGENTS.md — WZH Dinosaur

These instructions apply to `main/boards/wzh_dinosaur/` in addition to the repository instructions. Start with [the board README](README.md), [config.h](config.h), [the runtime guide](XIAOZHI_RUNTIME.md), and the nearest implementation.

## Board contract

- Keep ESP32-S3 board ID `wzh-dinosaur`, build name `wzh-dinosaur-270-servo`, Kconfig selector `CONFIG_BOARD_TYPE_WZH_DINOSAUR`, and the sole factory `DECLARE_BOARD(WzhDinosaur)`. Product metadata is in `wzh_product.cc`.
- Keep the logical axis order neck tilt, neck lean, head turn, tail up/down, tail left/right. The defaults are 70/90/90/90/90. IO17 controls tilt, IO16 lean, IO15 head turn, IO8 tail up/down, and IO18 tail left/right. Tail up/down stops at 55° on the low side. A smaller neck-tilt angle raises the neck, and a larger tail-up/down angle raises the tail. Neck lean and head turn increase toward the left. Tail left/right increases toward the right.
- Use 50 Hz PWM. Apply the 270° calibration only at output for IO17, IO16, IO8, and IO18. These axes use the 135° center (1500 µs) and ±100° physical travel. IO15 uses the 180° calibration.
- Legacy `auto_run_data.cc` has reversed `tail_ud` coordinates. Translate that single axis with `180 - angle` when importing. All current keyframes use the board's logical directions.
- ESP32 TX=IO12 and RX=IO11 connect to ML307 RX and TX. Keep both Wi-Fi and the firmware's AT transport. The selected transport provides its own connection readiness, while the Application owns the conversation.

## Motion and integration

- `dinosaur_controller.*` owns the `dino_motion` task and interpolation. Use the bounded latest-intent queue for callbacks. Clamp frame targets and final output. Use one interpolation progress value for all changing axes. Keep `kKeep` axes inherited when mirroring and preserve the exact axes of directed body commands.
- Preserve smooth traversal of same-direction keyframes, output-only voice/living offsets, and neck-lift attenuation at the deep feeding pose. Interruptions return from the last commanded pose. Protections release immediately. Idle follows the [shared cadence](../../../docs/wzh/motion/motion_libraries.md) and releases after returning to the default pose.
- Use actual speaker PCM for motion feedback. Offline performances choose an actual Flash recording and align motion to playback. Keep the duration-aware roar families, distinct openings, compile-time pose checks, and recorded trajectories for paired sounds. Keep `Roar pose` diagnostics.
- Keep `self.dinosaur.test_servo(gpio)` explicit, queued, limited, and free of voice/living overlays. Verify axis directions through the assembly before changing any action calibration.
- `wzh_dinosaur.cc` composes the board and registers the product hooks in `OnAudioServiceReady()`. `dinosaur_power.*` owns ADC, shutdown, and power latching. `dinosaur_cellular.*` and `dinosaur_ml307_*.*` own cellular signaling and AT transports. Application callbacks enqueue work.
- Keep `DinoAction`, `DinoBodyAction`, clip family indexes, parsers, keyword mappings, and `self.dinosaur.*` tools aligned. Preserve NVS keys, audio formats, device identity and the `dinosaur` tool prefix. Select stored audio from `AudioStore::Snapshot()`.

## Validation

Use ESP-IDF v6.0.2 and `python3 scripts/build.py wzh_dinosaur --name wzh-dinosaur-270-servo`. Changes to shared WZH modules also require the other two WZH builds and host tests per the root instructions. On the device check the five-axis direction, combination clearance, powered release, directional touch, 4G/Wi-Fi cold start and recovery, and audio under load.
