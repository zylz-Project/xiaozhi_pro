# WZH 三产品迁移矩阵（小智固件）

> 基线：`test_xiaozhi_Final` @ `df09992`，参考工程：`/home/wzh/program/ESP32/zylz_project/final/newWebServer_Project/`（下称 `BASE`）。
> 本文档是迁移的唯一对照表：每个功能完成并在真机验证后打勾。

## 0. 范围决定（2026-09-04）

- **不做**产品 Web 控制台（阶段 5 取消）：设备上的 HTTP 服务只保留小智自带配网门户（`esp-wifi-connect`）。
- Flash 音频内容的来源与入口：
  - 主途径：**Audio Hub 后台静默同步**（注册/激活/清单/下载）。
  - 补充途径：**小智 MCP 工具**（列表 / 试听 / 删除 / 擦除 / 手动同步 / 状态查询）。
- 老工程 AI 对话（`chat.cc`、`realtime_ws.c`、`ws_auth.c`、`chat_cert.h`）与独立 Wi-Fi 栈、独立 I2S 播放任务**一律不迁**，由小智 `Application`/`Protocol`/`WifiManager`/`AudioService` 代替。

## 1. 产品对应与身份

| 小智板型 | 构建名 / OTA type | 参考工程 | 配网热点前缀 | Audio Hub product_id |
|---|---|---|---|---|
| `wzh_crawlPanda` | `wzh-crawlpanda` / `wzh-crawlpanda` | `BASE/crawlPanda` | `CrawlPanda-XXXX` | `crawling-panda` |
| `wzh_dinosaur` | `wzh-dinosaur` / `wzh-dinosaur` | `BASE/dinosaur` | `Dino-XXXX` | `dinosaur` |
| `wzh_tailRedPanda` | `wzh-tailredpanda` / `wzh-tailredpanda` | `BASE/tailRedPanda` | `Panda-XXXX` | `tail-wagging-panda` |

## 2. 硬件真值表（三产品共用部分）

所有三款硬件的电源、音频、外部 Flash 引脚完全一致；只有舵机数量/引脚/限位不同。

| 项目 | 值 | 备注 |
|---|---|---|
| 电源锁存 | GPIO7，高电平保持 | 拉低断电 |
| 电源键 | GPIO6（ADC1_CH5 模拟量），原始值 < 1241 ≈ 按下 | 长按 1500ms 关机（爬爬 2000ms → 小智统一 1500ms），开机需先释放一次才武装 |
| 舵机电源轨 | GPIO4 | 关机序列：回安全位 → 切舵机电 → 松锁存 |
| 电池 | GPIO3（ADC1_CH2），分压 2k/4.7k ×2 | 3200mV=0%，4200mV=100%，32 次均值 + IIR |
| ES8311 | I2C SDA=2 SCL=38，I2S WS=13 BCLK=48 DOUT=46 DIN=14，MCLK=NC | 小智侧 24k 入 / 24k 出；老工程是 48k |
| 外部 Flash SPI | CS=10 CLK=9 MOSI=47 MISO=21（SPI2_HOST） | 型号待真机 JEDEC 确认，见 §3 |

### 舵机（各产品独立）

| 产品 | 轴 / GPIO / 默认角 / 限位 |
|---|---|
| 爬爬熊猫 | head=17 (90, 8~172)；left_arm=15 (90, 8~172，爬行 180→0)；right_arm=16 (90, 8~172，爬行 0→180) |
| 恐龙 | head_turn=15 (90)；neck_lean=16 (90)；neck_tilt=17 (70)；tail_lr=18 (90)；tail_ud=8 (90, 55~180，真机校准前保守) |
| 摇尾小熊猫 | head=18 (90, 8~172)；tail_lr=15 (90, 6~174)；tail_ud=16 (90, 3~177；内部动作库 150 坐标输出映射) |

## 3. 外部 Flash 真相（迁移前必须解决）

| 证据 | 内容 |
|---|---|
| 老工程 `config.h` ×3 | `EXTERNAL_FLASH_TYPE 2` = **W25N01GV NAND**（三款全是 NAND） |
| 老工程注释 bug | tailRedPanda 注释把 NAND 写成 "NOR"；W25Q256 容量注释写 64MB（实为 32MB） |
| 小智板当前配置 | crawlPanda/tailRedPanda = NAND（含驱动）；**dinosaur = 声称 W25Q256 NOR（32MB、数据区 4096 起）且无驱动文件，用内联 SPI 只读** |
| 结论 | dinosaur 型号存疑。以**真机 JEDEC ID 为准**：`EF 40 19`=W25Q256 NOR；`EF AA 21`=W25N01GV NAND |

处理：共享 Flash 抽象层在初始化时**运行时 JEDEC 自动探测**，同时支持 NOR/NAND 两套驱动，日志打印探测结果；Kconfig 保留强制覆盖项供异常情况使用。

### PNDA TOC 格式（老工程真相）

- 魔数 `0x41444E50`（"PNDA" 小端），头部 12 字节：magic、version(u32)、count(u32)。
- v1 条目 80B：`name[64] offset(u32) size(u32) sample_rate(u32) duration_ms(u32)`；v2 追加 16B `category[16]`（"animal"/"ambient"），共 96B。最多 32 条。**无 CRC、无备份 TOC。**
- 布局：TOC 在第 0 个擦除单元前 4KB；文件数据从第 1 个擦除单元开始，按擦除粒度对齐追加。NAND 擦除=128KB（TOC 块=block0，数据从 block1）；NOR 擦除=4KB（老工程数据从 4096 起）。
- 写入：流式（program-unit 缓冲），先擦后写，断电残留数据无 TOC 条目 = 安全孤儿。
- NAND 特性：用户区 block 0..999（1000-1023 留给片内 BBM LUT）；页 2048B，整页编程保证 ECC 正确；坏块检查 + 页内禁止二次编程。

### 本工程新增 TOC v3（带保护，向后兼容 v1/v2）

- v3 头：magic、version=3、seq(u32 单调递增)、count、crc32（整个 TOC 区 CRC）+ 96B 条目（同 v2）。
- 双槽提交：TOC 区内滚动写入新版本并整页回读校验，校验通过后旧槽自然失效（按 seq 取最大有效者）；写入中途断电时旧版本仍有效。
- 兼容：无 v3 时回退读 v1/v2（数据区起始地址：NAND 仍为 128KB 不变；NOR 旧 4096 / 新 8192，检测后自动适配）。

## 4. Audio Hub 协议（老工程 `device_registry` + `sync_audio`，三产品逐字节一致）

- 设备 ID：`ZYLZ-` + 6 字节 STA MAC（十六进制）。
- NVS：namespace `audio_hub`（`api_token` / `claim_token` / `act_code`），namespace `audio_sync`（`revision`）。
- 端点（Bearer token 认证，除 register/activate）：
  - `POST /api/device/register` `{device_id, product_id, firmware_version}` → `{claim_token, activation_code}`
  - `POST /api/device/activate` `{device_id, claim_token}` → `{api_token}`；401/410 → 重新注册
  - `POST /api/device/v1/check-in` `{firmware_version}`，每 60s 心跳；401/403 → 回到激活轮询
  - `GET /api/device/v1/files` → `{revision, files:[{name, size, category}]}`；严格校验（必须 .opus、category 合法、无重复）
  - `GET /api/device/v1/download/<index>`（index = 清单数组下标，180s 超时）→ 流式写 Flash
- 同步规则：revision 相同跳过；等大小视为已存在；大小变化 = 删旧再追加；清单中没有的本地文件删除；全部成功才保存新 revision；单文件重试 3 次（2s/4s 线性退避）。
- 服务地址：老工程硬编码 `192.168.1.7:5000` → 本工程移入 **Kconfig**（默认值 + NVS 可覆盖），Hub 不可用时设备完整离线运行。

## 5. 功能迁移清单

图例：☐ 未开始 · ◐ 进行中 · ☑ 完成（含真机验证）

### 5.1 已有基线（本轮之前）

- ☑ 小智 ASR/LLM/TTS、协议、OTA（框架自带）
- ☑ ES8311 录放音（唯一 codec/I2S 实例，24k/24k）
- ☑ 舵机 LEDC 驱动 + 默认角/限位宏
- ☑ 电源锁存 + 模拟电源键（长按关机/双击对话）+ 电池采样
- ☑ 只读本地音频：NAND 驱动（crawlPanda/tailRedPanda）+ PNDA TOC v1/v2 读取 + OggDemuxer → AudioService 播放 + 伪包络
- ☑ 动作引擎骨架：关键帧插值 + 行为族洗牌 + TTS 包络联动 + MCP 工具（panda: 27 剪辑 / dino: 简化版 / tail: 28 剪辑）
- ☐ AP 前缀传入 WifiManager（本轮修复，见 5.2）

### 5.2 阶段 1：基线与修复

- ☑ AP 前缀修复（`wifi_ssid_prefix_` → `WifiManagerConfig`）
- ☑ 硬件真值表与 Flash 真相（本文档）
- ☐ 真机 JEDEC 读取确认恐龙 Flash 型号（固件启动日志已会打印 JEDEC）

### 5.3 阶段 2：共享产品基础层（`main/boards/common/wzh/`）

- ☑ `wzh_flash`：JEDEC 自动探测 + NOR(w25q256)/NAND(w25n01gv) 双驱动 + 读/写/擦统一 API
- ☑ `wzh_audio_store`：TOC v1/v2 读取 + v3 双槽 CRC 写入、流式写、删除、擦除、回读校验
- ☑ `wzh_audio_player`：三产品共用播放引擎（AudioService 投递 + 包络 + 互斥）
- ☑ 产品配置描述（`wzh_product.h`：名称/前缀/Hub 产品 ID/Flash 引脚的单一来源）
- ☑ 三板去重（移除两份 w25n01gv.c 拷贝；恐龙改用共享 Flash 层，首次具备 Flash 音频）

### 5.4 阶段 3：小智对话联动完善

- ☑ connecting 停本地声 + 注意姿态（`SetConversationActive`）；speaking 真实包络驱动（`SetOutputAudioLevelListener`）；打断即停（状态切换 + ResetDecoder）
- ☐ listening 轻微倾听微反应（VAD 驱动，随动作专项一起做）
- ☑ wifi_configuring / upgrading / activating 安静姿态（暂停自主行为与本地声音）
- ☑ 关机序列：先停本地声音，再回安全姿态，后断电
- ☑ 状态监听已接入（`AddDeviceStateChangeListener`，connecting/listening/speaking/audio_testing）

### 5.5 阶段 4：Flash 完整音频管理

- ☑ 流式写入 + 断电恢复（v3 双槽；NOR 单槽擦写窗口由 CRC 检出）
- ☑ 新增/替换/删除/全部擦除 + 写入后目录即时刷新
- ☑ OGG/Opus 头校验 + 逐块回读校验 + 写入期间拒绝本地播放
- ☑ NAND 坏块/ECC/整页编程；NOR 4KB 扇区/页编程

### 5.6 阶段 5：Web 产品控制台 —— **已取消**（见 §0）

### 5.7 阶段 6：Audio Hub 注册与同步

- ☑ 注册/激活码/绑定轮询/心跳（后台任务，不阻塞对话）
- ☑ 清单 + 增量下载 + revision 保存 + 线性退避 + 失败保留旧声音/旧 revision
- ☑ Kconfig 服务地址（`WZH_AUDIO_SYNC_SERVER`，NVS `audio_hub/server` 可覆盖）+ 启用开关
- ☑ 对话期间暂停下载；MCP `sync_now`/`sync_status` 手动入口

### 5.8 阶段 7-8：统一生命行为调度与产品动作专项

- ☐ 有界优先级队列：安全 > 手动 > 明确动作 > 情绪 > 对话 > 音频 > 自主 > 微动作
- ☐ 平滑打断、动作族洗牌、近期表演签名、精力状态、低电量降级
- ☐ 爬爬熊猫：重心转移爬行、差异化步幅、吃竹抱爪、TTS 轻扒
- ☐ 小熊猫：尾巴延迟配重、惯性曲线、三层次空间轨迹、慢包络
- ☐ 恐龙：29 互动变体 + 5 环境场景 + 4 待机生命动作 + Catmull-Rom 五轴 + 表演签名（对齐 BASE V3.3 `auto_run.cc`）

### 5.9 阶段 9：MCP 与验收

- ☑ 音频管理 MCP 工具（sound_list/play/delete/erase/flash_status + sync_status/sync_now）
- ☐ 产品 MCP 工具唯一命名 + 兼容别名（self.crawl_panda.* / self.dinosaur.* / self.red_panda.*）
- ☐ 情绪→动作统一去重与冷却
- ☐ 三板 IDF 6.0.x 全量编译 + 真机验收清单（见各阶段 PR 描述）

## 6. 真机验收清单（每产品）

冷启动/无 Wi-Fi 配网/断网重连；唤醒-对话-打断多轮；本地声与 TTS 不重叠；Flash 写入断电后旧 TOC 可读；长按关机/双击对话/电源锁存；电池百分比；舵机方向限位与供电压降；长时间运行内存；同步+对话并发稳定。
