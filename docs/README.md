# 文档导航

三款 WZH 产品从 [WZH 文档导航](wzh/README.md)进入。下面保留小智通用开发文档，原路径不变。

## 常用入口

| 要做什么 | 文档 |
| --- | --- |
| 生成首次使用引导录音 | [播报文案](wzh/product/onboarding_voice_plan.md) · [录音 CSV](wzh/product/onboarding_voice_plan.csv) |
| 查看三款产品怎么使用 | [家庭快速上手](wzh/product/family_quickstart.md) |
| 查看音质、响度、对话提示与人声打断 | [WZH 音频资料](wzh/README.md#音频) |
| 查看硬件与保护 | [硬件复核](wzh/hardware/hardware_review.md) · [感应与保护](wzh/hardware/sensing_and_protection.md) |
| 查找以前的调试、对比和构建结果 | [历史归档](wzh/archive/README.md) |

## 小智通用开发文档

| 主题 | 文档 |
| --- | --- |
| SDK 与兼容性 | [ESP-IDF 6 迁移](esp-idf-6-migration.md) |
| 自定义开发板 | [中文](custom-board_zh.md) · [English](custom-board.md) |
| 代码风格 | [中文](code_style_zh.md) · [English](code_style.md) |
| WebSocket | [中文](websocket_zh.md) · [English](websocket.md) |
| MQTT/UDP | [中文](mqtt-udp_zh.md) · [English](mqtt-udp.md) |
| MCP 协议 | [中文](mcp-protocol_zh.md) · [English](mcp-protocol.md) |
| MCP 使用 | [中文](mcp-usage_zh.md) · [English](mcp-usage.md) |
| BluFi 配网 | [中文](blufi_zh.md) · [English](blufi.md) |
| 字形推送 | [中文](glyph-push_zh.md) · [English](glyph-push.md) |
| WZH 迁移真值表 | [硬件、格式与迁移记录](wzh_migration.md) |

## 文件放在哪里

- `wzh/product/`：产品使用、角色与首次播报设计。
- `wzh/audio/`：当前音频制作、响度与打断说明。
- `wzh/motion/`：动作机制、近期调整与预览。
- `wzh/hardware/`：硬件复核、感应和电源保护。
- `wzh/data/`：脚本和测试使用的数据，不能当作临时报告删除。
- `wzh/archive/`：按月保存的历史调试、构建和对比证据。

`v0/`、`v1/` 与通用协议图片保留在原处，供现有 README 和开发板资料引用。后续更新现有主题文档；一次性实验和构建记录放入归档，避免继续堆在根目录。
