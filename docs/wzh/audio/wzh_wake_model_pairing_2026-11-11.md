# WZH 唤醒模型与板型匹配

2026-11-16 起，唤醒模型由 **板型本身决定**：`main/CMakeLists.txt` 的板型分支给出该产品的默认模型，`build_default_assets.py --wakenet_model` 在打包 `generated_assets.bin` 时强制使用它。因此切换 `Target Board` 后不需要再单独设置唤醒词，残留的旧选择也会被忽略。

| 板型 | 唤醒词 | 打包模型 |
| --- | --- | --- |
| WZH Crawl Panda | 你好小安 | `wn9_nihaoxiaoan_tts2` |
| WZH Dinosaur | 小龙小龙 | `wn9_xiaolongxiaolong_tts` |
| WZH Tail Red Panda | 你好星宝 | `wn9l_ni3hao3xing1bao3_tts3` |

## 为什么不能只靠 Kconfig

`managed_components/` 下的 esp-sr Kconfig 不能修改，而实验表明 Kconfig 的 `depends on` 不会清除 `sdkconfig` 中已有的 `CONFIG_SR_WN_*=y`，`select` 也只能追加、不能关闭旧值。因此仅在 Kconfig 层做「板型联动唤醒词」无法保证唯一模型，必须由打包步骤强制指定。

`CONFIG_SR_WN_*` 只被资产打包脚本消费；运行期模型来自打包进 assets 分区的 `srmodels.bin`，与 sdkconfig 里的布尔值无关。这也是在打包层锁定模型就足够的原因。

## 2026-11-11 的故障复盘

当时工作区生成的固件包含大熊猫板代码，`sdkconfig` 和 `generated_assets.bin` 却选择了小熊猫的 `wn9l_ni3hao3xing1bao3_tts3`（你好星宝），导致烧入后说「你好小安」无响应。

2026-11-16 再次出现同类现象：IDE 中把 `Target Board` 切成 Dinosaur，`sdkconfig` 也确实是 `CONFIG_BOARD_TYPE_WZH_DINOSAUR=y`，但上一块板留下的 `CONFIG_SR_WN_WN9L_NI3HAO3XING1BAO3_TTS3=y` 没有清掉。`wake_model_check.cmake` 在 cmake 配置阶段直接 `FATAL_ERROR`，恐龙固件从未编译成功，设备上运行的仍是之前烧入的摇尾小熊猫固件（日志中的 `RedPanda-0B21`、`PandaPower` 和「小武」自我介绍都是它）。

因此判断板型是否正确，不能只看烧到设备上的日志，还要核对 `build/xiaozhi.bin` 里的板型字符串与本次构建是否一致。

## 仍需注意

`wake_model_check.cmake` 会检测 `sdkconfig` 中是否还残留另一产品的专属模型，但只输出 CMake 警告，不再中断配置。此前的 FATAL 曾造成死锁：残留值让配置阶段失败，SDK Configuration editor 随之打不开，也就无法改掉那个值。打包已由板型锁定，因此残留值不会进入固件，警告仅作为提示。

显式开启 `WZH_ALLOW_CROSS_PRODUCT_WAKE_MODEL` 会关闭该提示。

打包层无法确认设备上实际刷入的应用和资源是否来自同一次构建；仍须在三台设备分别验证待机唤醒、在线对话、音色复刻、人声打断以及重启后唤醒。