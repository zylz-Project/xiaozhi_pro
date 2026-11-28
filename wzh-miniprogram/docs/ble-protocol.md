# WZH 玩具 · 小程序 ↔ 设备 BLE 协议 v2

本文是小程序与 ESP32-S3 玩具之间的**唯一传输契约**，对应固件
`main/boards/common/wzh/wzh_ble_service.cc` 的当前实现。
小程序实现位于 `wzh-miniprogram/services/ble.js`，开发者工具 mock 位于
`wzh-miniprogram/services/mock.js`。

> v2 与旧 v1 的差异：`voice_list` 返回 `voices`；`wifi_scan` 返回
> `{networks}`；录音改为设备端 `voice_record {name}`；复刻音频改走
> `voice_upload_begin/chunk/end`；恢复出厂事件字段为 `phase`；固定音色是对象数组。
> 旧 v1 的云 `fileId` 与 `stage` 字段已废弃。当前手机录音通过与 WAV 文件相同的 BLE 上传协议发送。

## 1. 发现与连接

- 广播名：`<AP 前缀>-<MAC 后四位>`，例如 `CrawlPanda-1A2B`、`Dino-6C3F`、
  `RedPanda-9D2E`。前缀由各板 `PANDA_AP_PREFIX` 决定。
- 现有固件的广播包通过名称标识设备。小程序检查 `localName` 和 `name` 的产品前缀，合并实时回调与扫描快照。仅从快照得到的设备标为“最近发现”，本轮实时看到上次连接的设备后才允许自动重连。
- 已广播 NUS UUID 且缺少产品名的设备会显示为待识别。连接时通过 NUS 获取 `hello` 设备信息。
- 最近成功连接且由 `hello.device.identity` 验证的设备，返回时优先复用现有连接。重新连接只短试一次，失败即恢复发现流程。
- `hello.device.guide_epoch` 随恢复出厂变化。`status.guide_done` 表示设备已保存小程序全局引导的完成或跳过状态。首次完成或跳过时发送 `guide_complete`，参数为 `{"epoch": <guide_epoch>}`。操作可重复，过期周期会被拒绝。手动重看不写状态。
- `status.wifi.configured` 表示设备保存过网络，用于区分首次配网与暂时离线。`status.wifi.provisioning` 表示配网试连仍在进行。
- 连接参数：`wx.createBLEConnection` 超时 10s；连接后依次发现服务、开 notify、
  协商 MTU、发送 `hello`。
- 设备同一时间只接受一个 BLE 连接（`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`）。
- 断开后小程序回到连接页；设备保留 Wi-Fi、音量、音色等状态并重新广播。

## 2. GATT 定义

Nordic UART Service（NUS）：

| 用途 | UUID | 属性 |
| --- | --- | --- |
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | primary |
| RX（手机 → 设备） | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write Without Response |
| TX（设备 → 手机） | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify |

- 小程序优先按 UUID 查找；找不到时退化为“第一个可写特征 + 第一个 notify 特征”。
- 所有业务数据都走 RX/TX，不使用额外特征。
- 设备不会在连接后主动推送完整 status；小程序连接后主动发 `hello`。

## 3. 帧格式

UTF-8 JSON + `\n` 行分隔。**一个 JSON 对象一行，不允许跨行。**

### 请求（手机 → 设备）

```json
{"id":1,"op":"get_status","data":{}}
```

- `id`：正整数，单调递增；设备原样回传。
- `op`：操作名，见第 6 节。
- `data`：对象；没有参数时传 `{}`，不要省略。

### 成功响应（设备 → 手机）

```json
{"id":1,"ok":true,"data":{}}
```

### 失败响应

```json
{"id":1,"ok":false,"error":{"code":1005,"message":"Wi-Fi 密码需为 8 到 63 位"}}
```

- `ok` 缺省视为 true；`error` 存在视为失败。
- `message` 是可直接展示给用户的中文短句。

### 事件（设备主动推送）

```json
{"event":"wifi_state","data":{"state":"connected","ssid":"Home"}}
```

- 事件没有 `id`；小程序按 `event` 分发。
- 设备不主动推送通用 `status` 事件；小程序在操作成功后重新 `get_status`。

### 分片与 MTU

- 小程序默认按 20 字节有效载荷写；Android 请求 MTU 247，成功后按
  `min(180, mtu - 3)` 分片；iOS 通常保持 20 字节。
- 设备把同一连接上的连续 write 字节流按 `\n` 重新拼帧，不假设一帧一次 write。
- 设备单帧上限 4096 字节（不含换行），超过上限时丢弃到下一换行并返回 `1001`。
  每次 GATT 写入整段入队或整段拒绝；断开连接后不拼接上一连接的残片。
- 设备 notify 按 `min(mtu - 3, 244)` 分片，片间约 8ms。
- 小程序写队列串行发送；设备按到达顺序处理。

### 超时与重试

- 小程序默认请求超时 10s；`hello` 12s；`voice_upload_chunk` 30s。
- 超时错误码 `1009`。小程序不会自动重发非幂等操作
  （`voice_record`、`voice_upload_end`、`reset_factory`）。

## 4. 握手与配对

连接后小程序立即发送：

```json
{"id":1,"op":"hello","data":{"app":"wzh-mini","ver":"1.0.0","proto":2,"platform":"ios"}}
```

设备返回：

```json
{
  "id":1,"ok":true,
  "data":{
    "device":{
      "product":"爬爬熊猫",
      "productId":"WZH-CP01",
      "name":"CrawlPanda-1A2B",
      "model":"WZH-CP01",
      "firmware":"v1.0.8",
      "fixedVoices":[
        {"id":"longanhuan_v3.1","name":"龙安欢","description":"活泼亲切"}
      ],
      "clones":[{"id":"clone-1","name":"小满"}],
      "voiceCapacity":10
    },
    "status":{},
    "auth":{"required":false}
  }
}
```

- `product` 是中文产品名（爬爬熊猫 / 小恐龙 / 摇尾小熊猫），不是
  `crawlPanda` 这类内部 id；小程序用 `productId` 或 `model` 映射产品。
- `fixedVoices` 固定 6 个对象，包含 `id/name/description`。
- `clones` 只有 `id/name`，没有日期、音频 URL；小程序列表用序号补位。
- 产品配对由**硬件**完成；小程序没有登录页，不发送账号密码。
  固件当前 `auth.required=false`，不校验 token。
- 设备不把 Wi-Fi 密码写入日志或回传给小程序。

## 5. 状态对象

`status` 出现在 `hello.data.status` 与 `get_status` 响应中。设备不单独推送
`status` 事件；需要最新状态时由小程序主动拉取。电量暂未取得可信读数时
`battery` 为 `null`，小程序显示 `—`，不显示为 `0%`。

```json
{
  "online": true,
  "state": "idle",
  "battery": 86,
  "charging": true,
  "temp": 41,
  "tempState": "正常",
  "runtimeMinutes": 138,
  "runtime": "2h18",
  "ssid": "Home",
  "band": "2.4G",
  "signal": "强",
  "rssi": -48,
  "latency": 0,
  "volume": 60,
  "currentVoiceId": "longanhuan_v3.1",
  "currentVoice": "龙安欢",
  "version": "v1.0.8",
  "voiceCount": 6,
  "voiceCapacity": 10,
  "voice_management_busy": false,
  "voice_management_status": "ok"
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `online` | bool | 已连上 Wi-Fi 且拿到 IP |
| `state` | string | `idle` / `connecting` / `listening` / `speaking` / `starting` / `wifi_configuring` / `upgrading` / `activating` / `audio_testing` / `fatal_error` |
| `battery` | 0–100 | 电量百分比 |
| `charging` | bool | 是否充电中 |
| `temp` | number | 摄氏度 |
| `tempState` | string | `正常` / `偏高`（≥55°C） |
| `runtimeMinutes` | int | 本次开机分钟数 |
| `runtime` | string | 展示文案，如 `2h18` |
| `ssid` | string | 当前 Wi-Fi；离线时为空 |
| `band` | string | `2.4G` / `5G` |
| `signal` | string | `强` / `中` / `弱` |
| `rssi` | int | dBm |
| `latency` | int | 固定 0（固件未测量） |
| `volume` | 0–100 | 音量 |
| `currentVoiceId` | string | 当前音色 id |
| `currentVoice` | string | 当前音色名 |
| `version` | string | 固件版本 |
| `voiceCount` | int | 内置 6 + 已保存复刻数量 |
| `voiceCapacity` | int | 固定 10 |
| `voice_management_busy` | bool | 音色管理（上传/录音/删除）是否忙 |
| `voice_management_status` | string | `ok` / `busy` / `invalid` / `config` / `network` / `full` / `save` / `cancelled` / `uncertain` |

`status` 还包含 `wifi{connected,ssid,rssi,band,signal,bars,latency}` 与
`audio{...}` 等网页控制台字段；小程序只使用上表字段。`status` **不包含**
`voice_catalog`（3 秒轮询帧保持精简）；音色目录只走 `voice_list`。

## 6. 操作表

所有请求都是 `{"id":N,"op":"...","data":{...}}`。下表只列 `data` 与成功响应的
`data`；失败统一用第 8 节错误结构。

### 6.1 基础

| op | 请求 data | 成功 data | 说明 |
| --- | --- | --- | --- |
| `hello` | `app,ver,proto,platform` | `{device,status,auth}` | 握手 |
| `ping` | `{}` | `{pong:true,uptime:<秒>}` | 保活 |
| `get_status` | `{}` | `status` 对象 | 拉取全量状态 |
| `chat_start` | `{}` | `{state}` | 仅 `idle` 可进入对话；否则 `1010` |
| `chat_stop` | `{}` | `{state}` | 调用 `StopInteraction(true)` |

### 6.2 音量与音色

| op | 请求 data | 成功 data | 说明 |
| --- | --- | --- | --- |
| `set_volume` | `{volume:0..100}` | `{volume:N}` | 越界返回 `1003` |
| `set_voice` | `{id}` 或 `{name}` | `{id,name}` | 固定/复刻音色都可；忙返回 `1010` |
| `audition` | `{id}` 或 `{name}` | `{accepted:true,request}` | 仅 `idle` 时接受设备试听请求 |
| `audition_status` | `{}` | `{request,status}` | `status` 为 `idle`、`playing`、`complete`、`failed` 或 `cancelled` |
| `audition_stop` | `{request}` | `{accepted:true}` | 只取消同编号的活动试听 |
| `voice_restore` | `{}` | 与 `voice_list` 相同 | 切回上一音色，更新当前与上一个音色的记录 |
| `voice_record` | `{name,statusPoll?}` | `{accepted:true,name,request}` | 设备自己录音并生成复刻音色；旧客户端使用 `voice_record` 事件 |
| `voice_record_status` | `{}` | `{status,request,reason?}` | 查询当前或上次设备录音状态；可在进度通知丢失后恢复 |
| `voice_record_cancel` | `{request}` | `{}` | 仅准备或录音阶段；响应表示已受理，轮询看到 `cancelled` 才确认取消。 |
| `voice_list` | `{}` | `{voices,currentVoiceId,currentVoice,previousVoiceId}` | 上一音色为空时 `previousVoiceId` 返回空串 |
| `voice_rename` | `{id,name}` | `{id,name}` | 名称规则见下 |
| `voice_delete` | `{id}` | `{id}` | 删除本机的非当前复刻音色 |

`voices` 元素：

```json
{"id":"longanhuan_v3.1","name":"龙安欢","description":"活泼亲切","builtin":true}
```

- `name` 为 1–48 字节，不能为空；重名允许，用 `id` 区分。
- `voice_delete` 删除本设备归属的云端音色，并清理对应的本地名称及历史引用。
- 删除当前音色前先切换到其他音色。试听只有同一 `request` 的 `complete` 才确认播放完毕。旧固件不返回试听编号时，客户端保留原来的反馈。
- `voice_record` 事件：

```json
{"event":"voice_record","data":{"status":"prompt","name":"我的音色"}}
{"event":"voice_record","data":{"status":"recording","name":"我的音色"}}
{"event":"voice_record","data":{"status":"processing","name":"我的音色"}}
{"event":"voice_record","data":{"status":"success","name":"我的音色"}}
{"event":"voice_record","data":{"status":"failed","name":"我的音色","reason":"网络不稳定"}}
```

  `status` 取值：`prompt` / `recording` / `processing` / `success` / `failed` /
  `cancelled`；失败时 `reason` 是设备端错误码，可能为空。小程序对
  `busy` / `config` / `memory` / `short` / `cloud` / `auth` / `limit` /
  `voice_full` / `uncertain` / `save` 等状态也做了本地兜底文案。

新版小程序在 `voice_record` 中设置 `statusPoll:true`，然后主动查询
`voice_record_status`；设备不会为该次录音创建专用事件监听任务。未提供该字段的
旧客户端仍收到事件，监听有时间上限。查询只读取状态，不会重试云端创建；网络
中断或状态未知时应先检查音色列表，避免重复创建。录音最多 20 秒，可能在有效
人声达标且静音后提前结束；只有设备报告 `processing` 才视为录音结束。
取消使用本次录音的 `request`，不会取消后来开始的录音；已进入云端处理时不接受
取消录音。旧固件不返回 `request` 时，小程序只提供关闭窗口并保留状态轮询。

### 6.3 Wi-Fi

| op | 请求 data | 成功 data | 说明 |
| --- | --- | --- | --- |
| `wifi_scan` | `{}` | `{networks:[...]}` | 阻塞扫描；失败 `1005` |
| `wifi_status` | `{}` | `{connected,ssid,ip,rssi,band,signal,bars}` | 当前网络 |
| `wifi_connect` | `{ssid,password}` | `{accepted:true,ssid}` | 先 accepted，再推 `wifi_state` |

`wifi_scan` 返回示例：

```json
{
  "networks":[
    {"ssid":"Home","rssi":-48,"band":"2.4G","secure":true,"current":true,"bars":4},
    {"ssid":"Home-Guest","rssi":-55,"band":"2.4G","secure":false,"current":false,"bars":3},
    {"ssid":"Studio-5G","rssi":-67,"band":"5G","secure":true,"current":false,"bars":2}
  ]
}
```

规则：

- ESP32-S3 设备只能扫描并连接 2.4GHz；小程序展示设备返回的全部网络，若其他设备返回 5G 则标记为不支持，
  并对同名 SSID 去重（保留最强/当前连接），避免用户选完才失败。
- 开放网络 `password` 传空字符串；加密网络密码 8–63 位由设备校验。
- 连接过程事件：

```json
{"event":"wifi_state","data":{"state":"connecting","ssid":"Home"}}
{"event":"wifi_state","data":{"state":"connected","ssid":"Home"}}
{"event":"wifi_state","data":{"state":"failed","ssid":"Home","message":"连接超时，请检查密码或信号"}}
```

- 固件先在 RAM 中试连候选网络，最多等待 20 秒。确认已取得 IP 且 SSID 匹配后写入保存列表，再报告 `connected`。
- 失败时保留原 Wi-Fi 配置并恢复原列表联网。小程序显示错误，不自动重发换网请求。

### 6.4 复刻音色

复刻有两条路径，都**不经过小程序云端**：

1. **设备录音**。设备提示用户对玩具说话，再上传阿里云创建音色。
2. **手机录音或选择 WAV**。小程序将 WAV 转为 16 kHz 单声道 PCM16，通过 BLE 分片传给玩具，由玩具上传创建。

上传协议：

| op | 请求 data | 成功 data | 说明 |
| --- | --- | --- | --- |
| `voice_upload_begin` | `{name,bytes}` | `{bytes,chunkLimit}` | `bytes` 为 PCM 字节数，160000–640000 且偶数 |
| `voice_upload_chunk` | `{data:<base64>}` | `{received,total}` | 单块解码 ≤2048 字节 |
| `voice_upload_end` | `{}` | `{accepted:true}` | 异步创建，随后轮询 `get_status` |
| `voice_upload_cancel` | `{}` | `{cancelled:true}` | 释放缓冲 |
| `voice_create` | 任意 | 失败 `1003` | 保留旧 op，固定提示改用 upload_* |

- `bytes` 对应 5–20 秒 16kHz 单声道 PCM16（16000×2×秒）。
- `voice_upload_begin` 会分配约 `1+name+bytes` 的缓冲；失败返回 `1000`。
- `voice_upload_end` 返回 accepted 后，设备后台生成音色；小程序轮询
  `get_status` 的 `voice_management_busy` 与 `voice_management_status`。
- 生成成功/失败不会主动推事件；小程序最多轮询约 80s。

设备录音事件：

```json
{"event":"voice_record","data":{"status":"starting","name":"小满"}}
{"event":"voice_record","data":{"status":"prompt","name":"小满"}}
{"event":"voice_record","data":{"status":"recording","name":"小满"}}
{"event":"voice_record","data":{"status":"processing","name":"小满"}}
{"event":"voice_record","data":{"status":"success","name":"小满"}}
{"event":"voice_record","data":{"status":"failed","name":"小满"}}
```

`status` 可能为 `starting` / `prompt` / `recording` / `processing` /
`success` / `failed` / `cancelled`。设备只在状态变化时推送，最多轮询 45s。

### 6.5 恢复出厂

| op | 请求 data | 成功 data | 说明 |
| --- | --- | --- | --- |
| `reset_factory` | `{}` | `{accepted:true}` | 先 accepted，再推 `reset` 事件 |

事件：

```json
{"event":"reset","data":{"phase":"checking","current":0,"total":0}}
{"event":"reset","data":{"phase":"deleting_cloud","current":1,"total":3}}
{"event":"reset","data":{"phase":"clearing_local","current":0,"total":0}}
{"event":"reset","data":{"phase":"clearing_wifi","current":0,"total":0}}
{"event":"reset","data":{"phase":"rebooting","current":0,"total":0}}
{"event":"reset","data":{"phase":"failed","error":{"code":"...","message":"恢复失败，已保留 Wi-Fi，请检查网络和语音服务后重试。"}}}
```

顺序（与网页版定稿一致）：

1. `checking` 阶段先检查设备状态与本地音色记录。无云音色清理任务时可离线完成。
2. `deleting_cloud` 阶段清理本机云端复刻音色，`current/total` 报告进度。
3. `clearing_local` 和 `clearing_onboarding` 阶段清理本机音色记录与引导进度。
4. `clearing_wifi` 阶段最后清除 Wi-Fi，然后报告 `rebooting` 并重启。

云音色清理失败时保留 Wi-Fi 并报告 `failed`。已成功删除的云端条目不会恢复。
小程序在收到 `rebooting` 后报告成功。若蓝牙先断开，界面报告结果未确认。

## 7. 云能力（未接线）

`wzh-miniprogram/docs/cloud-api.md` 与 `services/cloud.js` 是早期云端占位，
当前小程序不调用这套接口。阿里云 Key 及上传云端的流程由设备持有和执行。
如未来改回“手机上传云端”，需要重新设计 `voice_create` 并升 proto。

## 8. 错误码

| code | 含义 | 小程序展示 |
| --- | --- | --- |
| 1000 | 通用错误/内部失败 | 使用设备 message |
| 1001 | 帧格式错误/超长 | 设备通信异常，请重试 |
| 1002 | 未知操作 | 设备不支持该操作 |
| 1003 | 参数错误 | 使用设备 message |
| 1004 | 未连接 | 设备未连接 |
| 1005 | Wi-Fi/网络错误 | 使用设备 message |
| 1007 | 复刻音色已满 | 复刻音色已满 |
| 1008 | 音色不存在 | 音色不存在 |
| 1009 | 超时/结果不确定 | 设备响应超时 |
| 1010 | 设备忙 | 设备忙，请稍后再试 |
| 1011 | 恢复出厂失败 | 使用设备 message |
| 1014 | 云能力未配置 | 该功能暂不可用 |

固件没有实现 1006（Wi-Fi 认证失败）和 1012（配对确认）；密码错误最终表现为
`wifi_state.failed`。

## 9. 固件实现要点

1. GATT 按第 2 节暴露；RX 支持 write/write-no-response；TX notify。
2. 按 `\n` 拼帧，处理跨 write 半帧；单帧上限 32768；非法 JSON 回 `1001`。
3. Wi-Fi 扫描/连接、录音、上传、恢复出厂等耗时操作先 accepted，再用事件报告。
4. 音色固定 6 个 + 复刻最多 10 个；`id` 持久化；删除当前音色回退内置第一个。
5. 恢复出厂严格按 6.5 顺序；失败保留 Wi-Fi。
6. BLE 不做账号登录；广播名/状态里不放 Wi-Fi 密码、token、云端 Key。
7. `proto=2`；新增字段只能追加，不能改已有字段含义。

## 10. 小程序侧验收清单

- [ ] 打开小程序自动扫描，能看到三款设备（名称/信号/图标正确）。
- [ ] 连接后进入详情页，状态、电量、温度、运行时长、Wi-Fi、音量、当前音色正确。
- [ ] 断开设备后 1.2s 回到连接页，提示“设备已断开”。
- [ ] 音量拖动 120ms 防抖后下发，设备状态回推后 UI 不回跳。
- [ ] 固定音色切换、试听、复刻音色切换都生效。
- [ ] 更改网络：扫描列表、5G 提示、密码 <8 位拦截、连接中转圈、成功关闭弹窗并刷新。
- [ ] 复刻录音：点开始后设备提示、设备确认录音结束后生成、列表刷新。
- [ ] 复刻上传：仅 WAV、5–20 秒、进度条、生成后列表刷新。
- [ ] 重命名、删除确认；删除当前音色后 UI 回到龙安欢。
- [ ] 恢复出厂：必须输入“确定恢复出厂设置”，成功后回连接页。
- [ ] 说明书、主题、明暗、布局、玻璃材质切换正常并持久化。
- [ ] 真机 iOS/Android 各跑一遍；Android 验证 MTU 分片，iOS 验证 20 字节分片。

## 11. Mock 模式

`services/config.js` 的 `USE_MOCK_IN_DEVTOOLS=true` 时，开发者工具自动使用
`services/mock.js`，不调用真实 BLE。Mock 实现本文全部 op（`voice_create` 返回
`1003`；上传/录音会延时添加克隆；`wifi_connect` 对 `Home-Guest` 开放网络跳过
密码长度校验）。

切真机：把 `USE_MOCK_IN_DEVTOOLS` 改为 `false`，或在手机上直接运行
（`platform()` 不是 `devtools` 时本来就用真实 `BleClient`）。

