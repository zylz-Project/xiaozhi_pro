# WZH 玩具 · 微信小程序

把已定稿的 WZH 网页详情页搬到微信小程序：**启动即蓝牙连接，连上后只有详情页**；
配网页收进“更改网络”弹窗；没有登录页、没有开发者页。UI 按 390px 网页设计稿
1:1 移植，声音复刻、说明书、主题、恢复出厂都在详情页内完成。

- AppID：`wx4b3ae68ee78890da`
- 小程序目录：`wzh-miniprogram/`（独立于固件 `main/`）
- 技术：原生 WXML/WXSS/JS，无 npm、无 CDN、无外部依赖
- BLE 契约：[docs/ble-protocol.md](docs/ble-protocol.md)
- 固件实现：`main/boards/common/wzh/wzh_ble_service.cc`（三款 WZH 板均已接线并编译通过）

## 1. 快速开始（开发者工具）

1. 打开微信开发者工具，登录有该 AppID 权限的微信。
2. 「导入项目」→ 目录选 `wzh-miniprogram/`，AppID 填 `wx4b3ae68ee78890da`。
3. 编译。开发者工具环境自动进入 **mock 演示模式**（`USE_MOCK_IN_DEVTOOLS=true`），
   不需要真实设备即可看完整 UI：连接页会列出三台演示设备，点任意一台进详情页。
4. 真机预览：工具栏「预览」扫码；手机端没有 `devtools` 环境，会走真实 BLE。

> 开发者工具首次使用需要用户自己登录/扫码；本仓库不包含登录态。
> 本机已安装微信开发者工具并开启自动化端口，模拟器验证见第 9 节；真机仍需手机扫码。

## 2. 真机 BLE

固件已实现 NUS 服务与 JSON 行帧协议，连接流程：

1. `wx.openBluetoothAdapter` → `wx.startBluetoothDevicesDiscovery`；
2. 按广播名前缀过滤三款玩具（`CrawlPanda-` / `Dino-` / `RedPanda-`）；
3. `wx.createBLEConnection` → 发现 NUS 服务/特征 → 开 notify → 请求 MTU；
4. 发 `hello`，拿到 `device` + `status`；
5. 进详情页；之后由小程序主动 `get_status` 刷新，设备不推送通用 status 事件。

广播包不带 128 位 UUID，必须按名称前缀发现设备。Android 会把 MTU 协商到 247
并按 180 字节分片；iOS 通常只有 20 字节，上传大文件会明显变慢。

切回 mock：把 `services/config.js` 的 `USE_MOCK_IN_DEVTOOLS` 设为 `true`；
在手机上运行时 `platform()` 不是 `devtools`，本来就会用真实 `BleClient`。

## 3. 目录

```
wzh-miniprogram/
├── app.js / app.json / app.wxss
├── project.config.json          # AppID、libVersion、忽略 docs/tools
├── sitemap.json
├── assets/                      # 三款产品 512x512 透明底 PNG + 泽元灵智品牌 logo
├── pages/
│   ├── connect/                 # 启动页：扫描、附近设备、连接遮罩、演示模式
│   └── device/                  # 详情页 + 展开控制台 + 所有弹窗
├── services/
│   ├── config.js                # BLE UUID、超时、MTU、mock 开关
│   ├── ble.js                   # 真实 BLE 客户端（JSON 行帧/分片/队列）
│   ├── mock.js                  # 开发者工具演示设备（按固件契约实现）
│   ├── audio.js                 # WAV 解析/重采样/Base64/分片上传
│   ├── products.js              # 三款产品元数据与 productId 映射
│   ├── store.js                 # 主题/产品/上次设备本地存储
│   └── cloud.js                 # 早期云接口占位，当前未接线
├── styles/icons.wxss            # 24 个 SVG mask 图标
├── utils/format.js              # 时间/信号/字数格式化
└── docs/                        # BLE 与云接口契约（不打包上传）
```

## 4. 页面与交互

### 连接页 `pages/connect`

- 顶部为“泽元灵智”透明底品牌 logo；暖纸渐变 + 雷达扫描。
- 三款玩具入口（爬爬熊猫 / 小恐龙 / 摇尾小熊猫）：点一款只看它的扫描结果，
  再点一次取消筛选；没有该款设备时给出对应提示。
- 显示附近设备（图标、名称、型号、信号格、RSSI）；列表下方有“第一次连接？”三步提示。
- 记住上次连过的设备（`panda-last-device`）：下次进入置顶并标“上次连接”，
  扫描到后 1.2 秒自动连一次（用户手动点选会取消）；不在附近时提示“上次连接的 X 不在附近”。
- 首次进入自动弹三步引导（打开蓝牙 → 把它放到手边 → 长按开机），
  走完后写 `panda-connect-guide-v1`，之后不再自动出现；底部“第一次用？看三步引导”可随时重看。
- 连接中全屏遮罩；失败显示错误并可重试。
- 开发者工具/无设备时显示“使用演示设备”。
- 连接成功写 `panda-toy` 与 `panda-last-device`，`wx.redirectTo` 详情页。

### 详情页 `pages/device`

- 收起态：状态胶囊、产品名、状态球、白色 sheet（设备状态、电量/温度/网络三指标、
  音量、当前音色 + 试听、黑色“开始对话”）。
- 展开控制台：控制台头（说明书、关闭）、电池环、温度/运行、网络条 + “配置”、
  固定 6 音色（自动左移、按住即停、可左右拖动）、VOICE LAB 录音/上传、复刻音色行、管理弹窗、
  音量、主题（布局 C/D、玻璃 A/B、颜色、明暗）、恢复出厂。
- 展开后右下角出现可拖动小助手，拖动可移动，点击开始/结束对话（与主按钮同步）。
- 更改网络：页内弹窗，设备扫描 Wi-Fi 列表 + 密码 + 连接状态；同名 SSID 去重并优先
  保留可连接的 2.4GHz，5G 网络仍显示但标注“设备不支持”且不可选，短密码拦截，
  连接成功后关闭弹窗、不跳转。
- 声音复刻：
  - **设备录音**：点“开始录制”，设备提示后连续对玩具说话约 10–15 秒（最长录 20 秒），设备自己上传云端；
  - **上传 WAV**：`wx.chooseMessageFile` 选 `.wav`，小程序解析为 16kHz 单声道
    PCM16（5–20 秒），再走 BLE `voice_upload_begin/chunk/end`。
  - 管理弹窗支持使用/重命名/删除（删除二次确认）；删除当前音色会回退龙安欢。
- 说明书：页内 13 章，结构与网页版 `manual.html` 一致（快速上手 / 认识它 / 开关机 /
  按键与触摸 / 对话 / 配网与换网 / 小程序控制台 / 声音与复刻 / 动作与互动 /
  安全与保护 / 常见问题 / 恢复出厂 / 规格），顶部 13 个章节胶囊可跳转，
  FAQ 可展开；主题/产品跟随，三款文案分别适配（恐龙含颈部/尾巴，小熊猫含大尾巴）。
- 恢复出厂：必须输入“确定恢复出厂设置”；按 `reset` 事件的 `phase` 展示进度，
  成功后清连接回连接页。

主题存储键与网页一致：`panda-appearance-v1` = `{theme,material,layout,mode}`；
产品键 `panda-toy`；上次设备 `panda-last-device`（`{deviceId,name,product,ts}`）；
连接页引导标记 `panda-connect-guide-v1`。

## 5. 声音复刻的数据流

```
设备录音（推荐）：
  小程序 voice_record {name}
    → 设备提示并录音，最长 20 秒
    → 设备自己上传阿里云、生成音色
    → 事件 voice_record{status:success}
    → 小程序 refreshVoices

上传 WAV：
  wx.chooseMessageFile(.wav)
    → services/audio.js 解析/重采样为 16k mono PCM16
    → voice_upload_begin {name,bytes}
    → voice_upload_chunk {data:base64} × N
    → voice_upload_end
    → 轮询 get_status 的 voice_management_busy/status
```

- 音频**不经过小程序云端**；设备持有阿里云 Key，小程序不接触 Key。
- 三款固件都编译 `voice_tts`（`main/CMakeLists.txt` 的 WZH 公共块 GLOB），
  设备录音、上传、复刻音色能力一致；`voice_record` 失败事件会带 `reason`，
  小程序按 `busy/config/prompt/memory/short/cloud/auth/limit/voice_full/...`
  显示具体原因。
- `services/cloud.js` 与 `docs/cloud-api.md` 是早期占位，当前 `device.js` 不引用。

## 6. 与网页版的关系

- 视觉/布局/主题/图标沿用已定稿的网页版详情页设计（网页版不在本仓库内）。
- 小程序去掉了：登录页、开发者页、说明书独立页、网页版配网流程；
  配网改为详情页“更改网络”弹窗。
- 小程序不加载 iframe/外部字体/外部图片；图标用 CSS mask，产品图用本地 PNG。
- 用户网页端已从固件移除（`main/assets/wzh_user_web.html` 删除、`wzh_user_web.cc`
  返回 404）；只保留 8080 开发者页，且仅同一 STA 局域网可访问。

## 7. 已知限制

- 开发者工具用 mock 验证 UI；真机 BLE 扫描/连接/断线重连/分片仍需在手机上完成。
- iOS 默认 20 字节写，5–20 秒 WAV 上传会很慢；优先用设备录音或 Android 上传。
- `voiceCount` 固件按“内置 6 + 复刻数”统计，`voiceCapacity` 固定 10；
  展示时不要把它当成“复刻 6/10”的容量。
- 说明书按网页版 `manual.html` 的 13 章结构与文案重做，但正文按“小程序 + BLE”
  改写（不出现 192.168.4.1、页面密码、登录页等网页流程）；改文案改
  `device.js` 的 `MANUAL_COPY` / `faqFor()`，改章节结构改 `MANUAL_NAV`。
- 未做小程序备案/类目/隐私协议配置；上线前需在公众平台补齐。

## 8. 验证清单

- [x] 开发者工具导入、编译无报错；连接页品牌 logo、三款入口、三台演示设备可连接。
- [x] 详情页收起/展开、说明书、主题、音量、音色、管理弹窗可用。
- [x] 更改网络：列表、短密码拦截、连接中转圈、成功关闭弹窗并刷新。
- [x] 设备录音：开始后事件顺序 starting/prompt/recording/processing/success。
- [x] WAV 上传：5–20 秒校验、进度、生成后列表刷新。
- [x] 重命名、删除；删除当前音色回退龙安欢。
- [x] 恢复出厂确认语、phase 进度、成功回连接页。
- [ ] 真机 iOS/Android 扫描/连接/断线重连。
- [ ] 真机 BLE 分片：Android MTU 247、iOS 20 字节。
- [ ] 真机设备录音、WAV 上传、云端克隆（mock 已覆盖 UI 与协议顺序）。

## 9. 本轮验证记录

- [x] 全部 JS `node --check` 通过。
- [x] Node harness 验证 WAV：6s / 44.1kHz / 双声道 → 16kHz 单声道，
  `duration=6`、`bytes=192000`。
- [x] Node harness + `MockClient` 跑通完整详情页：连接 mock 恐龙、展开控制台、
  改网（扫描/短密码拦截/连接成功）、设备录音事件链、WAV 分片上传、
  重命名/删除、恢复出厂（phase 进度 + 断开即成功 + 回连接页）。
- [x] Node harness 跑通连接页：三台演示设备扫描、恐龙筛选/取消筛选、连接跳转。
- [x] BLE 分片/UTF-8 跨包重组/成功响应/错误码映射全部通过。
- [x] 品牌 logo 转透明底并接入连接页；深色主题改用白色版资源。
- [x] 微信开发者工具模拟器全界面验证：连接页、收起态、展开控制台、Wi-Fi 弹窗、
  说明书、音色管理、录音/上传、恢复出厂、四主题明暗与 C/D 布局、三款产品。
- [x] 按设备返回结果展示网络；同名 SSID 优先保留可连接的 2.4GHz，连接成功只关弹窗、不跳转。
- [x] `voice_record` 失败 `reason` 映射为具体中文提示。
- [x] BLE 协议文档重写为 v2（实际固件契约）。
- [x] 固件 host tests 全量 365 项通过（265.846s；用户网页测试已随网页端移除）。
- [x] 三款固件串行构建通过：crawlPanda `0x30eec0`（22% free）、dinosaur `0x3772d0`（12% free）、tailRedPanda `0x31d5f0`（21% free）。
- [x] 三款 `-barge-in` 生产变体也构建通过：crawlPanda `0x30eec0`、dinosaur `0x3772c0`、tailRedPanda `0x31d5f0`；8 个变体的 `config.json` 均带 NimBLE 配置。
- [x] 固件 BLE 复核并修复：分帧尾包滞留、录音状态 `name` 跨任务竞争；WZH 不再创建 SoftAP，`:8080` 仅同一 STA 局域网可访问。
- [x] 修复微信 `<button>` 默认 184px 宽度导致的按钮错位（主按钮/恢复出厂/扫描/试听/管理/说明书关闭等）。
- [x] 修复说明书整页继承页码样式（`--ink-3` + 16.35rpx）导致深色下不可读。
- [x] 展开控制台底部留白避开悬浮小助手；连接页补设备提示；浅/深色状态胶囊与恢复出厂条对比度调整。
- [x] 移植网页版 15 步聚光灯引导（状态球→开始对话→控制台→网络→音色→录音/上传→音量→主题→恢复出厂→说明书→关闭），15 步全部通过。
- [x] 主题面板“布局/玻璃”回到与网页一致的同一行（两个 `.theme-mini` 各 168.5px，top 相同），材质 A/B 预览区分蓝灰/奶油，深色选中态改为 accent 描边 + 15% 白底。
- [x] 电池表盘由 `conic-gradient` 实心扇形改为 4.6px 细环（`mask: radial-gradient(farthest-side, transparent calc(100% - 4.6px), #000 0)`），与网页 `.battery-ring` 一致。
- [x] 固定音色条：`touchstart/touchmove` 暂停、`touchend/cancel` 后 1.8s 恢复（对齐网页 1800ms），收起时暂停；实测 tap 后 `railPaused=true`、2.2s 后恢复。
- [x] 音色 chip 加 `box-sizing:border-box` 固定 30px 高（与网页一致），字号提到 11.5px 抵消开发者工具 0.83 字体缩放。
- [x] 说明书按网页版重做为 13 章（章节胶囊跳转、FAQ 折叠、规格表、印章结尾），关闭按钮移到右上避开微信胶囊；修复深色主题未覆盖 `--ink-soft` 导致正文不可见。
- [x] 说明书正文 22.12rpx、标题/封面放大，顶部留白收紧；弹窗底部加 `safe-area-inset-bottom`。
- [x] 连接页补“第一次连接？”三步提示与设备副标题/RSSI 字号；设备列表下方空白明显缩小。
- [x] 录音成功文案改为“音色已生成，可以在管理里试听或使用”。
- [x] 修复展开控制台下半屏不绘制/底部大块留白：原生 `<scroll-view>` 在展开动画期间创建会把视口缓存成半高；改为显式高度 `calc(100vh - var(--menu-bottom) - 8px - 12rpx - 131rpx)` 并随展开立即挂载，底部内边距改为 `16rpx`（不再叠加 safe-area，恢复出厂条距屏幕底约 13.6px，紧贴 Home 指示条上方，比网页版更低）。
- [x] 微信开发者工具已登录（AppID `wx4b3ae68ee78890da`），`cli preview` 预览包生成成功（900 KB / 上限 2 MB）；二维码待用户手机扫码。
- [x] 数字格式化：温度四舍五入（真机曾显示 `51.79999...`）、按 45/55°C 分 `warm/hot` 配色，
  延迟 `<=0` 显示 `--`，其余 `Nms`；抽到 `utils/format.js`。
- [x] 真实延迟：固件 `status.latency` 恒为 0，改为状态刷新后追加一次 `ping` 计 RTT（超时 1.2s 忽略）。
- [x] 状态球随状态联动：`--orb-e`（能量：待机 .3 / 连接中 .4 / 听 .5 / 说话 .55+音量×.45 / 离线 .16）
  与 `--orb-k`（速度系数）只改气泡/光晕动画时长与透明度，不触发重排。
- [x] 引导皮肤抽到 `styles/guide.wxss`，详情页与连接页共用（`@import`）。
- [x] 连接页：记住上次设备（置顶 + “上次连接”徽标 + 1.2s 自动重连 + 不在附近提示）、
  首次进入三步引导（`panda-connect-guide-v1` 只出现一次，底部可重看）。
  实测：引导 3 步 spot 坐标有效；自动重连成功进入对应产品详情页；缺设备提示正常。
- [x] 控制台滚到最底部时悬浮小助手自动淡出（`onConsoleScroll` + `assistDim`，
  距底 <48px 且内容可滚动才触发），不再压住“恢复出厂”条；往上滚即恢复。
  重新生成：`wechat-devtools-cli cli preview --project <abs>/wzh-miniprogram --port 17583 --qr-format image --qr-output /tmp/preview-qr.png`。
- [x] 复刻音色条改为 `<scroll-view scroll-x>` + 内部 `.clone-track`（30px 行高、上下 2px 内边距），
  chip 与右侧“管理”按钮同轴心（实测 chip top 615.4 / 管理 614.4，中心同为 630.4）；
  右侧渐隐改用 px 单位（rpx 在 WXSS 的 mask 渐变里不生效）。
- [x] 固定音色条同样改为 `<scroll-view scroll-x>`，可手指左右拖动；漂移改由 JS 驱动 `scroll-left`
  （展开 2.6s 后每 60ms +1px，滚过一份音色回到 0），触摸即停、松手 1.8s 恢复，与网页
  `main/js/index-app.js:953-1029` 一致；实测 0→20px、暂停 1.1s 期间不变、恢复后继续。
- [x] 网络条右侧“更改”改为“配置”。
- [x] 点标题切换玩具时，当前产品也跳回连接页（同款可能有不同台设备）。
- [x] 15 步引导聚光灯不再回左上角：`.pg-spot` 去掉 `transition`，切换步骤时不再把
  `guideSpot` 重置为 0；实测 `nextGuide` 后 60ms 仍是旧位置，700ms 直接出现在新目标。
- [x] 引导气泡残影修复：`showGuideStep` 只切布局（展开/收起控制台、开关说明书），
  位置由 `measureGuide` 一次性 `setData({guideStep,guideCurrent,guideSpot,guideBubble})`；
  `.pg-bubble` 去掉 `opacity` 过渡。同状态步骤旧气泡保持到测量完成再整帧跳转（实测 636ms 原子切换）。
- [x] 引导中需要展开控制台/开关说明书的 3 个步骤：先把气泡隐藏、聚光灯缩成 0 点让整屏均匀变暗
  （`.pg-root.hide-bubble`），等 560ms 布局动画结束再一次性显示新气泡+新高亮（实测 10ms 隐藏、770ms 出现）。
- [x] `_guidePending` 守卫：切换未完成时连点“下一步”只前进一格。
- [x] 复刻音色条右侧“+ 新建”文字垂直居中：`.clone-rail .voice-add` 补 `inline-flex/align-items:center/justify-content:center`，
  实测图标与文字中心均为 630.39px（按钮中心同值），此前文字贴在按钮顶部。
- [x] 状态球卡顿优化（真机反馈“一动就卡”）：去掉 1 个 266px 圆 + 20 个气泡各自的
  `backdrop-filter` 与 `filter: blur`；`speaking` 动画由“动画 `border-radius`”（整棵子树每帧重绘）
  改为只动 `transform: scale`；气泡去掉 `bubbleMorph`；光晕改用 `radial-gradient` 代替
  `conic-gradient + blur`（对比网页三变体截图确认观感一致）；关键层加 `will-change`；
  控制台/说明书打开时暂停球体动画（16 个显式选择器，WXSS 不支持通配符 `*`）。
- [ ] 真机 BLE 扫描/连接/断线重连/分片。
- [ ] 真机设备录音、WAV 上传、云端克隆。
