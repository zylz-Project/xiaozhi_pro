# WZH 动作库与执行机制

三款产品使用板级动作库和共享舵机输出。动作输入经板级控制器投递给独立动作任务。任务根据当前姿态、音频和保护状态更新目标。所有目标经过板级限位与共享输出，舵机使用 50 Hz、14 位 LEDC。 [共享输出](../../../main/boards/common/wzh/wzh_servo_output.cc) · [接线和装配](../hardware/hardware_review.md)

| 产品 | 控制器 | 动作轴与默认位置 |
| --- | --- | --- |
| 大熊猫小安 | [panda_controller.cc](../../../main/boards/wzh_crawlPanda/panda_controller.cc) | 头部左右、左前爪、右前爪默认 90°。头 30°～150°，前爪各 8°～172°。 |
| 小熊猫星宝 | [panda_controller.cc](../../../main/boards/wzh_tailRedPanda/panda_controller.cc) | 头部左右与尾巴左右默认 90°。头 30°～150°，尾巴左右 6°～174°。尾巴上下使用动作坐标 0°～180°，当前默认 150°，使用直接输出。 |
| 恐龙小龙 | [dinosaur_controller.cc](../../../main/boards/wzh_dinosaur/dinosaur_controller.cc) | 颈上下默认 70°，颈左右、头左右、尾上下、尾左右默认 90°。尾上下动作坐标 55°～180°。颈部和尾部共四个轴使用 270° 舵机，控制器以 1500 µs 中位为基准换算这四轴的 PWM。 |

逻辑角度、舵盘转角和机身实际运动角度需要分别校对。使用板级 `config.h` 和控制器的最终限位，按实装分别验证中位、方向、组合同动和回中间隙。 [大熊猫配置](../../../main/boards/wzh_crawlPanda/config.h) · [小熊猫配置](../../../main/boards/wzh_tailRedPanda/config.h) · [恐龙配置](../../../main/boards/wzh_dinosaur/config.h)

## 动作从哪里来

- **身体口令。** 离线 MultiNet 和小智 MCP 通过板级能力词表查找可用的身体动作，进入板级队列。触摸、人体和姿态由各入口更新输入状态，再由动作任务处理。 [语音口令](../audio/voice_commands.md) · [感应与保护](../hardware/sensing_and_protection.md)
- **内置表演。** 板级行为族和动作剪辑驱动休闲、响应和专属表演。普通剪辑由姿态关键帧、时长、缓动等组成。关节轨迹从当前输出姿态接续，支持在指定动作中换向和洗牌选择。
- **声音配套。** 手调配套以嵌入的录音和动作关键帧为输入。普通 Flash 声音先按文件类别、时长和包络选择相应编舞。恐龙有专用吼叫轨迹，大熊猫可对脚步声选爬行步态，小熊猫使用头尾弧线。 [声音编舞](../../../main/boards/common/wzh/wzh_sound_choreography.h) · [手调配套](../../../main/boards/common/wzh/wzh_user_pair_motion.h) · [音画清单](../data/user_pairs.json)
- **对话随动。** 使用扬声器的实际 PCM 包络、重音和停顿驱动产品特有动作。姿态随语音变化，从当前输出接续，等待和退出时回中。 [对话随动](../../../main/boards/common/wzh/wzh_voice_motion.h) · [音频架构](../../../main/audio/README.md)
- **恐龙实时随动。** 回复句子的语气决定姿态，服务端情绪和用户转写提供兜底。问句抬头侧看，安慰轻低头，开心时颈尾幅度更大。重音经过限速和阻尼，短停顿逐渐卸力，长停顿从当前姿态回中。 [恐龙实时随动](../../../main/boards/wzh_dinosaur/dinosaur_speech_motion.h)

## 展示与释放

正常本地待机时，互动开启一轮展示。前 2 分钟每段声音和动作收尾后随机等待 2～3 秒。第 2 到第 5 分钟间隔 30 秒，随后切换为 2～3 分钟的稀疏间隔。恐龙从互动触发起算，两款熊猫从互动的声音和动作结束起算。新互动重新计时。对话、维护和保护暂停自动展示。

静置等待时从最后输出姿态平滑归中并释放。显式互动可抢占自主表演。低电、过热、落体与更新超时通过共享输出禁动。正常关机有限归中，临界欠压立即切断。 [传感与保护阈值](../hardware/sensing_and_protection.md)

## 修改与验证

1. 在对应控制器定位行为族、动作、声音入口或身体命令。新素材复用当前产品的队列、取消、限位与归中路径。
2. 用 `scripts/run_host_tests.py` 验证相关关键帧、输出边界、保护和声音配套。恐龙 270° 舵机的逻辑角度和实体行程均需核对。三款分别验证通电前起始姿态、转向、电机异响、连杆或外壳干涉、连续动作供电和保护抢占。
3. 浏览器预览用来检查记录的目标轨迹和音画配对，最终 PWM、电源和机械表现以上机为准。 [音画预览](audio_motion_preview.html) · [整机验收](../product/product_acceptance.md)

旧版动作数量、插值对照和阶段性实测记录见相应历史文档。 [2026-11-05 轨迹调整](wzh_fluid_motion_2026-11-05.md) · [2026-11-06 动作变化](wzh_motion_variety_shutdown_2026-11-06.md) · [2026-10 动作资料](../archive/README.md)
