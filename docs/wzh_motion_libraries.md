# WZH 动作库与动作执行机制

2026-10-21：三款音画入口均更新为逐声音 32 种变化，对话 16 种组合；恐龙吼叫／取食语义姿态、大熊猫独立爪部及小熊猫连续高尾表达见[本轮说明](wzh_expressive_audio_2026-10-21.md)。下方早期数量与实现按日期视作历史记录。

2026-10-21 源码更新：三款关节指令分别扩展为 22／24／33 类，离线 MultiNet 与在线原生小智 MCP 使用同一份能力词表；大小熊猫头部工作范围改为 30°～150°并软压缩大角度。未编译、未烧录；当前口令、接口和验证见[关节控制说明](wzh_joint_voice_2026-10-21.md)。下方旧计数以该说明为准。

2026-10-19 最新：三款常规池76／80／75段，互动各55个入口；本地录音8种变化、对话6种语句组合，详见[灵动动作更新](wzh_lively_motion_2026-10-19.md)。下方早期资料保留为历史。

2026-10-19 当前库：完整迁入参考工程 86 段编舞，新增 24 段故事动作，并接入第四套触摸/姿态回应。三款普通行为池分别为 68／72／67 段，互动各 44 个入口；当前数量、幅度与节奏以[本次迁移说明](wzh_reference_motion_2026-10-19.md)为准。下方早期数量和三套互动说明保留为历史。

爬爬熊猫后续整套重编排：73 组关键帧、74 个入口，头爪错拍、爬行约120°回看、平滑语音回应，见[当前爬爬熊猫编舞](wzh_crawl_lively_2026-10-16.md)。另外两款保留上一轮快动作。

当前节奏与幅度以[参考爬行及三款快动作](wzh_fast_motion_2026-10-16.md)为准：800／1200ms 连续爬行，通用输出上限 400°/s，更大目标与更短停顿；下文早期速度和爬行回看参数为历史。动作库数量和互动选择规则保留。

整机调度更新：普通情绪不反复重启动作、显式互动可打断、退出会话取消旧请求、关机有限收尾，见[产品流程优化](wzh_product_polish_2026-10-16.md)。

最新动作库：三款合计新增 78 段，每类互动三套独立洗牌，扩展完整与短声性格编舞；当前数量与选择规则以[动作库扩展](wzh_rich_motion_2026-10-16.md)为准。

后续流畅度更新：关键帧衔接、较快节奏、浮点 PWM、自身震动过滤与精简日志以[当前修正说明](wzh_fluent_motion_2026-10-16.md)为准；下文旧节奏参数保留为历史记录。

2026-10-16 更新：三款全面扩大动作，熊猫目标放大 1.60 倍，小熊猫改为到达关键帧后切换；爬爬熊猫增加有限交替/同步爬行；恐龙普通表演 1.35～1.50 倍。当前机制以[本次动作说明](wzh_large_motion_2026-10-16.md)为准，下文旧 FlowTarget/指数追踪与缩幅参数为历史机制。

> 适用板型：`wzh_crawlPanda` / `wzh_dinosaur` / `wzh_tailRedPanda`。
> 动作库 = 编译进固件的关键帧编舞数据 + 播放引擎 + 调度逻辑，全部在板级控制器里，没有独立组件。
> 三产品迁移矩阵见 `docs/wzh_migration.md`；本文讲"动作库长什么样、动作从触发到舵机转动的完整执行路径"。

## 0. 一页速览

|              | 爬爬熊猫                                         | 恐龙                                                       | 摇尾小熊猫                                         |
| ------------ | ------------------------------------------------ | ---------------------------------------------------------- | -------------------------------------------------- |
| 控制器       | `main/boards/wzh_crawlPanda/panda_controller.cc` | `main/boards/wzh_dinosaur/dinosaur_controller.cc`          | `main/boards/wzh_tailRedPanda/panda_controller.cc` |
| 动作任务名   | `panda_motion`                                   | `dino_motion`                                              | `panda_motion`                                     |
| 轴数         | 3（head / left_arm / right_arm）                 | 5（neck_tilt / neck_lean / head_turn / tail_ud / tail_lr） | 3（head / tail_lr / tail_ud）                      |
| 表演剪辑     | 27                                               | 24 条完整 + 24 条 Flash 短声专属 + 7 条怒吼                | 28                                                 |
| 身体部位剪辑 | 6                                                | 7                                                          | 5                                                  |
| 待机素材     | 行为族`idle` 4 条                                | 6 个个性姿态 + 8 条脉冲                                    | 行为族`idle` 4 条                                  |
| 插值方式     | 逐帧 ease_motion 缓动（非阻塞状态机）            | 逐段 PlaySegment 阻塞插值 + 三档缓动                       | 连续目标流 + 指数追踪（最顺滑）                    |
| 特殊机制     | ShuffleBag 洗牌、`settle` 收势                   | 同帧五轴同步、统一慢速限制、情绪→动作映射                  | FlowTarget 前瞻滑帧、指数追踪                      |
| 声音联动     | 手臂交替扒动 + 呼吸起伏                          | Flash 先选声再选动作 + 五轴真实 PCM 包络                    | 尾巴/头微幅跟随                                    |

三块板共同点：50Hz LEDC PWM 舵机、20ms 一拍（`kTickMs=20`）的专用动作任务、入口只投队列不碰舵机。

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
爬爬默认坐姿 90/90/90，头部限位 30~150、双爪 8~172；恐龙默认 neck_tilt=70、其余 90，tail_ud 限位 55~180；小熊猫默认 90/90/90，头部限位 30~150、尾巴 LR 为 6~174、UD 为 3~177（内部动作库仍用 150° 中心，输出时整库映射到 90°）。

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

| 行为族       | 剪辑                                                                                                    |
| ------------ | ------------------------------------------------------------------------------------------------------- |
| idle(4)      | idle_listen / idle_paw_adjust / idle_breath / idle_stretch                                              |
| curious(3)   | curious_peek / curious_reach / curious_double_check                                                     |
| eat(3)       | eat_hold_bamboo / eat_chew / eat_paw_swap                                                               |
| baby(3)      | baby_call / baby_paddle / baby_find_user                                                                |
| proud(3)     | proud_push_up / proud_patrol / proud_call                                                               |
| affection(3) | affection_hug / affection_shy / affection_ask_up                                                        |
| playful(3)   | playful_crawl / playful_paw_drum / playful_pounce                                                       |
| ambient(4)   | ambient_listen / ambient_rain / ambient_explore / ambient_stretch                                       |
| settle(1)    | settle（收势，不可镜像）                                                                                |
| body(6)      | body_shake_head / body_look_left / body_look_right / body_wave_left / body_wave_right / body_raise_arms |

**恐龙（11 表演 + 7 身体 + 4 脉冲 + 6 待机姿态）**：

| 动作 (DinoAction) | 剪辑                                                                                                                 | 说明                            |
| ----------------- | -------------------------------------------------------------------------------------------------------------------- | ------------------------------- |
| discover(2)       | discover-direct / discover-peek                                                                                      | 探索/窥探                       |
| affection(2)      | affection-bow / affection-nuzzle                                                                                     | 鞠躬/蹭脸                       |
| happy(2)          | happy-display / happy-chase                                                                                          | 摆身/大对角线追逐               |
| proud(1)          | proud-call                                                                                                           | 仰天长啸                        |
| eat(1)            | eat-peck                                                                                                             | 多口进食                        |
| listen(1)         | listen-locate                                                                                                        | 侧耳倾听                        |
| startled(1)       | startled-recoil                                                                                                      | 缩颈警戒                        |
| sleepy(1)         | sleepy-nod                                                                                                           | 低头伏睡                        |
| body(7)           | body-shake-head / body-look-left / body-look-right / body-look-up / body-look-down / body-wag-tail / body-twist-neck | 不镜像、不缩幅                  |
| pulse(4)          | pulse-glance / pulse-tail / pulse-perk / pulse-sniff                                                                 | 待机生命脉冲                    |
| idle(6 姿态)      | 平静/警觉/困倦/好奇/放松/轻快                                                                                        | `kIdlePoses`，缓慢漂移+呼吸正弦 |

**摇尾小熊猫（28 表演 + 5 身体）**：

| 行为族       | 剪辑                                                                                 |
| ------------ | ------------------------------------------------------------------------------------ |
| idle(4)      | idle_glance / idle_low_sweep / idle_perk / idle_circle                               |
| curious(3)   | curious_peek / curious_track / curious_circle                                        |
| eat(3)       | eat_search / eat_chew / eat_delight                                                  |
| baby(3)      | baby_seek / baby_wiggle / baby_comfort                                               |
| proud(3)     | proud_display / proud_patrol / proud_call                                            |
| affection(3) | nuzzle / shy_approach / soft_curl                                                    |
| playful(3)   | play_wag / play_chase / play_figure8                                                 |
| ambient(4)   | ambient_listen / ambient_rain / ambient_scent / ambient_stretch                      |
| settle(1)    | settle                                                                               |
| startled(1)  | startled_flinch                                                                      |
| body(5)      | body_shake_head / body_look_left / body_look_right / body_wag_tail / body_twist_neck |

## 2. 动作执行全链路（从触发到舵机转动）

一条动作指令要经过**四段**：入口投递 → 动作任务取件 → 逐拍插值 → 姿态合成输出。

### 2.1 第一段：入口投递

五类触发源，全部收敛到 `TriggerAction()` / `TriggerBodyAction()`：

| 来源           | 代码路径                                                                                              |
| -------------- | ----------------------------------------------------------------------------------------------------- |
| ① MCP 工具     | `self.<product>.action` / `body` / `react_to_user_emotion` 的工具回调                                  |
| ② llm.emotion  | 板级 `Display::SetEmotion` 重载 → `HandleLlmEmotion` → `EmotionToAction`                              |
| ③ 语音文本兜底 | 板级 `Display::SetChatMessage` 重载 → `HandleUserSpeechEmotion`（中文关键词打分，身体指令优先级最高） |
| ④ 设备状态切换 | `Application::AddDeviceStateChangeListener` → `SetConversationActive`（进入对话→listen/安静；回 idle→恢复自主） |
| ⑤ 离线自主     | 动作任务循环自己的定时器（§4）                                                                         |

`TriggerAction` 做两件事：

```cpp
// 1) 1.2s 内的同类情绪直接合并 —— 服务端一次状态更新常连发多帧
//    llm.emotion，不去重会让队列堆积、设备抽动。
if (action == last_action_ && now - last_action_time_us_ < 1200000) return true;
// 2) 投递 ActionRequest{action, with_local_sound, is_body} 到 4 槽有界队列；
//    队列满时丢最旧一条再投 —— 保证响应的是用户最新意图。
```

**这一步绝不碰舵机、不延时**。MCP/情绪事件跑在协议回调线程上，主循环（`Application::Schedule` 所在任务）绝不被动作阻塞 —— 这是整个设计的第一约束。

### 2.2 第二段：动作任务取件

初始化时创建专用 FreeRTOS 任务（`panda_motion` / `dino_motion`，优先级 2、栈 5120 字），主循环每 **20ms 一拍**。每拍结构：

```
1. xQueueReceive(action_queue_, timeout=0)      // 非阻塞探一次队列
   有请求 → 执行（含可选本地声音），并重置自主调度定时器
   无请求 → 走自主调度（离线随机表演 / 待机层）
2. 更新姿态（插值层 + 微动层 + 音频包络层）
3. 写舵机
4. vTaskDelay(20ms)
```

在线/离线每拍读一次：`conversation_active_`（原子量）= true 即在线。在线时情绪动作照常执行，但 `with_local_sound` 被过滤（不播本地声音，避免与 TTS 抢扬声器）。

### 2.3 第三段：逐拍插值（三种执行模型）

同一份数据，三块板用了三种执行模型 —— 这是读懂三份控制器的核心差异。

#### 2.3.1 爬爬熊猫：非阻塞状态机

剪辑"播放"不是阻塞函数，而是**状态机的逐步推进**。`StartClip()` 只设状态，真正的推进在每拍的 `UpdatePlayer(now_ms)`：

```cpp
void StartClip(int clip_id, uint32_t now_ms) {
    from_ = pose_;                  // 起手姿态 = 当前姿态
    clip_ = clip; frame_ = 0;
    tempo_ = frand(0.88, 1.14);     // 随机节奏：同一剪辑每次快慢不同
    // 随机镜像：clip.mirrorable 才允许；±2 平衡计数防长期偏一侧
    frame_duration_ms_ = max(180, frames[0].duration_ms * tempo_);
}

void UpdatePlayer(uint32_t now_ms) {           // 每拍调用
    // 1) 帧推进：按真实时钟累计，一帧播完自动跨到下一帧
    while (now_ms - frame_started_ms_ >= frame_duration_ms_) {
        pose_ = TargetPose(*clip_, frame_, mirror_);   // 整帧到达，吸附到目标
        from_ = pose_;
        frame_started_ms_ += frame_duration_ms_;
        if (++frame_ >= clip_->count) { clip_active_ = false; return; }
        frame_duration_ms_ = max(180, frames[frame_].duration_ms * tempo_);
    }
    // 2) 帧内插值：from_ → 当前帧目标，五次多项式缓动
    const float mix = ease_motion((now_ms - frame_started_ms_) / frame_duration_ms_);
    pose_.head     = from_.head     + (target.head     - from_.head)     * mix;
    pose_.left_arm = from_.left_arm + (target.left_arm - from_.left_arm) * mix;
    ...
}
// ease_motion(t) = t³·(t·(6t−15)+10)  —— 五次多项式，起止速度/加速度均为零
```

因为不阻塞，同一拍里任务还能叠加音频包络层并输出（§2.4），**一个任务同时跑"剪辑播放 + 跟声舞动 + 待机调度"三件事**。

#### 2.3.2 恐龙：阻塞式逐段插值

恐龙的 `RunAction()` 是**一个阻塞函数**，在动作任务里串行执行（动作期间队列不消费 —— 动作是原子的）：

```cpp
void RunAction(DinoAction action) {
    // 随机变奏：镜像 50%、幅度 0.92~1.10（绕 90 中位缩放）、节奏 0.90~1.15
    const Pose pose = AttentionPose(action, mirror);   // 先短促看向用户 240ms
    PlaySegment(current_pose_, pose, 240, kSmooth, living_phase);

    for (每帧 key in clip) {
        target = ResolveKeyframe(key, pose, mirror, amplitude);
        //    ↑ kKeep 轴保持上一帧值；非 kKeep 轴绕 90 缩放 amplitude
        PlaySegment(pose, target, key.duration_ms / tempo, key.ease, phase);
        pose = target;
    }
    PlaySegment(pose, 默认姿态, 500ms, kSmooth);       // 收势，不硬停
}

void PlaySegment(from, to, duration_ms, ease, living_phase) {
    steps = duration_ms / 20;
    for (step = 1..steps) {
        amount = EaseValue(ease, step / steps);        // 该帧专属缓动
        pose = from + (to - from) * amount;            // 五轴线性外推
        ApplyLiving(pose, living_phase);               // 叠加呼吸微动
        SetPose(pose);                                 // 限幅 + 写 LEDC
        vTaskDelay(20ms);                              // 精确 50Hz 步进
    }
}
```

关键点：

- **`kKeep` 只表示保持该轴**：同一关键帧中所有需要变化的轴共用同一个插值进度，同步开始、同步到达，不制造头、颈、尾的人工延迟。
- **连续缓动**：单段使用原有三次 SmoothStep；连续同向的 kSmooth 段使用夹紧单调 Hermite 切线连续穿过中间姿态，只在反向、定格和首尾自然减速，且不会超出相邻角度；kFastOut 仍保留少量提前到位感。
- `ApplyLiving` 叠加舒缓的低频呼吸/重心变化，并在每段落点归零；每段位移至少 420ms，最快轴平均速度不超过约 80°/s。声音偏移只影响 PWM 输出，不写回基础姿态。

#### 2.3.3 摇尾小熊猫：连续目标流 + 指数追踪

同样是状态机，但插值两处升级（三产品里最顺滑）：

```cpp
// ① FlowTarget：帧进度的最后 30% 提前滑向下一帧 → 相邻帧之间无接缝停顿
Pose FlowTarget(clip, frame, progress, mirror) {
    const Pose cur = TargetPose(clip, frame);
    const Pose nxt = TargetPose(clip, min(frame+1, count-1));
    const float lead = clamp((progress - 0.70) / 0.30, 0, 1);   // 最后 30%
    return LerpPose(cur, nxt, smoothstep(lead));
}

// ② 指数追踪：目标流不直接写舵机，姿态持续逼近目标流
const uint32_t tau = max(70, frame_duration_ms_ / 3);          // 响应速度
const float k = 1 - exp(-20 / tau);                             // 每拍逼近比例
pose_.head    += (target.head    - pose_.head)    * k;
pose_.tail_lr += (target.tail_lr - pose_.tail_lr) * k;
pose_.tail_ud += (target.tail_ud - pose_.tail_ud) * k;
```

指数追踪天然起止速度为零、软着陆无硬停；与小熊猫参考工程（`BASE/tailRedPanda/main/auto_run.cc`）逐行同源，也是迁移阶段 8 对齐的方向。

### 2.4 第四段：姿态合成与舵机输出

每拍把三层叠加，统一限幅，写 PWM：

```
插值层（剪辑关键帧插值结果 pose_）
  + 微动层（呼吸/凝视漂移/尾巴轻摆/肌肉微颤 —— 小熊猫 AddMicroMotion / 恐龙 ApplyLiving）
  + 音频包络层（ApplyAudioMotion —— §3）
  ↓
OutputPose / SetPose：clamp 到各轴工作限位（大小熊猫头部 30~150、爬爬双爪 8~172、恐龙 tail_ud 55~180 …）
  ↓
AngleToDuty：角度 → PWM 占空比
  pulse_us = 500 + angle × 2000 / 180      // 0°→500µs, 90°→1500µs, 180°→2500µs
  duty = pulse_us × (2¹³−1) / 20000        // 50Hz 周期 20ms，13-bit 分辨率
  ↓
ledc_set_duty + ledc_update_duty（每轴一次）
```

**角度永远是舵机直接角度**：镜像 = `180 − angle`（横向轴），幅度缩放 = `90 + (angle−90)·amplitude`。

### 2.5 镜像与随机变奏（防复读）

| 机制         | 爬爬/小熊猫                                     | 恐龙                         |
| ------------ | ----------------------------------------------- | ---------------------------- |
| tempo 随机   | 0.88~1.14                                       | 完整 0.88~1.00；Flash 短声 0.94~1.02 |
| 镜像         | clip.mirrorable 且带 ±2 平衡计数                | 每次动作随机 50%             |
| 幅度         | 无（镜像即变奏）                                | 0.92~1.10 缩放（绕 90 中位） |
| 选剪辑       | ShuffleBag 洗牌（族内不放回，首项不与上次重复） | 纯随机`esp_random() % count` |
| 身体部位动作 | 不镜像不随机（"向左看"必须真的向左）            | 同                           |

## 3. 音频包络：动作怎么"跟着声音动"

动作引擎能拿到的唯一响度来源是 `wzh::MotionData`（`main/boards/common/wzh/wzh_audio_player.h`）：

```cpp
struct MotionData { bool playing; float level; float attack; uint32_t elapsed_ms, duration_ms; int sound_index; };
```

### 3.1 level/attack 的产生（共享层 `AudioPlayer::UpdateEnvelope`）

共享 `MotionData` 会先用 **Opus 包字节数估算**本地声音包络：

```cpp
// 每 60ms 的 Opus 帧：包越大 ≈ 越响
kbps = packet_bytes × 8 × 1000 / 60;
raw  = clamp(kbps / kLoudnessRefBytes, 0, 1);
fast += (raw − fast) × (raw > fast ? 0.48 : 0.12);   // 快包络：快起慢落
slow += (raw − slow) × 0.035;                        // 慢包络：整体趋势
level  = slow;
attack = clamp((fast − slow × 1.08) × 3.2, 0, 1);    // onset 检测：快包络突起
```

恐龙还在 `AudioService` 扬声器输出端接入真实 PCM 回调，因此在线 TTS、Flash 音频和系统提示音最终都使用真实输出响度；Opus 估算只负责 PCM 尚未到达时的短暂预热。

### 3.2 各板怎么用包络

- **爬爬** `ApplyAudioMotion`（对齐旧工程 apply_audio_motion）：`attack>0.16` 且距上次 onset>320ms → 触发一次交替扒动（头 accent ±5~15°，onset 后 160~320ms 手臂交替下沉）；慢包络驱动呼吸起伏 `sin(phase)·min(5°, level·9)`，相位推进速度随 level 变化（响就喘得快）；无声时 accent 按 0.88~0.955 指数衰减归零。
- **恐龙** `SetPose`：五轴读取同一份低通过的 PCM 包络并同步响应，单段声音保持同一个随机观察方向；相位推进很慢，attack 只形成柔和抬头，不逐音节抽动。
- **小熊猫** `ApplyAudioMotion`：onset 驱动尾巴甩动 accent，慢包络驱动尾巴/头浮沉；微动层 life 系数在播声音时降到 0.8，尾巴仍持续做小 8 字摆动，不会因为说话而停住。

### 3.3 声音与动作的配对（声音驱动选动作，不是动作选声音）

```
声音开始播放 → motion.playing false→true
  → ClassifySound(index)：文件名关键词（吃竹/咀嚼/eat、宝宝/baby、撒娇、成年/威风…）
    + 时长启发式（>6500ms→proud/baby，<1700ms→curious）归类行为族
  → StartClip(ChooseClip(该行为族))        // 同族剪辑和声音一起出
声音播完 → playing true→false → StartClip(settle 收势) + 安排下一次声音
```

## 4. 待机与自主调度

### 4.1 爬爬/小熊猫：单循环多计时器

`Task()` 离线分支同时管理三组定时器 + 声音状态机：

- 播声音中：声音状态机驱动（§3.3），剪辑跟声音族走；
- 声音结束：settle 收势，`next_sound_ms_` = 20~40s 后；
- 空闲到 `next_idle_ms_`：随机 idle 族剪辑（4.8~10s 一次）；
- 到 `next_sound_ms_`（默认 6~12s）：随机 animal 声音；
- 到 `next_ambient_ms_`（默认 5 分钟）：随机 ambient 声音（之后 8~13 分钟一次）。

在线分支反之：不自主表演，情绪剪辑播完自动接一条 ambient 安静剪辑（避免动作停在奇怪姿态），TTS 包络叠加舞动。

### 4.2 恐龙：阻塞优先 + 空闲层

```
队列有请求 → RunAction / RunBodyAction（阻塞执行，动作原子）
队列空 & 离线 & 到点 → 先随机选 Flash animal 声音，再按文件名/时长选择动作，
                       每 5~13s 一次；Flash 不可用才随机无声动作
否则 → 待机层（非阻塞）：
  - RunIdlePose：偏姿 3~5s、默认中位 9~15s；12s 呼吸周期最后约 4s 回中停顿
  - 每 5~14s 随机 RunPulse（8 种生命小表情，相邻不重复）
```

待机层是非阻塞的，明确动作是阻塞的 —— 待机中被队列请求打断时，当前插值段在下一个 20ms 边界自然交接给新动作的 AttentionPose。

### 4.3 在线/离线行为总表

| | 在线（connecting/listening/speaking/audio_testing） | 离线（idle，含 Wi-Fi 已连但没对话） |
| --- | --- | --- |
| 情绪/MCP 动作 | 执行，不带本地声音 | 执行，带本地声音 |
| 自主表演 | 停止（恐龙只保留脉冲小表情） | 持续（声音驱动 + 定时器） |
| 本地声音 | 不播（进对话时 StopPlayback 清场） | 播放并驱动舞动 |
| 声音包络来源 | 扬声器真实 PCM | 扬声器真实 PCM（Opus 估算仅预热） |

## 5. MCP 工具与情绪映射

| 工具                                   | 作用                                               | 备注               |
| -------------------------------------- | -------------------------------------------------- | ------------------ |
| `self.<product>.react_to_user_emotion` | 模型语义判断情绪 → EmotionToAction → TriggerAction | offline 时返回暂停 |
| `self.<product>.action`                | 明确动作（8 种 PandaAction/DinoAction）            | 离线时带本地声音   |
| `self.<product>.body`                  | 身体部位动作（爬爬 6 / 恐龙 7 / 小熊猫 5）         | 在线也执行         |
| `self.<product>.get_mode`              | 返回 online/offline                                |                    |

`EmotionToAction` 把 XiaoZhi `llm.emotion` 词表（happy/laughing/sad/angry/...）映射到 8 种动作；`HandleUserSpeechEmotion` 对用户转写文本做中文关键词打分兜底（llm.emotion 经常缺失）。爬爬/小熊猫的 `PandaAction` 还要经 `ActionToBehaviour` 折算到行为族再挑剪辑（如 happy→playful、sleepy→settle）。

**改名约束**：`ParseAction` / `ActionName` / `BodyActionName` 与 MCP 工具的参数描述（服务端提示词）必须同步改。

## 6. 新增/修改动作的步骤

以恐龙为例（爬爬/小熊猫类似）：

1. 在控制器 `.cc` 匿名命名空间写 `constexpr Keyframe kXxxFrames[]`。注意 kKeep、限位（恐龙 tail_ud 55~180）和缓动选择；每条动作**必须以回到默认姿态的收势帧结尾**。
2. 剪辑数组追加到对应 `kXxxClips[]`，并同步更新 `kActionClips[]` 的 `{clips, count}`。
3. 若是新情绪：扩展动作枚举 + `ParseAction` + `ActionName` + `EmotionToAction` + MCP 工具描述（提示词）。
4. 限幅是最后防线：编舞时应先保证角度在机械限位内，不要依赖 `SetPose` 兜底。
5. 重新构建：`python3 scripts/build.py wzh_dinosaur --name wzh-dinosaur`（板名大小写敏感，三板不能并行）。

身体部位动作的 add 点在 `kBodyClips` + 身体动作枚举 + `ParseBodyAction`/`BodyActionName`，以及 `HandleUserSpeechEmotion` 的关键词表。

## 7. 与参考工程的对比（阶段 7-8 待办）

参考工程 = `BASE/<product>/main/auto_run.cc`（V3.3，只读，勿改）。三个产品逐一对：

### 7.1 爬爬熊猫 / 摇尾小熊猫 —— 方法一致（忠实移植）

- 爬爬：数据结构（Keyframe/Clip/mirrorable）、ease_motion 五次多项式、kFamily 行为族、ShuffleBag、计时器组调度（next_sound/next_idle/next_ambient/was_playing 声音状态机）与参考 `crawlPanda/main/auto_run.cc`（368 行）逐项对应。
- 小熊猫：FlowTarget（lead=(progress−0.70)/0.30）、指数追踪 k=1−exp(−20/τ)、mirror_balance ±2、tempo frand(0.88,1.14) 与参考 `tailRedPanda/main/auto_run.cc`（423 行）逐项对应。
- 差异仅在宿主环境：小智侧改用 `wzh::AudioPlayer`/`AudioService` 播声音、包络来自 Opus 包估算 + 在线 TTS 真实响度；参考工程用自己的 I2S 播放任务和 IsAudioPlaying 布尔量。**参考工程没有"随声音响度舞动"这层，小智侧是新增增强。**

### 7.2 恐龙 —— 同源但引擎简化，差距集中在这里

参考 `dinosaur/main/auto_run.cc`（独立引擎和 `auto_run_data.cc` 数据表）vs 小智侧 `dinosaur_controller.cc`（数据引擎一体）。**相同的骨架**：kKeep 哨兵、Ease 三档（枚举同源）、8 动作枚举 + ClipSet 池 + 20ms 拍、AttentionPose、收势回正、声音文件名→动作匹配。小智侧还增加了容量 1 的最新意图队列、统一慢速限制、24 条完整编舞、24 条 Flash 短声专属编舞、7 条按时长选择的怒吼变体、同族防连续复读，以及 Flash 先选声再选动作。移植旧动作时只有 `tail_ud` 使用 `180-angle` 反向换算，其他四轴不反向；新增短声轨迹则改成同帧五轴同步起停。**仍缺的机制**（按影响排序）：

| 参考工程机制 | 作用 | 小智侧现状 |
| --- | --- | --- |
| 30 条剪辑（每动作 3~4 变体） | 动作库本体（`auto_run_data.cc`，数据/引擎分离） | 24 条完整 + 24 条短声专属 + 7 条怒吼变体，数据引擎混在一文件 |
| Catmull-Rom 四点插值（EASE_SMOOTH 走 `catmull_pose`，前后帧都参与） | 相邻段曲线连续，无折角 | 夹紧单调 Hermite 切线：同向段连续速度，反向/定格自然停，不过冲 |
| 每帧独立 tempo（variation_seed 哈希 → 118%~154%/帧） | 同一剪辑每次表演节奏逐帧不同 | 单一 clip 级 tempo（完整 0.88~1.00；短声 0.94~1.02） |
| 动作专属 intensity（1.06~1.47）+ head 轴上限 1.12 | 大动作（happy/startled）视觉冲击更强 | 统一 0.92~1.10 |
| TailStyle(7)/NeckStyle(5)/HeadStyle(4) 空间风格随机 + 按动作加权（choose_tail_style 防三连同款） | 同一剪辑叠加不同空间轨迹，重复感低 | 无（轨迹完全由关键帧决定） |
| organic_sin 波形（sin+0.12sin2φ+0.05sin3φ 谐波） | 待机/表演微动更有机 | 纯 sin 微动 |
| story_envelope（sin(progress·π) 开根）+ neck/tail orbit 椭圆 | 动作有"叙事弧"：中段幅度大首尾收敛 | living 微动恒幅 |
| **表演签名**（recent_signatures[12] 哈希 clip+mirror+三风格，撞签重掷） | 不重复"骨架相同"的表演 | 只去重剪辑首项（ShuffleBag/avoid） |
| `continue_with_audio` 剪辑标志（proud 6.9s 叫声配多段连续表演） | 长声音不停手、不回呆站 | 部分具备：完整编舞按 Flash 时长向慢方向拉伸，但不会串接第二段 |
| 6 个 BehaviorParam 待机个性（center/amplitude/period 五轴参数表） | 待机姿态+频率都可配置 | 6 个姿态 + 固定正弦 |
| 5 个自然场景（NATURE_HORIZON_SCAN/RAIN_LISTEN/LOW_DRINK/LOOK_UP/RAIN_SHAKE，staged_ramp/pulse 时间线编排 + 洗牌） | 5~9s 的环境化长表演（配 ambient 声） | 无 |
| PoseFade rejoin（800ms 渐变回待机） | 表演结束不跳变 | 760~1000ms 平滑回默认姿态，再从当前姿态重建待机漂移 |
| g_tail_lr_bias 左右停留面积记忆（6.7s 时间常数）→ 镜像概率校正 + 输出端 ±22% 反偏 | 防止长期偏一侧 | 仅 mirror_balance 计数 |
| g_hard_swing 调试模式 | 真机校准用 | 无 |
| tail_ud 编码 0=上/125=下，输出时 `SERVO_MAX_ANGLE − pose.tail_ud` 转换 | 编舞角度语义化 | 直接舵机角 55~180（迁移时注意换算） |

参考工程**没有**而小智侧已有的：AudioService 扬声器真实 PCM 包络驱动五轴同步动作（覆盖在线 TTS、Flash 音频和系统提示音）——这是小智侧新增增强；参考工程只用了 IsAudioPlaying 布尔量做门控。

### 7.3 阶段 7-8 落点

阶段 7 先做统一行为调度器（有界优先级队列：安全 > 手动 > 明确动作 > 情绪 > 对话 > 音频 > 自主 > 微动作，含平滑打断、精力状态、低电量降级），三产品各自的调度收拢进来。阶段 8 恐龙对齐参考工程：按上表把**数据/引擎分离 + 30 剪辑 + Catmull-Rom + 三风格随机 + 表演签名 + continue_with_audio + 自然场景**逐项移植（顺序即影响排序）；爬爬/小熊猫只需把新增机制（如签名去重）回补。
