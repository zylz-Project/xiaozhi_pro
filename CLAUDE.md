# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Read AGENTS.md first

`AGENTS.md` is the authoritative repo-wide guide (upstream XiaoZhi architecture map, required rules, board-selection chain, validation policy). Follow it; this file only adds what is specific to this fork.

This repo is 78/xiaozhi-esp32 plus the **WZH product migration**: three proprietary ESP32-S3 toys — `wzh_crawlPanda` (爬爬熊猫), `wzh_dinosaur` (恐龙), `wzh_tailRedPanda` (摇尾小熊猫) — ported onto XiaoZhi. WZH work happens on branch `feature/wzh-product-migration`, one commit per task.

`docs/wzh_migration.md` is the migration truth table: hardware pinout tables, PNDA TOC v1/v2/v3 format, Audio Hub protocol, per-phase checklist. Consult it before touching WZH code, and check off its boxes as items are real-device verified.

## Build commands

ESP-IDF v6.0.1 lives at `/home/wzh/.espressif/v6.0.1/esp-idf`:

```sh
. /home/wzh/.espressif/v6.0.1/esp-idf/export.sh

python3 scripts/build.py --list-boards   # discover exact names
python3 scripts/build.py wzh_crawlPanda --name wzh-crawlpanda
python3 scripts/build.py wzh_dinosaur --name wzh-dinosaur
python3 scripts/build.py wzh_tailRedPanda --name wzh-tailredpanda

python3 -m unittest discover -s scripts/tests -v   # host-side tests
clang-format -i <touched-files>                    # format only touched C/C++
```

- The `board` argument is the **case-sensitive directory name** (`wzh_crawlPanda`, never `wzh_crawlpanda`); `--name` is the lowercase build name from that board's `config.json` and is what OTA reports.
- The three WZH boards share one `build/` directory — run builds **sequentially**, never in parallel.
- `scripts/build.py` rewrites `sdkconfig` for the selected board; don't assume the build dir still represents a previous target.
- A successful build is not hardware validation (AGENTS.md §Validation also applies).

## WZH architecture beyond upstream

- **Board chain**: `config.json` → `scripts/build.py` → `main/Kconfig.projbuild` (`CONFIG_BOARD_TYPE_WZH_*` + `menu "WZH Product Configuration"` — flash type override, Audio Hub sync toggle/server) → `main/CMakeLists.txt` (BOARD_DIR mapping, WZH common-sources block) → board source + `config.h`. Exactly one `DECLARE_BOARD` per build.
- **Shared product layer** `main/boards/common/wzh/`, compiled only under the three WZH board types. `wzh::Product()` (per-board `wzh_product.cc`) is the single config entry — common code must not include any board's `config.h`. Layering bottom-up: `wzh_flash` (JEDEC runtime auto-detect over `w25n01gv` NAND / `w25q256` NOR drivers) → `wzh_audio_store` (PNDA TOC v1/v2 read + v3 dual-slot CRC write, streaming, power-fail safe) → `wzh_audio_player` (feeds AudioService, playback gating) → `wzh_audio_hub` (background register/activate/check-in/sync; server from Kconfig `WZH_AUDIO_SYNC_SERVER`, NVS `audio_hub/server` overrides) → `wzh_mcp_tools` (flash-audio management tools).
- **Board-specific code** (`*_power.cc`, `*_controller.cc`, board `.cc`): power latch GPIO7, analog power key GPIO6, servo rail GPIO4, battery GPIO3, ES8311 codec. Shutdown sequence order matters: stop local sounds → shutdown chime (~700ms play-out) → servos to safe pose → cut servo rail → drop latch.
- **Branded provisioning portal**: `components/esp-wifi-connect` is an in-repo fork of 78/esp-wifi-connect 3.2.2 taken over via `override_path` in `main/idf_component.yml` (don't touch `managed_components/`). Only WZH boards compile the branded templates `assets/wifi_configuration_branded.html.in` / `wifi_configuration_done_branded.html.in` (CMake `configure_file @ONLY`, `@WZH_*@` placeholders); all other boards embed the upstream pages. Per-product brand tokens (name/emoji/accent colors, set in that component's `CMakeLists.txt`) are the only thing that differs between the three products' pages.
- **Reference project** (read-only porting source — never modify): `/home/wzh/program/ESP32/zylz_project/final/newWebServer_Project/` (latest of three copies). Its `auto_run.cc` and per-product code are the behavioral reference for the remaining migration phases (phase 7-8 motion work, especially dinosaur V3.3: 29 variants / 5 scenes / 4 idle).

## Pitfalls learned in this repo

- `main/CMakeLists.txt` gets re-included by IDF's component script mode, where `file(GLOB ... CONFIGURE_DEPENDS)` is a hard error. Use a plain GLOB and drive rebuilds through custom-command `DEPENDS` (see `COMMON_SOUNDS` / the `lang_header` target).
- New embedded sounds go in `main/assets/common/*.ogg`; `scripts/gen_lang.py` regenerates the gitignored `main/assets/lang_config.h` and names symbols after the uppercase file base (`power_on.ogg` → `Lang::Sounds::OGG_POWER_ON`). `AudioService::PlaySound` has no gating; `AudioPlayer::PlayIndex` refuses while writing or non-idle playback — use the latter to sequence flash sounds behind chimes.
- Generated/vendor paths are off-limits for hand edits (AGENTS.md): `build/`, `managed_components/`, `sdkconfig*`, `main/assets/lang_config.h`, generated mmap headers.
