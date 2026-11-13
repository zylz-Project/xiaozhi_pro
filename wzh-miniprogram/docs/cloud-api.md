# WZH 玩具 · 声音复刻云接口 v1

声音复刻的音频**不通过 BLE 传输**。小程序录手机麦克风或选择文件后，用 HTTPS 上传到
后端；后端完成降噪/克隆/TTS 合成，返回 `fileId` 与可播放的 `audioUrl`。BLE 只负责
把 `fileId` 注册到设备、同步列表和删除通知。

> 当前 `wzh-miniprogram/services/config.js` 的 `CLOUD_BASE` 是占位地址
> `https://wzh-api.example.com`，因此 `services/cloud.js` 的 `isEnabled()` 为 false，
> 小程序自动走 mock/本地逻辑。接入真实后端时改这里，并在微信公众平台配置
> request/uploadFile 合法域名。

## 1. 通用约定

- Base URL：`CLOUD_BASE`，例如 `https://api.your-domain.com`。
- 所有接口 HTTPS，JSON；上传接口 `multipart/form-data`。
- 响应统一包一层：

```json
{"ok":true,"data":{}}
{"ok":false,"error":{"code":"voice_full","message":"复刻音色已满"}}
```

- 鉴权：小程序启动时 `wx.login()` 拿 `code`，调 `POST /v1/auth/wx` 换短期 token；
  之后 `Authorization: Bearer <token>`。**没有登录页**，token 只用于云能力，不参与
  设备配对。
- 设备绑定：由 BLE 连接 + 硬件配对确认完成；云接口用 `deviceId`（设备广播名或固件
  序列号）关联，不信任客户端单方面传入的任意 ID。
- 时间：ISO 8601 UTC；文件 URL 建议 24h 过期，客户端每次列表刷新重新取。

## 2. 会话

### POST /v1/auth/wx

请求：

```json
{"code":"wx.login 返回的 code"}
```

响应：

```json
{"ok":true,"data":{"token":"...","expiresIn":7200}}
```

- token 存 `wx.setStorageSync('wzh-cloud-token')`；过期由后端返回 401，客户端重新 login。
- 不返回 openid/unionid 给小程序展示。

## 3. 上传音频

### POST /v1/voice/upload

`multipart/form-data`：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `file` | file | 是 | MP3/WAV/M4A；5–20 秒；≤ 10MB |
| `name` | string | 是 | 音色名，≤ 8 个字符 |
| `product` | string | 是 | `crawlPanda` / `dinosaur` / `redpanda` |
| `deviceId` | string | 是 | 设备广播名/序列号 |
| `source` | string | 是 | `record` / `upload` |
| `duration` | int | 否 | 秒 |

响应：

```json
{"ok":true,"data":{"fileId":"f_123","audioUrl":"https://cdn/.../f_123.mp3","expiresAt":"2026-11-12T00:00:00Z"}}
```

服务端校验：

- 只接受 5–20 秒、单声道/16k 以上采样；过短/过长返回 `audio_invalid`。
- 做静音检测/音量归一；失败返回 `audio_silent`。
- 文件类型按魔数校验，不只看扩展名。
- 每个设备每天上传次数限流（建议 20 次）。

## 4. 创建复刻音色

### POST /v1/voice/clone

```json
{"fileId":"f_123","name":"小满","product":"crawlPanda","deviceId":"CrawlPanda-1A2B"}
```

同步返回（小文件/快速克隆）：

```json
{"ok":true,"data":{"clone":{"id":"c_123","name":"小满","status":"ready","audioUrl":"https://cdn/.../c_123.mp3","date":"11-11"}}}
```

异步返回（推荐，克隆可能 10–60 秒）：

```json
{"ok":true,"data":{"clone":{"id":"c_123","name":"小满","status":"processing"}}}
```

### GET /v1/voice/clone/{id}

```json
{"ok":true,"data":{"clone":{"id":"c_123","name":"小满","status":"ready","audioUrl":"https://cdn/.../c_123.mp3"}}}
```

`status`：`processing` / `ready` / `failed`。`failed` 时带 `message`，小程序提示重试。

## 5. 列表、删除、试听

### GET /v1/voice/clones?deviceId=...

```json
{"ok":true,"data":{"clones":[{"id":"c_123","name":"小满","date":"11-11","audioUrl":"https://..."}],"capacity":10}}
```

### DELETE /v1/voice/clones/{id}

```json
{"ok":true,"data":{"deleted":true}}
```

- 删除是幂等的；不存在也返回 deleted=true。
- 删除前校验该音色属于当前 token 的设备绑定。

### POST /v1/voice/audition

```json
{"cloneId":"c_123"}
```

```json
{"ok":true,"data":{"audioUrl":"https://...","expiresAt":"..."}}
```

小程序用 `wx.createInnerAudioContext()` 播放；不把音频写进本地缓存。

## 6. 恢复出厂的云端顺序

设备执行 `reset_factory` 时，后端/固件按以下顺序：

1. 校验设备在线与绑定 Key；
2. 调 `DELETE /v1/voice/clones/{id}` 逐个删除该设备的云端复刻音色；
3. 删除设备本地记录；
4. 清 Wi-Fi、重启；
5. 任一步失败 → 中止，保留 Wi-Fi，返回 `reset_failed`。

> 云端 Key 只存在设备 NVS/后端，不下发给小程序。

## 7. 错误码（云）

| code | HTTP | 含义 |
| --- | --- | --- |
| `unauthorized` | 401 | token 失效，重新 login |
| `forbidden` | 403 | 设备不属于当前用户 |
| `audio_invalid` | 400 | 格式/时长不合法 |
| `audio_silent` | 400 | 静音/音量过低 |
| `voice_full` | 409 | 复刻音色已满（10 个） |
| `clone_failed` | 502 | 克隆服务失败 |
| `rate_limited` | 429 | 上传/创建过于频繁 |
| `internal` | 500 | 服务端错误 |

## 8. 隐私与合规

- 录音前必须有明确的用户授权（小程序 `scope.record`）；拒绝时仍可使用固定音色。
- 音频仅用于声音复刻，不用于训练通用模型；保留期建议 ≤ 30 天，删除音色即删除源文件。
- 隐私政策需写明：录音、上传、云端合成、删除方式；未成年人需监护人同意。
- 不把 Wi-Fi 密码、设备 token、云端 Key 写入日志/埋点。
- 上线前在微信公众平台完成：服务器域名配置、隐私协议、类目、备案（如适用）。

## 9. 小程序侧映射

| 小程序位置 | 云接口 |
| --- | --- |
| `services/cloud.js#ensureSession` | `POST /v1/auth/wx` |
| `services/cloud.js#uploadVoice` | `POST /v1/voice/upload` |
| `services/cloud.js#createClone` | `POST /v1/voice/clone` |
| `services/cloud.js#getClone` | `GET /v1/voice/clone/{id}` |
| `services/cloud.js#listClones` | `GET /v1/voice/clones` |
| `services/cloud.js#deleteClone` | `DELETE /v1/voice/clones/{id}` |
| `services/cloud.js#audition` | `POST /v1/voice/audition` |

`device.js#createVoice` 在 `cloud.isEnabled()` 为 true 时先上传拿 `fileId`，再把
`fileId` 放进 BLE `voice_create`；为 false 时走 mock/本地路径。
