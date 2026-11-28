# XiaoZhi firmware for WZH products

[中文](README_zh.md) · [日本語](README_ja.md)

This fork of [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) contains the ESP32-S3 firmware for three WZH toys. They share local touch and voice interaction, the device hotspot, XiaoZhi cloud conversation, and the audio stack. Dinosaur can switch between Wi-Fi and ML307 cellular networking.

| Product | Board directory | Main build name |
| --- | --- | --- |
| [Crawl Panda](main/boards/wzh_crawlPanda/README.md) | `wzh_crawlPanda` | `wzh-crawlpanda` |
| [Dinosaur](main/boards/wzh_dinosaur/README.md) | `wzh_dinosaur` | `wzh-dinosaur-270-servo` |
| [Tail Red Panda](main/boards/wzh_tailRedPanda/README.md) | `wzh_tailRedPanda` | `wzh-tailredpanda` |

Use ESP-IDF v6.0.2 when available. Source its `export.sh`, then run `python3 scripts/build.py --list-boards`. Build with `python3 scripts/build.py <board-directory> --name <build-name>`. Run host tests with `python3 scripts/run_host_tests.py`.

- [WZH product documentation](docs/wzh/README.md)
- [Shared XiaoZhi development documentation](docs/README.md)
- [Repository guidance](AGENTS.md)
- [License](LICENSE)
