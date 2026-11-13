# ESP-IDF 6 与 WZH 产品兼容性

仓库当前维护三款 ESP32-S3 产品，首选 SDK 为 **ESP-IDF v6.0.2**。SDK 版本、板型、发布变体和唤醒词模型共同决定产物；不要把其他板型的固件刷入现有设备。

| 产品目录 | 默认变体 | 其他现有变体 |
| --- | --- | --- |
| `wzh_crawlPanda` | `wzh-crawlpanda` | `wzh-crawlpanda-barge-in` |
| `wzh_tailRedPanda` | `wzh-tailredpanda` | `wzh-tailredpanda-barge-in` |
| `wzh_dinosaur` | `wzh-dinosaur` | `wzh-dinosaur-barge-in`、`wzh-dinosaur-vadnet-test`、`wzh-dinosaur-official-asr-probe` |

使用 `python3 scripts/build.py --list-boards` 查询当前实际配置。`config.json` 中的板型、版本和配置才是构建输入；选择特定变体后执行 `python3 scripts/build.py <目录> --name <变体>`。依赖由 ESP-IDF 组件管理器按清单解析；如未固定完整依赖快照，不应将不同环境的构建描述为逐字节可复现。

构建前后使用 `python3 -m unittest discover -s scripts/tests -v` 验证主机测试。构建和主机测试不等于硬件验收：音频采集与播放、唤醒和打断、BLE、Wi-Fi、掉电恢复、动作方向、电池和关机都需在对应产品上复测。BLE/网络共享代码修改应覆盖三个产品及其受影响变体。

涉及 SDK 迁移时，保留产品 NVS 键、音频存储格式、OTA 身份、板级引脚与舵机方向；若必须更改，先设计兼容或迁移方案。其他通用 SDK 配置及组件管理规则以仓库构建配置、[ESP-IDF 文档](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/)和产品目录的说明为准。
