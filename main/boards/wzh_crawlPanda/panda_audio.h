#ifndef WZH_CRAWLPANDA_AUDIO_H_
#define WZH_CRAWLPANDA_AUDIO_H_

#include "wzh_audio_player.h"

enum class PandaAction : uint8_t;

using PandaMotionData = wzh::MotionData;

// 爬爬熊猫离线声音：共享播放引擎 + 本产品的动作→声音选择策略。
// OGG 解复用、AudioService 投递和外部 Flash 访问都在 wzh::AudioPlayer /
// wzh::AudioStore 中完成，本类只负责按动作挑选合适的条目。
class PandaAudio : public wzh::AudioPlayer {
public:
    bool Initialize();
    bool PlayForAction(PandaAction action);
    bool PlayRandomSound(const char* category);

    int file_count() const { return store_.file_count(); }
    // MCP 管理工具需要直接访问仓库（删除/擦除/状态）。
    wzh::AudioStore& audio_store() { return store_; }
    int SoundCountByCategory(const char* category) const {
        return store_.CountInCategory(category);
    }
    const char* SoundName(int index) const;
    int SoundDurationMs(int index) const;
    const char* SoundCategory(int index) const;

private:
    // 按动作从目录快照中挑选条目（蓄水池采样），返回目录下标。
    int SelectFile(PandaAction action) const;

    wzh::AudioStore store_;
};

#endif  // WZH_CRAWLPANDA_AUDIO_H_
