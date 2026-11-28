# 历史归档

归档保留当时的实验、问题定位、构建记录和验证证据，按原日期查找。旧报告中的“当前”“已完成”只对应记录当时；现行入口见 [WZH 文档导航](../README.md)。

| 目录 | 内容 |
| --- | --- |
| [2026年10月](2026-10/) | 早期整机审查、动作与音画优化、控制台方案和构建清单 |
| [2026年11月](2026-11/) | 人声打断与音质、语音引导、固件迁移、审查和网页方案 |

10月的 `review/` 保留原始检查、CSV和复现脚本，`images/` 保留页面截图。旧复现脚本用于历史取证，不保证适用于最新源码。

## 按主题查找

| 主题 | 历史记录 |
| --- | --- |
| 整机审查与固件 | [10 月整机审查](2026-10/wzh_product_audit_2026-10-15.md)、[10 月编译验证](2026-10/wzh_build_2026-10-21.md)、[11 月代码审查](2026-11/product_code_review_2026-11-18.md) |
| 动作和音画 | [音画编排与轨迹](2026-10/wzh_audio_choreography_2026-10-19.md)、[参考动作曲线](2026-10/wzh_reference_expression_2026-10-20.md) |
| 人声打断与音质 | [小熊猫实机诊断](2026-11/wzh_tailredpanda_barge_in_diagnostics_2026-11-03.md)、[防自打断复核](2026-11/wzh_tailredpanda_voice_guard_2026-11-04.md)、[官方音乐实机反馈](2026-11/wzh_tailredpanda_music_feedback_2026-11-04.md) |
| 语音引导 | [录音方案第二版](2026-11/onboarding_voice_plan_v2_2026-11-08.md)、[逐段台词 CSV](2026-11/onboarding_voice_plan_v2_2026-11-08.csv) |
| 固件迁移 | [WZH 迁移记录](2026-11/wzh_migration_2026-11-13.md) |
| 网页 | [开发者控制台方案](2026-10/wzh_console_solution_2026-10-19.md)、[手机端优化计划](2026-11/wzh_web_ui_optimization_plan_2026-11-16.md) |

## 已去重的产物

2026-11-26 删除独立的 `wzh_sound_motion_audit_2026-10-19.json`。该文件与[音画预览](../motion/audio_motion_preview.html)内嵌的 JSON 逐字节一致，可按[历史报告](2026-10/wzh_audio_choreography_2026-10-19.md#试听与复核)提取。

2026-11-07 清理时，以下7个JSON与保留文件逐字节一致，已删除重复副本，并把引用指向保留文件。独有的调研、验证结果和截图均保留。

| 删除的重复文件 | 保留文件 |
| --- | --- |
| `wzh_user_pairs_firmware_2026-10-20.json` | [完整清单](2026-10/wzh_broad_expression_firmware_2026-10-19.json) |
| `wzh_fluid_motion_firmware_2026-10-20.json` | [完整清单](2026-10/wzh_broad_expression_firmware_2026-10-19.json) |
| `wzh_voltage_touch_firmware_2026-10-20.json` | [完整清单](2026-10/wzh_broad_expression_firmware_2026-10-19.json) |
| `wzh_chat_button_firmware_2026-10-19.json` | [完整清单](2026-10/wzh_random_sound_firmware_2026-10-19.json) |
| `wzh_dialog_developer_firmware_2026-10-19.json` | [完整清单](2026-10/wzh_random_sound_firmware_2026-10-19.json) |
| `wzh_lively_motion_firmware_2026-10-19.json` | [完整清单](2026-10/wzh_random_sound_firmware_2026-10-19.json) |
| `wzh_exhibition_firmware_2026-10-19.json` | [完整清单](2026-10/wzh_random_sound_firmware_2026-10-19.json) |
