# WZH 三款语音伙伴固件

本仓库维护三款基于 ESP32-S3 的产品：爬爬大熊猫、摇尾小熊猫和小恐龙。固件保留 XiaoZhi 共用的音频、状态机、网络协议和设备接口；三款产品各自拥有独立的板级配置、硬件引脚和 OTA 身份。

| 产品 | 板级目录 | 默认构建名称 |
| --- | --- | --- |
| 爬爬大熊猫 | [wzh_crawlPanda](main/boards/wzh_crawlPanda/README.md) | `wzh-crawlpanda` |
| 小恐龙 | [wzh_dinosaur](main/boards/wzh_dinosaur/README.md) | `wzh-dinosaur` |
| 摇尾小熊猫 | [wzh_tailRedPanda](main/boards/wzh_tailRedPanda/README.md) | `wzh-tailredpanda` |

产品还保留打断、VAD 与诊断等现有变体；用构建脚本列出完整名称。不同产品的固件不可互刷。

## 构建与测试

优先使用 ESP-IDF **v6.0.2**。先加载对应环境，再运行：

```sh
source /path/to/esp-idf/export.sh
idf.py --version
python3 scripts/build.py --list-boards
python3 scripts/build.py wzh_crawlPanda --name wzh-crawlpanda
python3 scripts/build.py wzh_dinosaur --name wzh-dinosaur
python3 scripts/build.py wzh_tailRedPanda --name wzh-tailredpanda
python3 -m unittest discover -s scripts/tests -v
```

构建脚本会更新本地 `sdkconfig` 与 `build/` 状态；切换产品后不要把旧构建目录当成上一产品的产物。构建成功只能证明编译通过，录音、播放、蓝牙、配网、动作和电源保护仍需对应产品的实机验收。

## 项目资料

- [三款产品文档导航](docs/wzh/README.md)；[硬件与存储迁移记录](docs/wzh_migration.md)。
- [微信小程序及 BLE 协议](wzh-miniprogram/README.md)。
- [音频架构](main/audio/README.md)、[WebSocket](docs/websocket.md)、[MQTT/UDP](docs/mqtt-udp.md) 和 [MCP](docs/mcp-protocol.md)。
- [ESP-IDF 迁移说明](docs/esp-idf-6-migration.md)与 [代码风格](docs/code_style.md)。

产品专用实现位于 `main/boards/wzh_*`，三款共用硬件和服务位于 `main/boards/common/wzh/`。不要将某个产品的引脚或运动方向复制到另一款产品。

本项目基于 [XiaoZhi ESP32](https://github.com/78/xiaozhi-esp32) 开源固件，保留原项目的许可证和必要的共用实现；详见 [LICENSE](LICENSE)。
