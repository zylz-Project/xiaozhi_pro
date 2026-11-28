# WZH 三款产品文档

| 要做什么 | 从这里开始 |
| --- | --- |
| 使用设备 | [家庭快速上手](product/family_quickstart.md) · [语音口令](audio/voice_commands.md) · [音色复刻](audio/voice_cloning.md) |
| 构建、烧录与迁移 | [构建与兼容性](../wzh_migration.md) · [仓库开发规则](../../AGENTS.md) |
| 修改板级硬件 | [爬爬熊猫](../../main/boards/wzh_crawlPanda/README.md) · [小恐龙](../../main/boards/wzh_dinosaur/README.md) · [摇尾小熊猫](../../main/boards/wzh_tailRedPanda/README.md) |
| 排查感应、保护与 4G | [感应与保护](hardware/sensing_and_protection.md) · [硬件复核](hardware/hardware_review.md) · [ML307](hardware/dinosaur_ml307_2026-11-15.md) |
| 修改动作 | [动作库与运行机制](motion/motion_libraries.md) · [声音与动作预览](motion/audio_motion_preview.html) |
| 修改音频或首次播报 | [音频架构](../../main/audio/README.md) · [播报制作与校验](audio/system_prompts.md) · [播报试听](audio/system_prompts_preview.html) |
| 修改配网页和开发者台 | [设备网页](web/README.md) |
| 验收或查日志 | [整机验收](product/product_acceptance.md) · [运行日志](product/运行日志说明.md) |
| 查看角色或历史记录 | [角色提示词](product/character_roles.md) · [历史归档](archive/README.md) |

`data/` 保存构建之外的脚本输入和校准资料。按任务查阅 [参考动作清单](data/reference_motion_manifest.json)、[音画素材](data/sound_pack.json)、[手调配对](data/user_pairs.json) 或 [小熊猫音频校准](data/tailredpanda_audio_calibration.json)。

更新当前行为时改对应主题的说明。过往的设计草案、实测及评审记录从归档入口查找。
