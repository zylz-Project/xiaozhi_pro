#ifndef WZH_DINOSAUR_AUDIO_H_
#define WZH_DINOSAUR_AUDIO_H_

#include "wzh_audio_player.h"

enum class DinoAction : uint8_t;

using DinoMotionData = wzh::MotionData;

// 恐龙离线声音：共享播放引擎 + 本产品的动作→声音选择策略。
// 外部 Flash 访问（JEDEC 自动探测 NOR/NAND）、OGG 解复用和 AudioService
// 投递都在 wzh::AudioPlayer / wzh::AudioStore 中完成。
class DinosaurAudio : public wzh::AudioPlayer {
public:
    // 初始化 Flash、读取 PNDA 目录并启动播放任务。没有有效目录时返回
    // false，但不影响恐龙继续执行无声动作。
    bool Initialize();

    // 按动作类型挑选一个合适的本地声音并放入播放队列；不会阻塞调用者。
    bool PlayForAction(DinoAction action);
    // 按分类随机播放（ambient 场景用）。
    bool PlayRandomSound(const char* category);
    // 最近一次 TTS 输出响度（控制器动作层使用，保留旧接口名）。
    float GetTtsLevel() const { return tts_level(); }
    float GetTtsAttack() const { return tts_attack(); }

    int file_count() const { return store_.file_count(); }
    // MCP 管理工具需要直接访问仓库（删除/擦除/状态）。
    wzh::AudioStore& audio_store() { return store_; }
    const char* SoundName(int index) const;
    int SoundDurationMs(int index) const;

private:
    // 按动作从目录快照中挑选条目（蓄水池采样），返回目录下标。
    int SelectFile(DinoAction action) const;

    wzh::AudioStore store_;
};

#endif  // WZH_DINOSAUR_AUDIO_H_
