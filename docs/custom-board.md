# Product board maintenance

This repository maintains three ESP32-S3 products: `wzh_crawlPanda`, `wzh_dinosaur` and `wzh_tailRedPanda`. Start with the applicable product's `AGENTS.md`, `README.md`, `config.h`, board source and `config.json`. The shared WZH implementations live under `main/boards/common/wzh/`.

Board configuration is coupled:

`config.json` → `scripts/build.py` → `main/Kconfig.projbuild` → `main/CMakeLists.txt` → the selected board's source and `config.h`.

A build must export exactly one `DECLARE_BOARD(...)`. The board's reported identity and variant name affect OTA compatibility. Never change an existing product's pin assignments, servo direction, flash layout or product identity to accommodate different hardware. If a genuinely new hardware assembly is authorized, introduce a uniquely identified board or release variant and update every link in the selection chain, target, partition settings, documentation, tests and CI.

Use `python3 scripts/build.py --list-boards` to inspect supported variants. Source ESP-IDF v6.0.2 and build the affected variant with `python3 scripts/build.py <board-directory> --name <variant>`. Changes in shared WZH code require representative builds for all three products and the host suite. A successful build does not establish electrical, motion, audio, power or network correctness; test those on the corresponding physical product.
