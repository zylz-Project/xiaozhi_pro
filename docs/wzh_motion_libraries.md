# WZH 动作库与动作执行机制

> 适用板型：`wzh_crawlPanda` / `wzh_dinosaur` / `wzh_tailRedPanda`。
> 动作库 = 编译进固件的关键帧编舞数据 + 播放引擎 + 调度逻辑，全部在板级控制器里，没有独立组件。
> 三产品迁移矩阵见 `docs/wzh_migration.md`；本文讲"动作库长什么样、动作从触发到舵机转动的完整执行路径"。

## 0. 一页速览

|              | 爬爬熊猫                                         | 恐龙                                                       | 摇尾小熊猫                                         |
| ------------ | ------------------------------------------------ | ---------------------------------------------------------- | -------------------------------------------------- |
| 控制器       | `main/boards/wzh_crawlPanda/panda_controller.cc` | `main/boards/wzh_dinosaur/dinosaur_controller.cc`          | `main/boards/wzh_tailRedPanda/panda_controller.cc` |
| 动作任务名   | `panda_motion`                                   | `dino_motion`                                              | `panda_motion`                                     |
| 轴数         | 3（head / left_arm / right_arm）                 | 5（neck_tilt / neck_lean / head_turn / tail_ud / tail_lr） | 3（head / tail_lr / tail_ud）                      |
| 表演剪辑     | 27                                               | 11                                                         | 28                                                 |
| 身体部位剪辑 | 6                                                | 7                                                          | 5                                                  |
| 待机素材     | 行为族`idle` 4 条                                | 6 个个性姿态 + 4 条脉冲                                    | 行为族`idle` 4 条                                  |
| 插值方式     | 逐帧 ease_motion 缓动（非阻塞状态机）            | 逐段 PlaySegment 阻塞插值 + 三档缓动                       | 连续目标流 + 指数追踪（最顺滑）                    |
| 特殊机制     | ShuffleBag 洗牌、`settle` 收势                   | `kKeep` 时序轴、情绪→动作映射                              | FlowTarget 前瞻滑帧、指数追踪                      |
| 声音联动     | 手臂交替扒动 + 呼吸起伏                          | 尾巴/头随响度正弦摆                                        | 尾巴/头微幅跟随                                    |

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
爬爬默认坐姿 90/90/90、限位 8~172；恐龙默认 neck_tilt=70、其余 90，tail_ud 限位 55~180；小熊猫 tail_ud 默认 150（尾巴上翘姿态）。

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

- **`kKeep` 是动物时序的核心**：某轴写 kKeep 表示"这拍不动"，编出"头先动（帧 1 只有 head_turn 非 kKeep）→ 脖子后随 → 尾巴最后响应"的接力感。爬爬/小熊猫 3 轴每帧全动，用不上。
- **每帧独立缓动** `EaseValue`：kSmooth 三次平滑 / kFastOut `1−(1−t)³`（快出慢停，适合突然探头/甩尾）/ kLinear（保持姿态）。
- `ApplyLiving` 每帧叠加 5 条频率互质的正弦（2.1/1.7/2.6/1.9/1.4 Hz），动作进行中也有呼吸感、避免五关节机械同步。

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

指数追踪天然起止速度为零、软着陆无硬停；也是迁移阶段 8 对齐参考工程 Catmull-Rom 的方向。

### 2.4 第四段：姿态合成与舵机输出

每拍把三层叠加，统一限幅，写 PWM：

```
插值层（剪辑关键帧插值结果 pose_）
  + 微动层（呼吸/凝视漂移/尾巴轻摆/肌肉微颤 —— 小熊猫 AddMicroMotion / 恐龙 ApplyLiving）
  + 音频包络层（ApplyAudioMotion —— §3）
  ↓
OutputPose / SetPose：clamp 到各轴机械限位（爬爬 8~172、恐龙 tail_ud 55~180 …）
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
| tempo 随机   | 0.88~1.14                                       | 0.90~1.15                    |
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

本地声音没有真实 PCM 包络（解码和 I2S 都在 XiaoZhi AudioService），所以用 **Opus 包字节数估算**：

```cpp
// 每 60ms 的 Opus 帧：包越大 ≈ 越响
kbps = packet_bytes × 8 × 1000 / 60;
raw  = clamp(kbps / kLoudnessRefBytes, 0, 1);
fast += (raw − fast) × (raw > fast ? 0.48 : 0.12);   // 快包络：快起慢落
slow += (raw − slow) × 0.035;                        // 慢包络：整体趋势
level  = slow;
attack = clamp((fast − slow × 1.08) × 3.2, 0, 1);    // onset 检测：快包络突起
```

在线 TTS 走真实响度：`AudioService.SetOutputAudioLevelListener` 回调 → `FeedTtsLevel`；speaking 状态时动作引擎读 `GetTtsMotionData()`，整体压低（×0.6/×0.45）—— TTS 每个音节都有 attack 尖峰，不压会抖。

### 3.2 各板怎么用包络

- **爬爬** `ApplyAudioMotion`（对齐旧工程 apply_audio_motion）：`attack>0.16` 且距上次 onset>320ms → 触发一次交替扒动（头 accent ±5~15°，onset 后 160~320ms 手臂交替下沉）；慢包络驱动呼吸起伏 `sin(phase)·min(5°, level·9)`，相位推进速度随 level 变化（响就喘得快）；无声时 accent 按 0.88~0.955 指数衰减归零。
- **恐龙** `SetPose`：tail_lr 加 `sin(phase)·min(14°, level·26)` 正弦摆（phase 每拍推进 0.30+0.45·level），tail_ud 小幅同频；attack>0.15 时头随机 ±4° 抖动。
- **小熊猫** `ApplyAudioMotion`：onset 驱动尾巴甩动 accent，慢包络驱动尾巴/头浮沉；微动层 life 系数在播声音时降到 0.55（跳舞时抑制呼吸微动，让舞动主导）。

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
队列空 & 离线 & 到点 → 加权表随机选动作（discover/affection/happy 权重×2），
                       每 5~13s 一次，配本地声音
否则 → 待机层（非阻塞）：
  - RunIdlePose：每 4~8s 换一个 kIdlePoses 个性姿态（平静/警觉/困倦/好奇/放松/轻快），
    姿态用 k=0.018 低通缓慢漂移过去，叠加多频呼吸正弦
  - 每 5~14s 随机 RunPulse 生命脉冲（glance/tail/perk/sniff，打破正弦可预测性）
```

待机层是非阻塞的，明确动作是阻塞的 —— 待机中被队列请求打断时，当前插值段在下一个 20ms 边界自然交接给新动作的 AttentionPose。

### 4.3 在线/离线行为总表

| | 在线（connecting/listening/speaking/audio_testing） | 离线（idle，含 Wi-Fi 已连但没对话） |
| --- | --- | --- |
| 情绪/MCP 动作 | 执行，不带本地声音 | 执行，带本地声音 |
| 自主表演 | 停止（恐龙只保留脉冲小表情） | 持续（声音驱动 + 定时器） |
| 本地声音 | 不播（进对话时 StopPlayback 清场） | 播放并驱动舞动 |
| 声音包络来源 | TTS 真实响度（压低） | Opus 包估算 |

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

## 7. 与参考工程的差距（阶段 7-8 待办）

当前恐龙库是简化版（11 剪辑）。参考工程 `BASE/dinosaur/main/auto_run.cc`（V3.3，只读参考，勿改）有：

- 29 个互动变体 + 5 个环境场景 + 4 个待机生命动作；
- Catmull-Rom 五轴插值（比恐龙现在的逐段 SmoothStep 更平滑）；
- 表演签名（近期表演去重）。

阶段 7 计划先做统一行为调度器（有界优先级队列：安全 > 手动 > 明确动作 > 情绪 > 对话 > 音频 > 自主 > 微动作，含平滑打断、精力状态、低电量降级），阶段 8 再逐产品把动作专项对齐参考工程。当前三产品各自的调度（恐龙的定时随机/爬爬小熊猫的计时器组）将来都收拢进统一调度器。
