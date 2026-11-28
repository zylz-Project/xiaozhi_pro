# 网页版 UI 优化方案（对齐小程序 v1.1 体验）

> 历史设计及测量记录。现行页面入口与验证方式见[设备网页](../../web/README.md)。

- 日期：2026-11-16
- 仓库：`/home/hanhan/Desktop/zylz-Project/xiaozhi/wzh/xiaozhi_pro`（分支 `feature/wzh-tailredpanda-barge-in`，HEAD `e9bfaf8`）
- 范围：`main/assets/wzh_ui/` 下的用户页（index / setup / done / login / manual）与其 CSS/JS；开发者页 `developer.html` 不在本次视觉范围内（保持 8080 桌面形态）。
- 不改：任何 C++、协议、NVS、路由、打包脚本。只改静态资源；固件需重新打包才能上机，但浏览器可先验收。

## 1. 为什么改

小程序端在 v1.1 已完成一轮“可读性 + 触控 + 性能”优化（字号 7 级阶梯、控制台头部降拥挤、容量去重、Wi-Fi 列表去重、展开态隐藏状态球、去掉常驻 blur）。网页版是同一套设计的另一实现，但目前：

- 最小字号 **7px**，全站 22 个不同字号档位（小程序 7 档）；
- 大量可点元素高度 **26–36px**，低于 44px 触控基线；
- 页面常驻 **34 个 `backdrop-filter` 图层 + 12 处 `filter: blur`**，状态球/极光层在待机时也持续动画；
- 同一信息重复（6/10 容量出现 3 次）、文案与小程序不一致（待机 / 播放中 / 本次开机 / 复刻声音）。

目标：**不改变页面结构与导航**，只做视觉、排版、触控、性能与文案的对齐，让网页版与小程序看起来是同一产品。

## 2. 勘察方法（保证“和手机上一样”）

- 本地起静态服务 + **API mock**（`/api/status`、`/api/user/voices`、`/scan` 等返回真实结构 JSON），避免页面停在 `--` 或弹 JSON 解析错误。
- 浏览器每次新建 CDP session 后 `Emulation.setDeviceMetricsOverride {width:390,height:844,deviceScaleFactor:2,mobile:true}`，再 `Page.captureScreenshot` 取图——视口与 iPhone 12/13/14 完全一致（网页是 CSS px 直出，无缩放）。
- 逐页跑脚本采集：可见文本的 `font-size/color/rect`、可点元素高度、`backdrop-filter`/`blur`/`animation` 计数、容器 `scrollWidth` 溢出检测。
- 结果：**320 / 390 均无横向溢出**；问题集中在字号、触控尺寸、对比度、性能与重复信息，而不是布局崩坏。

### 实测数据（390 宽）

| 项目 | 现状 |
|---|---|
| 全站字号档位 | index 22 档（7 / 7.5 / 8 / 8.5 / 9 / 9.5 / 10 / 10.5 / 11 / 11.5 / 12 / 12.5 / 13 / 13.5 / 14 / 15 / 15.5 / 16 / 18 / 19 / 20 / 21） |
| 最小字号 | 7px（弹窗 kicker、步骤序号）、7.5px（lab-kicker、manage-count） |
| 正文 | 音色管理按钮 **8px**；网络行副标题 8.5px；说明书正文 11.5px |
| 控制台头部 | 宽 364 / 高 79；`.con-guide` 36×36、`.con-manual` 84×44、`.con-close` 44×44（三个按钮三种尺寸） |
| 触控 | `.vm-use` 41×26、`.net-change` 46×26、`.mode-btn` 115×30、`.voice-chip` 30 高、`.modal-btn` 35 高、`.vm-hero-play` 28 高 |
| 性能 | 待机时 6 个动画常驻（`orb-aurora:auroraSpin`、`panda:breathe`、`status-dot:dotPulse`、`net-sweep:radarSweep` 等）；`backdrop-filter` 34 处；`filter: blur` 12 处 |
| 容量重复 | `.lab-count` 6/10、`.manage-count` 6、`.vm-cap` 6/10 三处同义 |

## 3. 问题清单

### P0（影响可用性）

1. **字号过小**：7–10px 在手机上不可读；说明书正文 11.5px 长文阅读吃力。
2. **触控目标过小**：音色管理每行 4 个 8px/26px 按钮，误触率高；`.net-change`、`.mode-btn`、`.modal-btn` 均 < 44px。
3. **控制台头部三按钮尺寸不一**（36/84/44），视觉重心偏右，“说明书 P.01”胶囊过宽。

### P1（体验与一致性）

4. **性能**：34 层毛玻璃 + 常驻 blur 动画，中低端机滚动掉帧、耗电。
5. **重复信息**：容量 6/10 三处；“音色管理”入口与“管理”按钮并存。
6. **对比度**：9–10px 小字用 `rgba(47,40,34,.48)`，正文对比度不足（≈3.2:1）。
7. **文案不一致**：待机/播放中（小程序为 待机中/说话中）、“本次开机”、“复刻声音”、“清除网络与本机复刻声音”。
8. **换网模式文案错位**：`/setup?from=device` 仍显示“连上家里的 Wi-Fi / 第一次配网 / 连热点-选网络-输密码”，与实际“已在线，换一个网络”的语境不符；且列表里当前网络与顶部“当前连接”卡重复。

### P2（打磨）

9. 主题面板缺少当前组合摘要（小程序已加“奶糖 · 奶润 · 浅色”）。
10. 展开态右下浮动助手遮住“恢复出厂设置”条。
11. `.voice-chip` 固定轨在 390 宽时超出容器 23px（靠 `overflow-x` 裁剪，右侧渐隐与“管理”按钮贴边）。
12. 说明书页 h1 27px / 正文 11.5px 落差过大。

## 4. 目标设计

### 4.1 字号阶梯（与小程序同一套，CSS px）

| 级别 | 字号 | 用途 |
|---|---|---|
| L0 | 10 | 英文 kicker、步骤序号、极小标签（下限，不再出现 7–9px） |
| L1 | 11.5 | 次要元信息、提示、badge |
| L2 | 12.5 | 说明性正文、副标题 |
| L3 | 13.5 | 默认正文、按钮文字 |
| L4 | 15 | 强调正文、数值、主按钮 |
| L5 | 17 | 区块标题 |
| L6 | 22 | 页面主标题 / 大数字 |

保留展示级：done/manual 的 h1（27px）、index 品牌名（20→22）、`.voice-now`（18→20）、`.con-title`（19→20）。

### 4.2 关键映射

| 选择器 | 现在 | 目标 |
|---|---|---|
| `.con-eyebrow` / `.panel-meta` / `.voice-cap` | 9.5 | 11.5 |
| `.con-title` | 19 | 20 |
| `.con-sub` / `.net-val` / `.row-label` | 10 | 11.5 |
| `.net-name` | 11.5 | 13.5 |
| `.net-sub` / `.net-chip` / `.net-change-txt` | 8.5 | 11.5 |
| `.voice-chip` | 10.5 | 12.5 |
| `.voice-now` | 18 | 20 |
| `.battery-caption` / `.lab-count` / `.vm-cap` | 9 | 11.5 |
| `.lab-kicker` / `.manage-count` | 7.5 | 10 |
| `.lab-title` | 14 | 15 |
| `.lab-step` | 8.5 | 11.5 |
| `.audio-hint` / `.field-hint` | 8 | 11.5 |
| `.vm-use` | 8 | 12.5 |
| `.vm-hero-play` / `.vm-new` | 9 / 10.5 | 12.5 |
| `.modal-btn` | 11 | 13.5 |
| `.modal-kicker` | 7 | 10 |
| `.sheet-state` / `.status-text` | 10 / 9.5 | 11.5 |
| `.cta-label` | 13 | 15 |
| `.volume-val` | 10.5 | 15 |
| `.metrics b` / `.metrics em` | 16 / 10 | 17 / 11.5 |
| `.theme .swatch` / `.layout-btn` / `.mat-btn` / `.mode-btn` | 9.5–16 | 12.5（按钮文字统一 12.5） |
| setup `.sub` / `.net-row small` / `.scan-meta` | 10.5 / 8.5 / 8 | 12.5 / 11.5 / 11.5 |
| setup `.net-row b` | 12.5 | 15 |
| done `.sub` / `.note` | 11.5 / 8.5 | 13.5 / 11.5 |
| login `.sub` / `.login-note` | 10.5 / 8.5 | 12.5 / 11.5 |
| manual 正文 / 章节标题 | 11.5 / 18 | 13.5 / 20 |

### 4.3 触控与容器

| 元素 | 现在 | 目标 |
|---|---|---|
| `.con-guide` / `.con-manual` / `.con-close` | 36 / 84×44 / 44 | 统一 44×44 圆形，说明书改为图标 + `P.01` 小字（去掉宽胶囊） |
| `.voice-chip` | 30 高 | 36 高，字号 12.5 |
| `.voice-add` / `.manage-btn` / `.listen-mini` | 32 / 32 / 34 | 36 |
| `.net-change` | 26 | 34（保留文字“更改”） |
| `.layout-btn` / `.mat-btn` / `.swatch` / `.mode-btn` | 36 / 36 / 36 / 30 | 36 / 36 / 36 / 34 |
| `.vm-use` | 41×26 | 48×34 |
| `.vm-row` 行高 | ≈44 | 56（名称 13.5 + 操作行） |
| `.modal-btn` | 35 | 44 |
| `.modal-close` / `.vm-hero-play` | 32 / 28 | 36 / 34 |
| `.create-tab` | 32 | 36 |

### 4.4 对比度

- 浅色：小字 `rgba(47,40,34,.48–.58)` → `rgba(47,40,34,.66)`；次要正文 `.70`。
- 深色：`rgba(246,236,226,.70)`。
- 目标：正文 ≥ 4.5:1，10–11.5px 小字 ≥ 4.5:1（现在约 3.2:1）。

### 4.5 文案（与小程序统一）

| 位置 | 现在 | 改为 |
|---|---|---|
| 设备状态 | 待机 / 播放中 | 待机中 / 说话中 |
| 运行时长 | 本次开机 | 开机时长 |
| VOICE LAB 标题 | 复刻声音 | 我的音色 |
| 容量 | 复刻 6/10（三处） | 我的音色 6/10（仅声音区一处 + 进度条） |
| 恢复出厂 | 清除网络与本机复刻声音 | 会清空 Wi-Fi 和本机录制的音色 |
| setup 2.4G | 只支持2.4GHz。选一个网络，输入密码就好。 | 只能连 2.4GHz 网络。选一个网络，输入密码就好。 |
| setup 换网模式 | 连上家里的 Wi-Fi / 第一次配网 / 连热点·选网络·输密码 | 换一个 Wi-Fi / 更换网络 / 选网络·输密码·自动切换 |
| setup 空列表 | — | 没有发现网络，请靠近路由器后重试 |
| 上传提示 | 5–20 秒 · MP3 / WAV | WAV / MP3，5–20 秒（网页端确实支持 MP3，保留） |

### 4.6 结构微调（不动导航）

- 音色管理列表：当前网络式重复去掉——容量 6/10 只在声音区标题显示，管理弹窗内改为“我的音色 6/10”小字 + 进度条。
- 音色管理每行：名称（13.5）+ 右侧「使用」主按钮 + 「试听 / 改名 / 删除」收进一个「⋯」次操作区（或两行：名称行 + 操作行），避免 4 个 8px 按钮挤一行。
- 主题面板：`panel-meta` 显示 `奶糖 · 奶润 · 浅色` 摘要。
- 展开态：`.float-assistant` 上移或降低层级，避免遮挡恢复出厂条。

## 5. 性能方案

1. **减少毛玻璃**：`backdrop-filter` 从 34 处收敛到 ≤ 6 处（仅 `.phone` 顶部栏、底部 sheet、弹窗遮罩），其余卡片改半透明纯色 + 1px 内高光，视觉差异 < 5%，GPU 合成层数量大幅下降。
2. **动画白名单**：只允许 `transform` / `opacity` 动画；停掉待机常驻的 `auroraSpin`、`radarSweep`，`panda:breathe` 降为 4s 且仅在 idle 运行。
3. **blur 静态化**：`orb-aurora`、`orb-glow` 的 `filter: blur()` 改为预渲染的径向渐变，动画只做 `transform/opacity`。
4. **`will-change`**：仅对 `.orb`、`.panda` 设置，且展开态移除。
5. **`prefers-reduced-motion`** 已有，保留；新增展开态暂停球体动画。
6. 验收指标：待机常驻动画 ≤ 2 个；`backdrop-filter` ≤ 6；`filter: blur` ≤ 4；滚动 5 屏无明显掉帧（DevTools Performance 采样对比）。

## 6. 实施方式

- 每个 CSS 文件**末尾追加**一段 `/* TYPOGRAPHY LAYER */` 与 `/* POLISH */`，用 `.phone` 上的 `--fs-0…--fs-6` 变量 + 高特异性覆盖，**不改原 3761 行 CSS 的既有规则**（便于回退与逐条 review）。
- HTML 只改：控制台头部三按钮结构（`.con-manual` 内部改为图标 + P.01）、音色管理行结构（若采纳次操作区）、必要的 `aria-label` 与文案。
- JS 只改：状态文案映射、换网模式文案、容量去重、列表过滤（当前网络不进列表）、主题摘要；不改请求/轮询/录音/上传逻辑。
- 改动文件预计：`css/index.css`、`css/setup.css`、`css/done.css`、`css/login.css`、`css/manual.css`、`css/panda.css`（共用组件）、`index.html`、`setup.html`、`setup.js`、`js/device-detail.js`、`js/index-app.js`。
- 备份：改前对 `main/assets/wzh_ui` 打 tar 快照到 `/home/hanhan/Desktop/zylz-Project/backups/`。

## 7. 验证

1. 页面 × 宽度矩阵：index（收起/展开/3 个弹窗）× setup（首配/换网/离线）× done × login × manual，分别 320 / 390 / 430，截图对比。
2. 断言脚本：字号表（无 < 10px 可见文本）、可点元素高度（主操作 ≥ 44，次操作 ≥ 34）、横向溢出（`scrollWidth ≤ innerWidth`）、`backdrop-filter`/动画计数。
3. 交互回归：展开/收起、音色选择与试听、录制/上传弹窗、恢复出厂弹窗、换网提交流程（mock）、登录、说明书 iframe/页面。
4. 三款产品（爬爬熊猫 / 恐龙 / 小熊猫）主题色与图标；四主题 × 明暗 × A/B 材质。
5. 上机前：`python3 scripts/pack_wzh_ui.py` 重新打包并确认 manifest 正常；固件需重编译才能上机（本轮不改 C++）。

## 8. 顺序与工作量

| 阶段 | 内容 | 预计 |
|---|---|---|
| A | 字号阶梯 + 对比度（5 个 CSS 追加层） | 1 轮 |
| B | 容器与触控尺寸（按钮/行高/间距） | 1 轮 |
| C | 控制台头部三按钮 + 音色管理行重构 | 1 轮 |
| D | 文案统一 + 换网模式 + 容量去重 + 主题摘要 | 1 轮 |
| E | 性能（毛玻璃收敛、动画白名单、blur 静态化） | 1 轮 |
| F | 全矩阵验证 + 文档 + 打包 | 1 轮 |

## 9. 风险与回退

- 风险 1：字号上调后个别容器高度不足 → 逐屏截图检查，必要时同步加高容器（voice-chip 36、modal-btn 44）。
- 风险 2：毛玻璃收敛导致层次感变弱 → 只保留顶部/底部/弹窗三处，其余用高光边框补偿。
- 风险 3：换网模式文案改动涉及 setup.js 分支 → 保留原分支逻辑，只改文案常量。
- 回退：整目录 tar 快照，单文件可直接还原。

## 10. 待确认（3 个决策点）

1. **字号阶梯**：采用与小程序一致的 7 级（10 / 11.5 / 12.5 / 13.5 / 15 / 17 / 22），最小 10px？
2. **结构**：只做视觉与文案（保留 `/setup` 整页换网、保留登录页），还是也把换网改成详情页内弹窗（完全对齐小程序）？
3. **性能**：接受减少毛玻璃与常驻动画（观感略“实”，但更省电更跟手）？

---

## 11. 执行结果与验证记录（2026-11-16 落地）

三个决策点按“推荐”执行：① 字号统一 7 级阶梯、最小 10px；② 只改视觉与文案，保留 /setup 整页换网与登录页（不做页内换网弹窗）；③ 接受减少毛玻璃与常驻动画。

### 11.1 改动文件（全部只动 main/assets/wzh_ui/）

| 文件 | 改动要点 |
|---|---|
| css/index.css | 末尾追加 TYPOGRAPHY LAYER（--fs-0…--fs-6 + 高特异性覆盖）、POLISH（容器/触控）、UI FIX 1（容量条、主题双行、当前音色行）、小助手块、玻璃材质预览、滚动遮罩柔化；backdrop-filter 34 → 4 |
| css/setup.css | 字号层 + .net-row 行高 60、.net-sig 18px、.guide-open/.scan-btn ≥ 44 |
| css/done.css、css/login.css、css/manual.css、css/panda.css、css/guide.css | 字号层；manual 章节导航右缘遮罩；guide 气泡字号 13.5px |
| index.html | 控制台头部三按钮（? / 图标+P.01 / ✕）、容量条结构、文案（开机时长、我的音色 6/10、会清空 Wi-Fi 和本机录制的音色） |
| js/index-app.js | 主题摘要（奶糖 · 奶润 · 浅色）、容量绘制 paintCapacity()、小助手拖动位置持久化 + 滚动让位 |
| js/device-detail.js | 状态文案（待机中/聆听中/说话中/连接中/升级中/配网中）、voiceRow() 重构音色管理行、paintCapacity() |
| js/setup.js | 三种模式文案（首配/换网/离线）、网络行信号条+锁+频段+箭头、过滤当前网络、5G 标签 |
| js/product.js | 无语音能力产品不再覆盖容量条（P.axes + 关节 · 动作说明）、三款 resetSub 统一 |
| js/done.js | 成功态修复（标题/状态行/状态胶囊/tab title 全部跟随结果） |

### 11.2 实测数据

- **字号**：390 宽原始 22 档（最小 7px）→ 7 级阶梯；320 / 390 / 430 三档“可见文本 < 10px”计数均为 **0**，scrollWidth ≤ innerWidth 全部通过。
- **控制台头部**：.con-actions 148px（44×3 + 8×2），.con-title 20px；320 宽 head 302px 不溢出（改前 390 需 332px）。
- **触控**：主操作 primary 48、modal-btn 43–48、mode-btn 34、manage-btn 40、vm-use 51×33、音色管理图标钮 33–34（受 320 宽行内空间限制，为已知取舍）。
- **性能**：生效的 backdrop-filter 34 → 4；待机常驻动画只留 1 个（呼吸），其余动画仅 transform/opacity。
- **换网列表**：当前网络只在顶部卡片，附近列表 4 → 3 行；信号条 1–4 格 + 2.4G/5G 标签 + 锁 + 箭头。
- **容量去重**：我的音色 6/10 + 46×4 进度条（60% 填充），与声音面板标题同一行；管理弹窗内不再重复。
- **done 成功态**：#doneHeading=网络连接成功、#doneStatus2=已连接 Home、#doneChip=已连接、title=连接成功 · 爬爬熊猫、doneActions opacity=1、主按钮 350×44。
- **manual**：390 全幅 iframe 内 docW=364、正文 14px/1.64、章节标题 20px、导航横向滚动右缘柔化。
- **三款产品**：爬爬熊猫 #c97752、恐龙 #5f7f57（bamboo）、小熊猫 #b65f76，图标/音色/文案均随 build 切换；四主题 × 明暗 × A/B 材质已截图核对。
- **弹窗（320 宽）**：音色管理 card 300×595、行 274×58；新建复刻音色 card 296×485；恢复出厂 card 364×372；均无横向溢出。

### 11.3 已知取舍

1. 小助手默认右下角，静止时会压住主题面板第 4 个色卡（竹青）；已支持拖动 + 位置持久化（localStorage: panda-assistant-pos），滚动时自动让位（fa-away）。
2. 音色管理行内 3 个图标钮 33–34px，低于 44 触控标准，受 320 宽单行四控件限制。
3. done 页在无头浏览器需 Emulation.setFocusEmulationEnabled 才能跑轮询（document.hidden 为 true 时 poll() 直接返回），真机不受影响。

### 11.4 备份与回退

- 改前快照：/home/hanhan/Desktop/zylz-Project/backups/wzh-ui-2026-11-16-2030.tar.gz（586433 B）。
- 回退：cd <仓库根> && tar -xzf /home/hanhan/Desktop/zylz-Project/backups/wzh-ui-2026-11-16-2030.tar.gz。
- 本轮未改任何 C++ / 路由 / NVS / 打包脚本；上机前仍需 python3 scripts/pack_wzh_ui.py 并重编译固件。

## 12. 组件效果层与用户反馈修复（2026-11-16 晚）

这一轮不新增功能，只打磨组件效果，并修复 4 个用户现场反馈。全部改动仍在 `main/assets/wzh_ui/` 内，未碰 C++ / 路由 / NVS / 打包脚本。

### 12.1 用户反馈修复

| 反馈 | 根因 | 处理 |
| --- | --- | --- |
| 固定音色不会自己动，手一碰就停 | 漂移逻辑在初始化时给 `.fixed-track` 追加克隆节点做无缝循环，但 `device-detail.js` 的 `voices()` 会先 `textContent=''` 重建 6 个 chip，克隆被清掉，`measureLoop()` 取不到首克隆 → loopPoint 恒为 0 | 改为签名守卫的 `refreshClones()`：内容变化才重建克隆（避免 MutationObserver 自触发死循环），步进改为时间基准 16px/s，到 loopPoint 回绕；点击取值兼容 `data-voice-id` / `data-voice` |
| 主题面板排版变了 | 上一轮把 `.theme-pair` 改成上下两行 | 恢复左右两列：`.theme-pair` row + 两个 `.theme-mini` 各占一半（390 宽时四个按钮各 66px、标签单行）；只在 `@media (max-width:359px)` 才换上下两行 |
| 说明书章节导航是一条白带 | 性能优化把导航的暖色渐变换成近白 `rgba(255,252,246,.94)` 并去掉背景模糊，压在暖纸上发白 | `.manual-nav` 改为 `position:static`、无底色无阴影，章节胶囊直接落在暖纸上（与小程序一致） |
| 深色模式整屏发灰 | setup/done/login/manual 不加载 `index.css`，暗色靠 `panda.css` 的 `filter: brightness(.32)` 压暗奶油色 → 中性灰 | 把四主题暗色墙纸变量（`--wall-1..3`、`--blob-1..5`、`--wall-glow`）搬到 `panda.css`，并 `.phone.dark .wallpaper{filter:none}`；实测墙纸起始色 rgb(52,37,28)、像素 (48,33,26) 暖棕 |

### 12.2 组件效果（COMPONENT POLISH 1/2）

1. **状态球去背景模糊**：`.orb` 原本 `backdrop-filter: blur(17px) saturate(154%)`，内部 10 个气泡 + 6 个泡沫每个还有 1.2–1.4px 的背景模糊 —— 手机上每帧合成成本很高。全部去掉，并用更实的径向渐变补偿（b 材质 .74/.26/.10/.20，a 材质 .60/.14/.06/.14），视觉几乎无差。
2. **展开态淡出后隐藏**：原来 `.phone.expanded .orb-wrap{opacity:.18;filter:blur(3px)}`，等于给整棵动画子树套一层实时模糊。改为 `opacity:0 + visibility:hidden`（0.24s 后隐藏），和小程序一致。
3. **待机静止、状态动效恢复**：性能层此前用 `!important` 关掉了 aurora / panda / status-dot 的常驻动画，连“说话中”也被一起关掉。现在只在 `[data-state="speaking"]` / `[data-state="listening"]` 恢复 auroraSpin+auroraBreathe、glowPulse、orbJelly、orbRing、pandaTalk；待机保持 0 动画省电。
4. **玻璃收敛**：底部 `.sheet` 背景模糊 32px → 18px。
5. **触控反馈**：去掉点击高亮块，按钮 / 音色胶囊 / 模式按钮 / 色卡 / 弹窗按钮 / 恢复出厂条等统一 `touch-action:manipulation`、`user-select:none`、按下 `scale(.972)`，禁用态不缩放。
6. **安全区**：控制台滚动区、恢复出厂条、底部弹层都加 `env(safe-area-inset-bottom)`，不压 Home 指示条。
7. **滚动与数字**：滚动容器 `overscroll-behavior:contain` + 惯性滚动；指标 / 音量 / 容量等数字 `tabular-nums`，数值跳动不抖。
8. **减少动态效果**：`prefers-reduced-motion: reduce` 时停掉球体 / 气泡 / 泡沫 / 状态点 / 扫描线动画，过渡缩到 0.01ms。

### 12.3 实测（390 / 320，DevTools 与本地 mock）

- 待机 `document.getAnimations()` = 0；说话态 = 46 个（aurora ×2、glow、orbJelly、pandaTalk、10 气泡 ×2、6 泡沫 ×2、stateBreath 等），全部只动 transform / opacity / border-radius。
- 生效的 backdrop-filter：4（sheet + 3 个隐藏弹窗卡）；球体 / 气泡 / 泡沫均为 `none`。
- 展开态 `.orb-wrap` opacity 0、visibility hidden；收起态可见。
- 390 / 320：`<10px` 可见文本 0、横向溢出 0、pageerror 0；主题面板 390 为 row（按钮 66px），320 为 column（按钮 112px）。
- 暗色：`.phone` 背景 rgb(29,22,18)、墙纸暖棕、`.sheet` rgba(28,21,17,.62)。
- 音色管理弹窗：card 364×611、6 行 ×57px；「新建」胶囊文字垂直居中偏差 0px。
- 说明书 / setup / done / login 深色与浅色均已复测。

### 12.4 编译

三款产品按 `python3 scripts/build.py <board> --name <variant>` 串行编译（共用 `build/`，不能并行），本轮 UI 资源会经 `scripts/pack_wzh_ui.py` 打进固件。

### 12.5 本轮新增备份

- 改前快照仍为 `backups/wzh-ui-2026-11-16-2030.tar.gz`；本轮结束后另存 `backups/wzh-ui-2026-11-16-2315.tar.gz`（含组件效果层与 4 个反馈修复）。


## 13. 用户反馈 5 项修复（2026-11-17）

用户现场截图反馈的 5 个问题，全部只改 `main/assets/wzh_ui/` 静态资源，未碰 C++ / 路由 / NVS / 打包脚本。

### 13.1 修复明细

| # | 反馈 | 根因 | 改法 |
| --- | --- | --- | --- |
| 1 | 顶部假时间 9:41、产品名位置太低 | 6 个页面各自写死了一个 `<span class="status-time">9:41</span>` | 6 个页面各删掉该 span（index/setup/done/login/manual/developer）；`.statusbar` 改 `justify-content:flex-end`。品牌行在 index.html 重排为「泽元灵智 logo + 分隔线 + 产品名/CRAWL PANDA」，并上移到 y=38（原 52） |
| 2 | 状态球一动就卡 | 顶部时间只是表象；真正成本在球体 / 气泡 / 泡沫的 `backdrop-filter` 与展开态的 `filter: blur()` | 沿用第 12 节 COMPONENT POLISH 1/2：球体与 16 个气泡/泡沫全部去掉背景模糊、展开态淡出后 `visibility:hidden`、待机 0 动画，只在聆听/说话恢复 transform/opacity 动画 |
| 3 | 说明书按钮里的 P.01 多余 | 头部三个按钮尺寸/内容不一致 | index.html 删除 `.con-manual-tag`；`.con-manual{gap:0}` 只留书本图标，圆形 44×44，图标与按钮同心 |
| 4 | 上传音频是假文件卡（「我的音频.mp3 · 已选择 · 14 秒 · 1.2 MB」） | 原实现是一个静态展示卡，真正的 picker 由 JS 动态创建 `display:none` 的 input —— iOS Safari 不会为隐藏 input 打开资源库 | 改为真实 `<input type="file" accept="audio/*,.mp3,.wav,.m4a,.aac">` + `<label for>`（input 视觉隐藏但可点击）；点「上传音频」tab 时 `input.click()`；选中后写文件名、启用生成按钮、自动填名称 |
| 5 | 弹窗按钮大小/高度不一致 | `.primary` 的 `margin-top:10px` 与 `.modal-close` 的 `flex-basis:32px` 作用在弹窗里 | `.modal-actions .modal-btn{width:auto;margin-top:0}`、`.modal-close{flex:0 0 40px}`；实测取消/生成音色均 166×43.3、y 相同，关闭钮 39.4×39.4 正圆 |

### 13.2 顺带修的两处

- **上传面板空 90px**：未选文件时 `.audio-wave`（52px 空轨道）与 `.audio-trim`（`hidden` 但被 `display:flex` 顶开）造成空洞。隐藏空轨道与 trim，弹窗高度 459 → 370；未选文件时「生成音色」禁用（opacity .38）。
- **320 宽网络指标把 dBm 截成「c」**：`.metric b.ssid` 降到 `--fs-5`、`.metric-net em` 加 `min-width:0; white-space:nowrap`，`@media(max-width:359px)` 隐藏「强」字样只留信号条。320/390/430 的 `scrollWidth == clientWidth`。

### 13.3 实测

- 390×745：收起态 logo 100×29 + 产品名/CRAWL PANDA、右上只剩「待机中」胶囊；展开态三圆钮 42–44px；上传 tab 触发 filechooser（`opened=1`），选中 sample-8s.wav 后文件名/名称/按钮状态正确。
- 320 / 390 / 430：`<10px` 可见文本 0、横向溢出 0、pageerror 0；setup/done/login/manual 在 745 高下 `docH == innerH`，无整页滚动。
- 引导 15 步（清空 localStorage 重跑）：step0 球体 400×304、step4 控制台头 378×92、step5 网络行 346×48，聚光灯与气泡位置正常。
- 暗色 390：`.phone` rgb(29,22,18)、logo 反白、弹窗与按钮对比正常。
- JS：`node --check` 四个入口全过；`css/index.css` 1325/1325、`css/panda.css` 238/238 花括号配平。

### 13.4 编译（IDF v6.0.2，三款串行）

| 板型 | 结果 | xiaozhi.bin |
| --- | --- | --- |
| wzh_crawlPanda → wzh-crawlpanda | EXIT 0 | 3,917,376 B |
| wzh_dinosaur → wzh-dinosaur | EXIT 0 | 4,323,008 B |
| wzh_tailRedPanda → wzh-tailredpanda | EXIT 0 | 3,961,616 B |

`build/esp-idf/main/wzh_ui_pack.bin` = 655,935 B（新增 `assets/zeyuan-logo.webp`，45,296 B，manifest 第 11 行）。编译成功不等于硬件验证，真机仍需复测上传资源库与深色显示。

### 13.5 主题机制说明

- 外观存在浏览器 `localStorage` 的 `panda-appearance-v1`（`{theme, material, layout, mode}`），由 `js/theme.js` 读写；没有 `prefers-color-scheme` 跟随，深浅色需手动切。
- 三款设备共用同一个源（设备热点 / 局域网 IP），所以同一浏览器里三款共享一份外观；换手机或清缓存会回到默认「奶糖 · 奶润 · 连续 · 浅色」。
- 若要「一台设备一套」或换手机保留，需要固件侧提供 `/api/appearance`（存 NVS）或前端把 key 拼上设备 ID；本轮按用户要求只做静态资源，未改固件。


## 14. 全量 UI 审计与修复（2026-11-17）

审计范围：320 / 390 / 430 三档视口 × index（收起 / 展开 / 音色管理 / 新建录音 / 上传空 / 上传有文件 / 恢复出厂）、setup（first / online / offline）、done、login、manual、developer（8 个 tab）。工具：本地 mock 服务（`/__mode`、`/__build`、`/__state` 切换状态）+ Playwright Chromium，统计 <10px 可见文本、横向溢出、clipped、broken image、tap 目标与 console error。

### 14.1 发现并修复的 6 个问题

| # | 问题 | 根因 | 修复 |
| --- | --- | --- | --- |
| 1 | 深色切换不持久、主题摘要始终显示「浅色」 | `js/index-app.js` 第二个 IIFE 调用了第一个 IIFE 内的 `updateThemeMeta()`，运行时报 `updateThemeMeta is not defined`，后续 `PandaTheme.set` 未执行 | 在该 IIFE 内新增 `syncThemeMeta()`，`setMode()` 改调它；实测点击后摘要为「奶糖 · 奶润 · 深色」、localStorage `mode=dark`、刷新后保持 |
| 2 | 深色状态胶囊几乎不可读（1.23:1） | `index.html` 不加载 `panda.css`，其 `.phone.dark .status-pill` 规则不生效 | `index.css` 追加深色 `.statusbar` / `.status-pill` 覆盖；实测文字 rgba(246,236,226,.86)，像素对比 ≈6.7:1 |
| 3 | 320 宽 setup 顶栏标题与热点名折行 | appbar 可用宽度不足 | `setup.css` 的 `@media(max-width:359px)` 收窄 gap/内边距、标题 13.5px 单行、热点名 10px 单行；实测 320 标题 95px 单行不截断、热点名 98px 完整 |
| 4 | done 页兜底文案空格错误 | `sessionStorage` 无 SSID 时输出「已连接 所选网络」 | `js/done.js` 分流 `已连接所选网络` / `已连接 Home` / `正在连接…` |
| 5 | 开发者页状态卡「网络延迟」实际显示 RSSI、运行时长副标题写成「本次开机」 | 文案与数据源不一致 | `developer.html` 改为「信号强度 / -54 dBm / Home」「开机时长」；网络卡「RSSI / -54 dBm」 |
| 6 | 320 开发者日志页按钮行溢出 4px、深色恢复出厂条对比 2.8:1 | 日志操作行 `display:flex` 不换行；危险色 #9f493a 压深底 | 日志行改 `flex-wrap:wrap` + select `flex:1 1 120px`；`index.css` 深色下危险条文字提亮到 #e89a80（≈7.5:1） |

### 14.2 审计结果（修复后重跑）

- **用户页全部干净**：320/390/430 × 13 个状态，`tiny(<10px)=0`、`clipped=0`、`brokenImgs=0`、`overflowX=false`；setup / login / done 的 offscreen 全部是场景装饰层（`section.*-scene`、`i.blob`、`span.hill`，被容器 `overflow:hidden` 裁切，不产生滚动）。
- **弹窗**：音色管理 6 行、每行「使用 + 试听/改名/删除」单行；新建复刻音色在未选文件时不再有 90px 空洞（370 高，按钮 disabled），选文件后提示文件名并启用生成。
- **引导**：320 / 390 各 15 步全部通过，气泡宽度 302 / 300、x=10 / 45，spot 均落在手机框内，无 pageerror。
- **三款 × 四主题 × 明暗**：tiny=0、overflow=false，主题色跟随切换（产品默认色会被所选主题覆盖，属既有设计）。
- **唯一 console 报错** `/favicon.ico` 404，非资源缺失。
- **开发者页**：按技术后台保留 8–9.5px 的密集元信息小字（普通用户页不使用）；320 宽 API 列表有 1 处 `code` 单行省略号（`/api/developer/flash/erase`），属设计内截断。
- **仍偏小的触控目标**（次级控件，按设计保留）：`voice-try` / `listen-mini` / `net-change` 34px、`voice-chip` 36px、`vm-use` 33.5px、`vm-icon` 33.5px、manual 章节胶囊 30px。

### 14.3 改动文件

仅 `main/assets/wzh_ui/`：`css/index.css`、`css/setup.css`、`js/index-app.js`、`js/done.js`、`developer.html`（累计 18 个修改文件 + 2 个新增，未动 C/C++、路由、打包脚本）。

### 14.4 重新编译

UI 打包内容变化，三款需重新串行编译；结果见 14.5。

### 14.5 第二次编译（2026-11-17 17:20–17:37，IDF v6.0.2，三款串行）

| 板型 | 结果 | xiaozhi.bin |
| --- | --- | --- |
| wzh_crawlPanda → wzh-crawlpanda | EXIT 0 | 3,918,304 B |
| wzh_dinosaur → wzh-dinosaur | EXIT 0 | 4,323,952 B |
| wzh_tailRedPanda → wzh-tailredpanda | EXIT 0 | 3,962,544 B |

`build/esp-idf/main/wzh_ui_pack.bin` = 656,869 B（上一版 655,935 B，+934 B 对应本轮 CSS/JS 增量）。`build/` 当前停留在 tailRedPanda；三款仍需真机复测（编译成功不等于硬件验证）。

备份：`/home/hanhan/Desktop/zylz-Project/backups/wzh-ui-2026-11-17-1727.tar.gz`（660,558 B，sha256 3a2811b94394e631db4d09997c74ce89ad9a159860593bdf844902fe7bcd763d，含 main/assets/wzh_ui 与本文件）。

