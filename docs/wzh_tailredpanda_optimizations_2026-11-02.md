# 小熊猫固件：三批稳定性与音频优化（2026-11-02）

优化前已按用户要求保存整个工作区，并推送至
`zylz/feature/wzh-product-migration`，提交
`817dc4b`（`Save WZH audio and flash baseline before upstream optimizations`）。
这是本次优化的回退基线，包含原有扬声器保护、Flash 驱动和唤醒词打断配置。

官方对照为 2026-11-02 查询的 `78/xiaozhi-esp32` 主线
`a78ca19d5bc9a12ee0853b9f509530276d36aa03`；正式发布版为 v2.5.2。
本次在现有 2.4.1 产品分支选择性迁入修复，没有整体合并官方产品层或升级依赖。

## 三批改动

| 批次 | 实现 | 保留的产品约束 |
| --- | --- | --- |
| 联网与解析 | WebSocket hello 类型校验；无异常 OTA 版本解析；MQTT 端口校验和锁外 UDP 发送；释放 custom 消息序列化字符串；修复 OGG 零长度终止分段 | WebSocket / MQTT-UDP 包格式、NVS 键、OTA 板型不变 |
| 音频内存 | 固定容量队列、直接存放 AudioTask、直接编码到包负载；重用解码转换源缓冲；AFE 处理栈优先 PSRAM 并保留内部 RAM 回退 | 96 压缩包 / 6 PCM 帧、录音测试完整容量、单 AFE 和单扬声器写入任务 |
| 播放与扬声器 | 滑动峰值检测；RMS 目标每 16 采样更新；提前限幅覆盖瞬态；输出任务执行 DSP 复位；同步复位重采样器并释放过期转换器；拒绝无效 Opus 格式 | 120Hz 高通、约 -1.5dBFS 峰值、最多 4dB RMS 衰减、24kHz 硬件率、120ms / 最长 80ms 预缓冲 |

官方参考修复：
[MQTT 锁](https://github.com/78/xiaozhi-esp32/commit/f5b495d)、
[OTA 解析](https://github.com/78/xiaozhi-esp32/commit/b2bb950)、
[OGG 尾包](https://github.com/78/xiaozhi-esp32/commit/eeae329)、
[固定队列](https://github.com/78/xiaozhi-esp32/commit/ac6deed)、
[AFE 内存](https://github.com/78/xiaozhi-esp32/commit/7aa9266)。

固定队列仍支持完整的 166 包录音测试回放，不会按实时会话 96 包预算截断。
输入、网络发送和播放仍保留各自的等待/丢弃策略。服务器 AEC 时间戳队列保留最新 3 项。
Opus 参数校验和重采样复位位于两种传输共用的 AudioService。

小熊猫的引脚、板型、舵机控制、动作库、触摸/姿态、Flash 音效、网页管理、
Audio Hub、双击进入/退出、WakeNet + MultiNet 路由均保持原实现。
“你好星宝”及 `CONFIG_WAKE_WORD_DETECTION_IN_LISTENING=y` 保留；未重新引入普通人声
直接打断实验，也没有改动现有 AFE/AEC 参数。最新版 Pong 回调依赖新的网络组件 API，
本次保留现有网络依赖，未迁入该 API 或新增主动通知功能。

## 验证与产物

使用本机已有 ESP-IDF v6.0.1；首选 v6.0.2 未安装。

- 全量主机测试 **204 项通过**，68.664 秒；日志 `/tmp/wzh-tail-optimization-20261102/tests-final.log`。
- 新增/扩展测试执行实际 C++：非法版本与溢出、异常 hello 字段、UDP 发送锁与生命周期、custom 消息释放、OGG 所有分块边界、固定队列完整录音转移、解码格式与重采样生命周期、多个采样率下的瞬态/分片/会话复位。
- 小熊猫和 C3/4G 均已通过规范构建，WebSocket 与 MQTT/UDP 均参与编译。
- C/C++ 格式检查和 `git diff --check` 通过。
- 最终 build / sdkconfig 为小熊猫，唯一板工厂；合并镜像每段与独立文件一致，ZIP 与合并文件一致；assets 校验和、唤醒模型、81 条本地短语及分区容量已核对。
- 切换芯片会重建 managed_components，最终全量测试在构建完成后单独运行，避免依赖目录重建影响主机测试。

| 构建 | app 字节数 | 最小 app 分区剩余 |
| --- | ---: | ---: |
| wzh-tailredpanda | 2,837,648 | 1,291,120 |
| xmini-c3-4g | 1,605,568 | 1,474,624 |

产物目录：[dist/wzh-tailredpanda-optimized-20261102](../dist/wzh-tailredpanda-optimized-20261102/README.md)。
[完整 ZIP](../dist/wzh-tailredpanda-optimized-20261102/v2.4.1_wzh-tailredpanda.zip)、
[保留配网烧录说明](../dist/wzh-tailredpanda-optimized-20261102/README.md)、
[文件/源码 SHA256 与验证清单](../dist/wzh-tailredpanda-optimized-20261102/manifest.json)。
ZIP SHA256：`462dbce6cf29c358a89cea4437bec6376a0d6975c888a895c8f74c5a26d89132`。

构建日志：`/tmp/wzh-tail-optimization-20261102/build-tail-final.log`、
`/tmp/wzh-tail-optimization-20261102/build-c3-4g-final.log`。

## 实机验收

本次未连接设备、未烧录和试听。编译与主机测试不能证明实机音质、识别率或延迟改善。
仍需确认：

- 连续播报、首尾完整、故事/音乐与 Flash 音效听感。
- 唤醒词打断、双击退出、停止与重连后无旧语音重放。
- 专用唤醒词和离线口令，适用的 VAD / AEC 行为。
- 三轴动作、触摸/姿态、归中卸力与同时播放时的供电。
- 长时间联网/离线切换、队列状态和内存是否稳定。

固件仍报告 2.4.1，适合手动烧录或显式升级；不会仅因本次回补而触发版本号自动升级。
现有设备若使用一致的 16MB 分区，可按打包说明仅更新 app、配套 assets 和启动槽，保留 NVS。
