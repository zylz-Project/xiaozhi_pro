# WZH 三款产品的 WakeNet 唤醒

更新日期：2026-10-15。

| 产品 | 说出的唤醒词 | ESP-SR 模型 | 构建名 |
| --- | --- | --- | --- |
| 恐龙 | **小龙小龙** | `wn9_xiaolongxiaolong_tts` | `wzh-dinosaur` |
| 爬爬熊猫 | **你好小安** | `wn9_nihaoxiaoan_tts2` | `wzh-crawlpanda` |
| 摇尾小熊猫 | **你好星宝** | `wn9l_ni3hao3xing1bao3_tts3` | `wzh-tailredpanda` |

## 如何开始对话

完成配网和云端激活后，说本款唤醒词，听到提示音再说问题。例如恐龙先说“小龙小龙”，
随后说“讲一个恐龙故事”。后续聆听、语音回复、说话中再唤醒打断，继续使用小智原有流程。
按键仍可切换聊天；触摸、姿态和人体感应仍触发本地宠物回应。

识别唤醒词本身不需要网络；开放式聊天需要网络及服务就绪。配网/激活阶段保留本地互动入口。
“醒醒”恢复本地宠物活动；本款专用唤醒词恢复活动并在服务就绪时进入聊天。
停止、休息恢复后，欠压、过热、落体等保护仍然有效。

## 保留的离线交互

“开心、撒娇、吃东西、睡觉、摇摇头、向左看、向右看、停下、别动、安静、晚安、醒醒”
这 12 条短语继续由 MultiNet7 在本地识别，无需先说唤醒词。
传感器回应、大幅平滑动作、动作结束释放舵机和安静等待互动的产品策略继续保留。
本轮没有更改硬件引脚、舵机限位和电池/温度保护阈值。

## 代码和配置变化

- 三款从 `USE_CUSTOM_WAKE_WORD` 改为 `USE_AFE_WAKE_WORD`；每个构建条目的
  `default_wake_word` 指定一个不同的专用模型，构建脚本自动清除默认“你好小智”模型，
  并校验 Kconfig 是否接受新模型。命令行 `--wake-word` 可显式覆盖产品默认值。
- 资源脚本同时打包 WakeNet 与中文 MultiNet7；MultiNet 词表只含本地口令，
  不再加入原先的“你好小智”自定义唤醒短语。
- `AfeAudioEngine` 按配置选择主唤醒引擎；WakeNet 与 MultiNet 共用一个 AFE 和采集链路。
  WakeNet 检出时优先处理，并清理尚未完成的本地识别状态。本地结果仍经
  `Board::HandleLocalVoiceCommand()` 消费，不建立云端会话。
- 本地模式会过滤旧资源里的非 `local:*` 短语。识别回调释放模型输入锁后再通知应用，
  避免立即停止/重新开启识别时锁住；停止后的部分采样和模型状态会清理。
- 本地口令阈值为 `WZH_OFFLINE_COMMAND_THRESHOLD=50`，独立于 WakeNet。
  `SEND_WAKE_WORD_DATA=n` 保持原有设置，普通唤醒后提示音再录音；本地命令不分配额外的唤醒上传缓存。

WakeNet9/9l 在 S3 上的支持见
[Espressif 官方文档](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html)，
MultiNet 使用 AFE 输出的接口见
[命令识别文档](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/speech_command_recognition/README.html)。

## 构建与更新

```sh
python3 scripts/build.py wzh_dinosaur --name wzh-dinosaur
python3 scripts/build.py wzh_crawlPanda --name wzh-crawlpanda
python3 scripts/build.py wzh_tailRedPanda --name wzh-tailredpanda
```

使用配套的 app 和 assets。本次改变了识别模型，仅更新 app 或云端角色名不能完成切换。
完整内部 Flash 合并镜像同时包含 app 与 assets；外部动物声音 Flash 独立维护。
没有为“你好熊猫”“你好小狸”等任意中文名字训练新 WakeNet 模型，也不支持在对话中即时学习新唤醒词。

## 验证与实机验收

主机测试覆盖双模型同时存在时的主引擎选择、普通 WakeNet/自定义 MultiNet 兼容、
本地口令分流、唤醒优先级、停止后重新监听、资源打包以及命令行覆盖产品默认模型。
82 项主机测试通过；三款产品和普通 S3/C3 板型共 5 个完整构建通过，使用本机 ESP-IDF v6.0.1
（项目首选 v6.0.2 未安装）。4 个本轮触及 C/C++ 文件格式及 git diff 检查通过。
从三款实际镜像解析确认各自唯一 WakeNet 模型、中文 MultiNet7/FST 和 12 条本地短语；
资源校验和、合并镜像的 app/资源偏移与 SHA-256 均通过。
产物见[固件包](../dist/wzh-wakenet-20261015/README.md)，记录见[验证结果](review/wzh_wakenet_validation_2026-10-15.json)。

尚未烧录样机。逐产品实机检查儿童/青少年发音和距离、背景聊天误唤醒、播放中唤醒/打断、
舵机噪声与 AEC、断网/重连、无网络的 12 条本地口令，以及长时间运行的堆和任务余量。
同时检测 WakeNet 与 MultiNet 的实时负载只能在样机上确认；构建通过不等于声学效果验收。
