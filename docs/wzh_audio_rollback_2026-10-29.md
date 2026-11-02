# 小熊猫回退普通版，保留唤醒词打断（2026-10-29）

用户反馈最新实验版没有正常语音输出，要求恢复之前版本，并保留已在配置中开启的
唤醒词打断。本次按该要求回退，不再继续修改直接人声打断实验。

## 回退内容

恢复本轮实验之前的源码基准：
`f7fd9b0`（完整提交号见烧录包 manifest.json）。所有17个已修改的跟踪文件经上次
构建源码SHA256核对，确认均为本轮改动后恢复；12个新增实验头文件、测试和记录先备份，
再移出当前源码／测试目录。实验构建 `wzh-tailredpanda-barge-in` 也已撤下。

Application、AudioService、AFE、ES8311、动作控制与既有测试恢复原代码，包括撤回：

- 普通人声直接打断、本地前缀判定及其迟到TTS门控。
- 软件DMA播放参考、额外回声路径／原始麦克风相关度搜索及PSRAM旁路队列。
- 本轮120Hz高通、峰值限制及相关输出复位接口；使用原硬件音量映射。
- 本轮动作循环延迟诊断。

保留用户已选的 `CONFIG_WAKE_WORD_DETECTION_IN_LISTENING=y`，写入普通小熊猫
`main/boards/wzh_tailRedPanda/config.json`，避免规范构建重新生成配置时丢失。
唤醒词仍为“你好星宝”，使用原AFE WakeNet路径；正在播报时通过唤醒词打断，
不再通过本轮新增的普通人声自动截断。

回退后C/C++与基准逐字一致；跟踪文件仅剩板级JSON中这一项配置差异。
没有修改生成目录中的配置；最终sdkconfig由规范构建生成并核对。

## 备份与历史固件

回退前工作树patch、全部相关文件及用户原sdkconfig保存在：
`/tmp/wzh-audio-rollback-20261029-ne03x417`。
先前实验固件留在 `dist/wzh-tailredpanda-raw-echo-20261029/`，已在该manifest中标为
撤回并指向本次普通版；旧固件文件未覆盖。用户这次提供的日志中，两次
`Barge confirmed` 后都马上发生 `Abort speaking`，属于本轮撤回的本地打断路径。

## 构建与烧录

使用本机已安装ESP-IDF v6.0.1；首选v6.0.2未安装。规范构建：

```sh
source /home/wzh/.espressif/v6.0.1/esp-idf/export.sh
python3 scripts/build.py wzh_tailRedPanda --name wzh-tailredpanda --zip
```

普通版规范合并包为
[v2.4.1_wzh-tailredpanda.zip](../releases/v2.4.1_wzh-tailredpanda.zip)。
已有设备优先使用[分区包与保留配网烧录说明](../dist/wzh-tailredpanda-rollback-20261029/README.md)，
写入app、配套assets和otadata启动槽，保留既有布局的NVS。不要继续刷旧的barge-in实验包。

构建、主机测试、源码和固件校验见
[manifest.json](../dist/wzh-tailredpanda-rollback-20261029/manifest.json)。
191项主机测试通过（70.455秒），日志 `/tmp/wzh-audio-rollback-tests-20261029.log`。
普通小熊猫与C3／4G构建均通过，WebSocket和MQTT/UDP参与编译；最终build为普通小熊猫。

| 构建 | app字节数 | 最小app分区剩余 |
| --- | ---: | ---: |
| 普通小熊猫，保留唤醒词打断 | 2,834,320 | 1,294,448 |
| C3／4G回归 | 1,603,600 | 1,476,592 |

确认最终sdkconfig包含 `CONFIG_USE_AFE_WAKE_WORD=y` 和
`CONFIG_WAKE_WORD_DETECTION_IN_LISTENING=y`；实验特征符号／日志不参与最终链接，
匹配唤醒／离线口令资源、分区数据与合并镜像一致，只有一个板工厂；`git diff --check`通过。
未连接样机、未烧录或试听；仍需实际验证完整播报、音乐、唤醒词打断、触摸／待机动作
和重连。回退构建成功不能代替设备验证。
