# WZH 三款产品文档

大熊猫小安、小熊猫星宝、恐龙小龙共用此导航。首次使用语音是设计稿；音频与动作说明中的软件验证、构建成功和实机验收分别记录，不代表已经发布。

## 产品与录音

| 文档 | 用途 |
| --- | --- |
| [首次使用播报](product/onboarding_voice_plan.md) · [录音 CSV](product/onboarding_voice_plan.csv) | 本轮优化后的完整文案，按触发时机生成音频 |
| [家庭快速上手](product/family_quickstart.md) | 日常触摸、口令、配网、聊天和按键操作 |
| [角色设定](product/character_roles.md) | 小安、星宝、小龙的人设与对话提示词 |
| [整机验收表](product/product_acceptance.md) | 装机与家庭试用时逐项确认 |
| [功能总览参考](product/product_features.md) · [交互机制参考](product/pet_interaction.md) | 较早的详细说明，部分参数和行为需要结合当前代码核对 |

## 音频

| 文档 | 用途 |
| --- | --- |
| [本地播报制作](audio/system_prompts.md) · [试听页面](audio/system_prompts_preview.html) | 现有固定音包的来源、生成、导入与验证 |
| [响度恢复](audio/wzh_loudness_recovery_2026-11-07.md) | 针对音量100仍偏小的补偿及保护方案 |
| [满音量音质修正](audio/wzh_volume100_audio_2026-11-07.md) | 功放增益、余量与失真调查；响度后续调整见上一条 |
| [进入与退出对话提示音](audio/wzh_conversation_sounds_2026-11-07.md) | 对话提示的区分、取消及验证 |
| [大熊猫与恐龙人声打断](audio/wzh_voice_barge_in_2026-11-07.md) | 构建入口、软件验证和待实机确认事项 |
| [音频架构](../../main/audio/README.md) | 当前采集、播放、AEC及打断链路 |

## 动作与硬件

| 文档 | 用途 |
| --- | --- |
| [动作机制参考](motion/motion_libraries.md) | 执行路径和动作库结构，包含历史参数 |
| [动作连贯性调整](motion/wzh_fluid_motion_2026-11-05.md) | 插值、PWM和轨迹对照 |
| [动作变化与关机](motion/wzh_motion_variety_shutdown_2026-11-06.md) | 降低重复、关机播报和保护边界 |
| [声音与动作预览](motion/audio_motion_preview.html) | 离线查看配对与轨迹；声音还需本地音频源 |
| [硬件复核](hardware/hardware_review.md) | 主板接口、芯片与证据 |
| [感应与保护](hardware/sensing_and_protection.md) | 传感、关键词、电池和舵机保护 |
| [电池与关机调研](hardware/wzh_battery_shutdown_research_2026-11-06.md) | 分层保护策略与调研依据 |
| [迁移真值表](../wzh_migration.md) | 产品身份、引脚、存储格式和迁移检查 |

## 数据与历史

`data/` 是脚本输入，不是待清理的临时文件：

- [参考动作清单](data/reference_motion_manifest.json)
- [声音配对清单](data/sound_pack.json)
- [手调音画配对](data/user_pairs.json)
- [小熊猫音频校准](data/tailredpanda_audio_calibration.json)

[历史归档](archive/README.md)保留此前的调试、实验、构建和复现证据。没有把旧报告中的参数、完成状态自动当作当前结论。

[返回全部文档](../README.md)
