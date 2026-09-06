# WZH 动作库与动作调用机制

> 适用板型：`wzh_crawlPanda` / `wzh_dinosaur` / `wzh_tailRedPanda`。
> 动作库 = 编译进固件的关键帧编舞数据 + 播放引擎 + 调度逻辑，全部在板级控制器里，没有独立组件。
> 三产品迁移矩阵见 `docs/wzh_migration.md`；本文讲"动作库长什么样、动作怎么被调起来"。

## 0. 一页速览

| | 爬爬熊猫 | 恐龙 | 摇尾小熊猫 |
|---|---|---|---|
| 控制器 | `main/boards/wzh_crawlPanda/panda_controller.cc` | `main/boards/wzh_dinosaur/dinosaur_controller.cc` | `main/boards/wzh_tailRedPanda/panda_controller.cc` |
| 动作任务名 | `panda_motion` | `dino_motion` | `panda_motion` |
| 轴数 | 3（head / left_arm / right_arm） | 5（neck_tilt / neck_lean / head_turn / tail_ud / tail_lr） | 3（head / tail_lr / tail_ud） |
| 表演剪辑 | 27 | 11 | 28 |
| 身体部位剪辑 | 6 | 7 | 5 |
| 待机素材 | 行为族 `idle` 4 条 | 6 个个性姿态 + 4 条脉冲 | 行为族 `idle` 4 条 |
| 插值方式 | 逐帧 ease_motion 缓动（非阻塞状态机） | 逐段 PlaySegment 阻塞插值 + 三档缓动 | 连续目标流 + 指数追踪（最顺滑） |
| 特殊机制 | ShuffleBag 洗牌、`settle` 收势 | `kKeep` 时序轴、情绪→动作映射 | FlowTarget 前瞻滑帧、指数追踪 |
| 声音联动 | 手臂交替扒动 + 呼吸起伏 | 尾巴/头随响度正弦摆 | 尾巴/头微幅跟随 |

## 1. 数据结构

### 1.1 Keyframe（关键帧）

```cpp
// 爬爬/小熊猫（3 轴，每帧一个 duration + 三个角度）
struct Keyframe { uint16_t duration_ms; int16_t a; int16_t b; int16_t c; };

// 恐龙（5 轴 + 每帧独立缓动 + kKeep 哨兵）
struct Keyframe {
    int neck_tilt, neck_lean, head_turn, tail_ud, tail_lr;
    uint16_t duration_ms;
    MotionEase ease;          // kSmooth 三次平滑 / kFastOut 快出慢停 / kLinear 匀速
};
constexpr int kKeep = 0x7FFFFFFF;   // 该轴沿用上一帧
```

角度都是**舵机直接角度**（0~180，90=中位），不是物理空间角。各板中位/限位不同：
爬爬默认坐姿 90/90/90；恐龙默认 neck_tilt=70、其余 90，tail_ud 限位 55~180；小熊猫 tail_ud 默认 150（尾巴上翘姿态）。

### 1.2 Clip（剪辑）与行为族

```cpp
struct Clip {
    const char* name;         // 日志用名字，如 "affection_hug"
    const Keyframe* frames;
    uint8_t count;
    bool mirrorable;          // 允许随机镜像（方向性动作置 false）
};

// 行为族表：PandaAction/情绪 → 行为族 → 随机挑一条具体剪辑（-1 为空位）
constexpr int8_t kFamily[kBehaviourCount][4] = {
    {C_IDLE_LISTEN, C_IDLE_PAW_ADJUST, C_IDLE_BREATH, C_IDLE_STRETCH},  // idle
    {C_CURIOUS_PEEK, C_CURIOUS_REACH, C_CURIOUS_DOUBLE_CHECK, -1},      // curious
    ...
};
```

恐龙不用行为族表：`DinoAction` 直接对应 `kActionClips[action]` 里的剪辑组，`PickClip` 随机取一条。

### 1.3 三产品剪辑清单

**爬爬熊猫（27 表演 + 6 身体）** `kClips[C_COUNT]` + `kBodyClips[BODY_COUNT]`：

| 行为族 | 剪辑 |
|---|---|
| idle(4) | idle_listen / idle_paw_adjust / idle_breath / idle_stretch |
| curious(3) | curious_peek / curious_reach / curious_double_check |
| eat(3) | eat_hold_bamboo / eat_chew / eat_paw_swap |
| baby(3) | baby_call / baby_paddle / baby_find_user |
| proud(3) | proud_push_up / proud_patrol / proud_call |
| affection(3) | affection_hug / affection_shy / affection_ask_up |
| playful(3) | playful_crawl / playful_paw_drum / playful_pounce |
| ambient(4) | ambient_listen / ambient_rain / ambient_explore / ambient_stretch |
| settle(1) | settle（收势，不可镜像） |
| body(6) | body_shake_head / body_look_left / body_look_right / body_wave_left / body_wave_right / body_raise_arms |

**恐龙（11 表演 + 7 身体 + 4 脉冲 + 6 待机姿态）**：

| 动作 (DinoAction) | 剪辑 | 说明 |
|---|---|---|
| discover(2) | discover-direct / discover-peek | 探索/窥探 |
| affection(2) | affection-bow / affection-nuzzle | 鞠躬/蹭脸 |
| happy(2) | happy-display / happy-chase | 摆身/大对角线追逐 |
| proud(1) | proud-call | 仰天长啸 |
| eat(1) | eat-peck | 多口进食 |
| listen(1) | listen-locate | 侧耳倾听 |
| startled(1) | startled-recoil | 缩颈警戒 |
| sleepy(1) | sleepy-nod | 低头伏睡 |
| body(7) | body-shake-head / body-look-left / body-look-right / body-look-up / body-look-down / body-wag-tail / body-twist-neck | 不镜像、不缩幅 |
| pulse(4) | pulse-glance / pulse-tail / pulse-perk / pulse-sniff | 待机生命脉冲 |
| idle(6 姿态) | 平静/警觉/困倦/好奇/放松/轻快 | `kIdlePoses`，缓慢漂移+呼吸正弦 |

**摇尾小熊猫（28 表演 + 5 身体）**：

| 行为族 | 剪辑 |
|---|---|
| idle(4) | idle_glance / idle_low_sweep / idle_perk / idle_circle |
| curious(3) | curious_peek / curious_track / curious_circle |
| eat(3) | eat_search / eat_chew / eat_delight |
| baby(3) | baby_seek / baby_wiggle / baby_comfort |
| proud(3) | proud_display / proud_patrol / proud_call |
| affection(3) | nuzzle / shy_approach / soft_curl |
| playful(3) | play_wag / play_chase / play_figure8 |
| ambient(4) | ambient_listen / ambient_rain / ambient_scent / ambient_stretch |
| settle(1) | settle |
| startled(1) | startled_flinch |
| body(5) | body_shake_head / body_look_left / body_look_right / body_wag_tail / body_twist_neck |

## 2. 动作调用链

```
入口（五类）                                去重/入队                 专用任务串行执行
──────────────                             ────────                 ───────────────
① MCP 工具    self.panda.action / body / react_to_user_emotion
② llm.emotion 事件   Display::SetEmotion 拦截 → HandleLlmEmotion → EmotionToAction
③ 用户语音文本兜底   Display::SetChatMessage 拦截 → HandleUserSpeechEmotion（关键词打分）
④ 设备状态切换       AddDeviceStateChangeListener → SetConversationActive
                     （进入对话→listen/安静；回 idle→恢复自主）
⑤ 离线自主调度       动作任务循环里定时随机
        │
        ▼
TriggerAction(action, with_local_sound)
   · 1.2s 内同类情绪直接合并（防服务端连发造成抽动）
   · ActionRequest{action, with_local_sound, is_body} → xQueueSend
   · 有界队列 4 槽；满了丢最旧，保证响应最新指令
        │
        ▼
动作任务（panda_motion / dino_motion，优先级 2，栈 5120）
   · 在线时情绪动作照做，但不播本地声音（with_local_sound 被
     conversation_active_ 过滤）
   · 离线时按动作匹配本地声音（PlayForAction / ClassifySound）+ 播剪辑
   · 空闲时叠加待机层（见 §5）
```

关键设计约束：

- **入口永远不碰舵机**。所有插值、vTaskDelay、LEDC 写入都在动作任务里串行执行，网络回调和主循环（`Application::Schedule` 所在任务）绝不被动作阻塞。
- **在线/离线的界线是 `conversation_active_`**（原子量），不是 Wi-Fi。connecting/listening/speaking/audio_testing = 在线；配网/升级/激活也置为在线（安静姿态）。
- 动作结束后推迟自主节奏和脉冲，避免"动作+脉冲"连轴转。

## 3. 播放引擎：三产品三种插值

同一份剪辑数据，三块板用了三种播放机制，是理解代码的关键差异。

### 3.1 爬爬熊猫：非阻塞状态机 + 统一缓动

- `StartClip(clip_id, now_ms)`：记录 `from_`（当前姿态）、帧 0、随机 tempo(0.88~1.14)、随机镜像（带 ±2 平衡，防长期偏一侧）。
- `UpdatePlayer(now_ms)` 每个任务循环调用：按帧时长推进帧号，用 `ease_motion(progress)` 在 `from_` → 当前帧目标之间插值。
- **非阻塞**：一帧插值瞬间完成，任务循环里同时叠加微动层和声音层（§5）。

### 3.2 恐龙：阻塞式逐段插值 + 三档缓动 + kKeep

- `RunAction`：`AttentionPose` 先短促看向用户建立互动 → 逐帧 `PlaySegment(from, target, duration, ease, living_phase)` → 收势 `settle` 回默认姿态。
- `PlaySegment` 内部按 `kTickMs` 步进，每步用该帧的 `ease`（kSmooth/kFastOut/kLinear）算插值量，**每帧叠加 `ApplyLiving` 呼吸微动**再 `SetPose`（统一限幅 + LEDC）。
- `kKeep` 是恐龙动物时序的核心：某轴写 kKeep 表示"这拍不动"，由此编出"头先动、脖子后随、尾巴最后响应"。爬爬/小熊猫 3 轴全动所以不需要。
- 恐龙每帧有独立缓动类型；爬爬/小熊猫全剪辑共用一个统一缓动函数。

### 3.3 摇尾小熊猫：连续目标流 + 指数追踪

- `FlowTarget`：帧进度走到最后 30% 时，目标提前滑向下一帧——动作之间**永不停顿、无接缝**。
- 目标流不是直接写舵机，而是**指数追踪**：`pose_ += (target - pose_) * k`，`k = 1 - exp(-20/tau)`、`tau = max(70, 帧时长/3)`。起止速度自然为零，软着陆无硬停。
- 这是三产品里最顺滑的实现，也是迁移阶段对齐参考工程 V3.3 的目标方向（参考工程用 Catmull-Rom 五轴）。

### 3.4 镜像与随机变奏（防复读）

| 机制 | 爬爬/小熊猫 | 恐龙 |
|---|---|---|
| tempo 随机 | 0.88~1.14 | 0.90~1.15 |
| 镜像 | clip.mirrorable 且带 ±2 平衡计数 | 每次动作随机 50% |
| 幅度 | 无（镜像即变奏） | 0.92~1.10 缩放（绕 90 中位） |
| 选剪辑 | ShuffleBag 洗牌（族内不放回，首项不与上次重复） | 纯随机 `esp_random() % count` |
| 身体部位动作 | 不镜像不随机（"向左看"必须真的向左） | 同 |

## 4. 声音联动（动作引擎读什么包络）

`wzh::MotionData`（`main/boards/common/wzh/wzh_audio_player.h`）是动作引擎能拿到的唯一"响度"：

```cpp
struct MotionData {
    bool playing; float level; float attack;
    uint32_t elapsed_ms, duration_ms; int sound_index;
};
```

本地声音没有真实 PCM 包络（解码和 I2S 输出都在 XiaoZhi AudioService），所以 **level/attack 由 Opus 包字节数估算**；在线 TTS 则由 `AudioService.SetOutputAudioLevelListener` 回调喂真实响度（`FeedTtsLevel`）。

各板应用层：
- 恐龙 `SetPose`：tail_lr 加 `sin(phase)·min(14°, level·26)` 正弦摆，attack>0.15 时头随机 ±4° 抖动；在线 TTS 压低幅度（×0.6/×0.45）避免音节尖峰抽动。
- 爬爬 `ApplyAudioMotion`：前肢交替扒动 + 呼吸起伏。
- 小熊猫 `ApplyAudioMotion`：尾巴/头微幅跟随。

离线声音与动作的**配对**：`ClassifySound` 按文件名关键词（吃竹/咀嚼/eat、宝宝/baby、撒娇、成年/威风……）+ 时长启发式（>6.5s→proud，<1.7s→curious）把声音归到行为族，然后同族剪辑与声音一起出。

## 5. 待机层与自主调度

动作任务空闲时的叠加结构（爬爬/小熊猫为微动层常驻，恐龙为逐层切换）：

```
关键帧剪辑（若有）→ + 微动层（呼吸/凝视漂移/尾巴轻摆/肌肉微颤，频率互质防机械同步）
                 → + 声音包络层（§4）
                 → SetPose（限幅 + LEDC 写入）
```

- **微动层**（`AddMicroMotion` / 恐龙 `ApplyLiving` / `RunIdlePose`）：呼吸正弦、头部凝视漂移（随机新目标 + 缓动逼近）、肌肉微颤。让静止的设备仍有生命感。
- **待机个性姿态**（恐龙专属）：6 个多轴坐姿每 4~8s 轮换，姿态用低通漂移（k=0.018）缓慢过渡过去。
- **生命脉冲**（恐龙专属）：每 5~14s 随机做一次瞥视/甩尾/嗅闻/精神一振，打破正弦的可预测性。
- **自主表演**：离线（= 不在对话中）时，恐龙每 5~13s、爬爬/小熊猫按 `next_idle_ms_ / next_sound_ms_ / next_ambient_ms_` 计时器随机表演并配本地声音；在线时全部暂停（只保留微动+脉冲小表情）。

## 6. MCP 工具与情绪映射

动作同时是 MCP 对外接口（`RegisterMcpTools`）：

| 工具 | 作用 | 备注 |
|---|---|---|
| `self.<product>.react_to_user_emotion` | 模型语义判断情绪 → EmotionToAction → TriggerAction | offline 时返回暂停 |
| `self.<product>.action` | 明确动作（8 种 PandaAction/DinoAction） | 离线时带本地声音 |
| `self.<product>.body` | 身体部位动作（爬爬 6 / 恐龙 7 / 小熊猫 5） | 在线也执行 |
| `self.<product>.get_mode` | 返回 online/offline | |

`EmotionToAction` 把 XiaoZhi `llm.emotion` 词表（happy/laughing/sad/angry/...）映射到 8 种动作；`HandleUserSpeechEmotion` 对用户转写文本做中文关键词打分兜底（llm.emotion 经常缺失），身体部位指令（"摇摇头/向左看/…"）优先级最高。

**改名约束**：`ParseAction` / `ActionName` / `BodyActionName` 与 MCP 工具的参数描述（服务端提示词）必须同步改。

## 7. 新增/修改动作的步骤

以恐龙为例（爬爬/小熊猫类似）：

1. 在 `dinosaur_controller.cc` 匿名命名空间写 `constexpr Keyframe kXxxFrames[]`。注意 kKeep、限位（tail_ud 55~180）和缓动选择；每条动作**必须以回到默认姿态的收势帧结尾**。
2. 剪辑数组追加到对应 `kXxxClips[]`，并同步更新 `kActionClips[]` 的 `{clips, count}`。
3. 若是新情绪：扩展 `DinoAction` 枚举 + `ParseAction` + `ActionName` + `EmotionToAction` + MCP 工具描述（提示词）。
4. `SetPose` 会兜底限幅，但编舞时应先保证角度在机械限位内，限位只作为最后防线。
5. 重新构建：`python3 scripts/build.py wzh_dinosaur --name wzh-dinosaur`（板名大小写敏感，三板不能并行）。

身体部位动作的 add 点在 `kBodyClips` + `DinoBodyAction` 枚举 + `ParseBodyAction`/`BodyActionName`，以及 `HandleUserSpeechEmotion` 的关键词表。

## 8. 与参考工程的差距（阶段 7-8 待办）

当前恐龙库是简化版（11 剪辑）。参考工程 `BASE/dinosaur/main/auto_run.cc`（V3.3，只读参考，勿改）有：

- 29 个互动变体 + 5 个环境场景 + 4 个待机生命动作；
- Catmull-Rom 五轴插值（比恐龙现在的逐段 SmoothStep 更平滑）;
- 表演签名（近期表演去重）。

阶段 7 计划先做统一行为调度器（有界优先级队列：安全 > 手动 > 明确动作 > 情绪 > 对话 > 音频 > 自主 > 微动作，含平滑打断、精力状态、低电量降级），阶段 8 再逐产品把动作专项对齐参考工程。当前三产品各自的调度（恐龙的定时随机/爬爬小熊猫的计时器组）将来都收拢进统一调度器。
