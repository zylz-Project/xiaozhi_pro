# WZH 三款产品固件

[English](README.md) · [日本語](README_ja.md)

本仓库基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，维护三款 ESP32-S3 产品。三款支持本地触摸与语音口令、设备热点、联网对话和音色管理，小恐龙另支持 ML307 4G。

| 产品 | 板型目录 | 主构建名 |
| --- | --- | --- |
| [爬爬熊猫 · 小安](main/boards/wzh_crawlPanda/README.md) | `wzh_crawlPanda` | `wzh-crawlpanda` |
| [小恐龙 · 小龙](main/boards/wzh_dinosaur/README.md) | `wzh_dinosaur` | `wzh-dinosaur-270-servo` |
| [摇尾小熊猫 · 星宝](main/boards/wzh_tailRedPanda/README.md) | `wzh_tailRedPanda` | `wzh-tailredpanda` |

开发时优先使用 ESP-IDF v6.0.2。加载 SDK 的 `export.sh` 后，可运行 `python3 scripts/build.py --list-boards` 查看全部变体，再用 `python3 scripts/build.py <板型目录> --name <构建名>` 构建。主机测试入口为 `python3 scripts/run_host_tests.py`。

- [家庭快速上手](docs/wzh/product/family_quickstart.md)
- [WZH 构建、硬件、交互和网页文档](docs/wzh/README.md)
- [小智通用开发文档](docs/README.md)
- [仓库开发规则](AGENTS.md)
