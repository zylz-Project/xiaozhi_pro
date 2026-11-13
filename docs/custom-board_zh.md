# 产品板级维护

本仓库维护三款 ESP32-S3 产品：`wzh_crawlPanda`、`wzh_dinosaur` 和 `wzh_tailRedPanda`。修改前先阅读对应产品目录的 `AGENTS.md`、`README.md`、`config.h`、板级源文件和 `config.json`。三款共用的 WZH 实现位于 `main/boards/common/wzh/`。

板型配置是一个联动链：

`config.json` → `scripts/build.py` → `main/Kconfig.projbuild` → `main/CMakeLists.txt` → 所选板级源文件与 `config.h`。

每次构建必须只导出一个 `DECLARE_BOARD(...)`。板型身份和变体名称影响 OTA 兼容性。不得为了适配另一套硬件而修改现有产品的引脚、舵机方向、Flash 布局或产品身份；确需新增硬件时，应新增唯一身份的板型或变体，并同步更新选择链、芯片目标、分区配置、文档、测试和 CI。

使用 `python3 scripts/build.py --list-boards` 查看现有变体。加载 ESP-IDF v6.0.2 后，用 `python3 scripts/build.py <板级目录> --name <变体>` 构建。修改 WZH 共用代码时应构建三款产品并运行主机测试。编译成功不等于实机验收，电气、运动、音频、电源和网络行为必须在对应产品上验证。
