#ifndef WZH_AUDIO_PLAYER_H_
#define WZH_AUDIO_PLAYER_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "wzh_audio_store.h"
#include "wzh_interaction.h"
#include "wzh_interaction_sound.h"
#include "wzh_sound_coverage.h"
#include "wzh_sound_score.h"
#include "wzh_user_pairs.h"

namespace wzh {

using MotionData = SoundPlayback;

// 三个产品共用的离线声音播放引擎。
//
// 从 AudioStore 分块读取 OGG/Opus → OggDemuxer 拆包 → XiaoZhi AudioService
// 解码播放。本类不触碰 I2S，也不创建第二套解码器，与在线 TTS 共用
// ES8311 的唯一输出通路；开始新声音前用 IsPlaybackIdle 门控避免混音。
class AudioPlayer {
public:
    // 创建播放队列并启动播放任务。空目录可随后由 Audio Hub 填充。
    bool Start(AudioStore* store);

    // 停止本地声音：递增代次、清空队列、复位解码器，防止残留声音
    // 干扰收音或 TTS。也清零 TTS 包络残留，避免 speaking 状态带着
    // 上一段声音的包络立即“舞动”。
    void StopPlayback();
    void SetMaintenance(bool enabled);
    bool WorkerIdle() const { return !worker_active_.load(); }
    std::string CurrentName() const;

    // 播放指定条目。AudioService 未空闲或正在写入 Flash 时拒绝。
    bool PlayIndex(int index);
    int FindSoundIndex(const char* name) const;
    std::vector<AudioFileInfo> PairedFiles() const;
    // 从外部 Flash 的现有动物声音中按互动选声，有其他条目时避免连续重复。
    bool PlayInteractionSound(PetInteraction interaction);
    // Fair coverage of every valid Flash entry, including ambient; no replacement
    // within a round, and a failed/busy enqueue does not consume a turn.
    // ambient_percent biases the mix: 0 = never ambient, 100 = original pool.
    bool PlayAutonomousSound(bool include_ambient = true, uint32_t ambient_percent = 100);
    bool Playing() const { return is_playing_.load() || playback_pending_.load(); }
    // MultiNet commands can echo our Flash calls/TTS. Keep ordinary WakeNet
    // interruption available; board routing only gates local action commands.
    bool LocalCommandEchoRisk();

    MotionData GetMotionData() const;

    // 由 XiaoZhi AudioService 的输出包络回调驱动（TTS/在线回复的真实响度）。
    void FeedTtsLevel(float level, float attack);
    // 在线 TTS 播放期间的响度数据（playing 仅在确有输出时置真）。
    MotionData GetTtsMotionData() const;
    float tts_level() const { return GetTtsMotionData().level; }
    float tts_attack() const { return GetTtsMotionData().attack; }

protected:
    struct PlayRequest {
        int index;
        uint32_t generation;
        AudioFileInfo file;
    };
    bool QueueFile(int index, const AudioFileInfo& file);
    bool PlayFile(const PlayRequest& request);

    AudioStore* store_ = nullptr;
    QueueHandle_t play_queue_ = nullptr;
    // 每次停止播放都会增加代次；旧播放看到代次变化后退出。
    std::atomic<uint32_t> playback_generation_{0};
    // Reserve from enqueue until Flash reading finishes. Decoder idle alone does
    // not cover the interval before the worker submits its first Opus packet.
    mutable std::mutex playback_mutex_;
    std::atomic<bool> playback_pending_{false};
    std::atomic<bool> maintenance_{false};
    std::atomic<bool> worker_active_{false};
    std::array<char, 64> current_name_{};

    // Local playback state; envelope and start clock come from speaker PCM.
    std::atomic<bool> is_playing_{false};
    std::atomic<float> motion_level_{0.0f};
    std::atomic<float> motion_attack_{0.0f};
    std::atomic<uint32_t> motion_elapsed_ms_{0};
    std::atomic<uint32_t> motion_duration_ms_{0};
    std::atomic<int> motion_sound_index_{-1};
    std::atomic<const SoundScore*> motion_score_{nullptr};
    std::atomic<SoundKind> motion_kind_{SoundKind::kCall};
    std::atomic<uint32_t> motion_sequence_{0};
    std::atomic<int64_t> local_pcm_start_us_{0};
    std::atomic<int> local_packet_ms_{60};
    // 在线 TTS 包络（来自 AudioService 输出回调）。
    std::atomic<int64_t> tts_updated_us_{0};
    std::atomic<float> tts_level_{0.0f};
    std::atomic<float> tts_attack_{0.0f};
    InteractionSoundSelector interaction_sounds_;
    SoundCoverage sound_coverage_;
    SoundCoverage paired_coverage_;
    uint32_t autonomous_turn_ = 0;
    std::mutex reaction_mutex_;

    static void TaskEntry(void* arg);
    void Task();
};

}  // namespace wzh

#endif  // WZH_AUDIO_PLAYER_H_
