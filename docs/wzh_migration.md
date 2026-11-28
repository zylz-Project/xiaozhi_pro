# WZH 构建与兼容性

三款使用 ESP32-S3，建议使用 ESP-IDF v6.0.2。当前板型和变体以 `scripts/build.py --list-boards` 输出为准。主配置构建命令如下。

```sh
source /path/to/esp-idf/export.sh
python3 scripts/build.py wzh_crawlPanda --name wzh-crawlpanda
python3 scripts/build.py wzh_dinosaur --name wzh-dinosaur-270-servo
python3 scripts/build.py wzh_tailRedPanda --name wzh-tailredpanda
```

每款产品发布一份固件，三份都通过 ESP32-S3 原生 USB Serial/JTAG 输出应用日志。构建会更新当前目录的 `sdkconfig` 和 `build/`，并生成 `build/merged-binary.bin`。需要发布压缩包时加 `--zip`。

## 在 VS Code 切换已构建固件

ESP-IDF 工具栏的火焰图标执行「构建、烧录并监视」，会构建当前工作区所选板型。使用已有固件包时，运行下面的任务。

1. 用 USB 接入设备，通过 ESP-IDF 的串口选择命令设定端口。如果串口监视器正在运行，先关闭它。
2. 按 `Ctrl+Shift+P`，运行 **Tasks: Run Task（任务: 运行任务）**，选 **WZH 选择产品并烧录现成固件**。
3. 选与手上设备一致的型号。任务从 `releases/` 及其子目录取该型号最近生成的 ZIP，显示包名后校验并烧录。三款共用 ESP32-S3，设备型号以你选择的产品为准。

任务会校验包内板型和设备的 16 MB 分区布局，再写入应用、语音资源和启动槽选择。配网、密钥和外置音频保留。首次烧录或使用旧分区布局的设备，先用该型号的完整包初始化。生成新版本时在对应构建命令后加 `--zip`，完成后就能在任务中选用。

也可以在 ESP-IDF 终端指定某个历史包。传入 `--dry-run` 只检查包，适合先核对型号。

```sh
python3 scripts/flash_prebuilt.py wzh-tailredpanda --port /dev/ttyACM0 --archive releases/usb-logs/v2.4.1_wzh-tailredpanda.zip
```

## 设备标识

| 板型 | OTA `type` | 热点前缀 | Audio Hub `product_id` |
| --- | --- | --- | --- |
| [爬爬熊猫](../main/boards/wzh_crawlPanda/README.md) | `wzh-crawlpanda` | `CrawlPanda-` | `crawling-panda` |
| [小恐龙](../main/boards/wzh_dinosaur/README.md) | `wzh-dinosaur` | `Dino-` | `dinosaur` |
| [摇尾小熊猫](../main/boards/wzh_tailRedPanda/README.md) | `wzh-tailredpanda` | `RedPanda-` | `tail-wagging-panda` |

OTA `name` 随所选构建变体。板级引脚与限位见各板 `config.h`，设备标识以 `config.json` 和 `wzh_product.cc` 为准。

## 内部固件与外置音频

- [内部 16 MB 分区表](../partitions/v2/16m.csv)为每个 OTA 槽分配 `0x430000`，资源分区从 `0x880000` 开始。旧设备若使用不同分区布局，需要整机重刷新的分区表和镜像。
- 三款默认关闭 `CONFIG_ENABLE_ONLINE_UPDATES`，仍会联网激活并获取对话服务配置。需要验证在线固件或资源升级时，使用明确启用该选项的构建。
- 外部音频 Flash 由 `wzh_flash` 根据 JEDEC ID 识别。`EF 40 19` 对应 W25Q256 NOR，`EF AA 21` 对应 W25N01GV NAND。实际型号以样机启动日志为准。
- `wzh_audio_store` 可读 PNDA v1、v2，写入 v3 时校验 CRC 和回读。NAND 在首个擦除块内轮换目录槽；NOR 使用首扇区的单槽。NAND 压实和 NOR 原地目录改写都有掉电窗口，需分别做断电恢复验证。
- Audio Hub 后台管理外置音频，播放共用 `AudioService`。接口、NVS 键和同步规则以 [Audio Hub 实现](../main/boards/common/wzh/wzh_audio_hub.cc)为准。

## 验证与历史

2026-11-26 的优化版本使用 ESP-IDF v6.0.1 构建通过五个变体，538 项主机测试通过。爬爬熊猫、小恐龙、摇尾小熊猫主配置的应用分区分别剩余约 22%、8%、19%。小恐龙的余量从约 158 KiB 增至约 343 KiB，主要来自简化唤醒词列表解析，避免链接整套 C++ 流和区域设置库。搭配样机逐项完成[整机验收](wzh/product/product_acceptance.md)。

[旧工程迁移记录](wzh/archive/2026-11/wzh_migration_2026-11-13.md)保留移植过程和当时的硬件、存储与接口假设。
