# AGENTS.md

This ESP-IDF fork of XiaoZhi ships three ESP32-S3 products. Each firmware build selects one board factory. Start with [the WZH index](docs/wzh/README.md) for product-specific documentation.

## Find the owning code

- `main/boards/wzh_*` owns product wiring, initialization, and motion. Read the relevant board's `AGENTS.md`, `config.h`, and closest implementation.
- `main/boards/common/wzh/` owns shared product features such as touch, protection, local audio, onboarding, and portals. Portal assets live in `main/assets/wzh_ui/`. The maintained Wi-Fi component lives in `project_components/esp-wifi-connect/`.
- `main/application.*` and `main/device_state_machine.*` own the event loop and runtime states. `main/audio/`, `main/protocols/`, `main/display/`, `main/led/`, and `main/mcp_server.*` own reusable audio, transport, presentation, and MCP behavior.
- Scope routine searches to the owning paths and relevant `scripts/tests/` modules. Use `docs/wzh/archive/` for historical evidence on demand. Read generated builds, vendor files, and large `wzh_user_pairs_catalog.inc` assets only when the task requires them.

## Implementation rules

- Keep patches focused and preserve existing worktree changes. Never change shipping pins to support a different assembly. Add a uniquely named board or variant and follow [the board guide](docs/custom-board.md).
- Keep board selection consistent across `config.json`, `scripts/build.py`, `main/Kconfig.projbuild`, `main/CMakeLists.txt`, and the board sources. Keep one `DECLARE_BOARD(...)` per build and preserve OTA identity, chip, and partition compatibility.
- Use `Board` interfaces from core code and handle optional hardware. Change device state through `Application::SetDeviceState()`. Schedule application mutations from callbacks via `Application::Schedule()` or event bits.
- Keep the main loop and audio tasks responsive. Bound queues and audio allocations. Keep shared network semantics in `Protocol`, validate incoming data, preserve `cJSON` ownership, and migrate persistent NVS keys when changing them.
- Guard target features with Kconfig and component rules. Edit maintained sources only. Leave `build/`, `dist/`, `releases/`, `managed_components/`, the legacy `components/` copy, generated `sdkconfig` files, `main/assets/lang_config.h`, and mmap headers to their generators. Source defaults are maintained through the board configuration chain. Format only touched C/C++ lines with `.clang-format`.

## Build and verify

Use ESP-IDF v6.0.2 when available. See [SDK compatibility](docs/esp-idf-6-migration.md) for legacy environments.

```sh
source /path/to/esp-idf/export.sh
idf.py --version
python3 scripts/build.py --list-boards
python3 scripts/build.py <board-directory> --name <variant-name>
python3 scripts/run_host_tests.py
```

`build.py` updates the checkout's `sdkconfig` and `build/`. Build sequentially in a checkout or use isolated copies. Run affected board builds and relevant host tests. For shared protocol changes, cover WebSocket and MQTT/UDP. For audio changes, cover capture, playback, wake/VAD, interruption, reconnect, and applicable AEC modes. For UI or assets, check affected display paths and partition usage. Report hardware checks still pending.

Keep this file for durable rules. Put product behavior, version-specific details, and test evidence in the linked documentation.
