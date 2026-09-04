#ifndef WZH_AUDIO_PLAYER_H_
#define WZH_AUDIO_PLAYER_H_

#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "wzh_audio_store.h"

namespace wzh {

// 供动作引擎读取的“声音活跃度”。本地声音没有真正的 PCM 包络（解码和
// I2S 输出都由 XiaoZhi AudioService 完成），这里用 Opus 包字节数估算
// 响度，足以驱动头/四肢/尾巴的跟随舞动。
struct MotionData {
    bool playing;
    float level;   // 0..1 平滑响度
    float attack;  // 0..1 起始能量
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    int sound_index;
};

// 三个产品共用的离线声音播放引擎。
//
// 从 AudioStore 分块读取 OGG/Opus → OggDemuxer 拆包 → XiaoZhi AudioService
// 解码播放。本类不触碰 I2S，也不创建第二套解码器，与在线 TTS 共用
// ES8311 的唯一输出通路；开始新声音前用 IsPlaybackIdle 门控避免混音。
class AudioPlayer {
public:
    // 创建播放队列并启动播放任务。store 必须已经 Init 成功。
    bool Start(AudioStore* store);

    // 停止本地声音：递增代次、清空队列、复位解码器，防止残留声音
    // 干扰收音或 TTS。也清零 TTS 包络残留，避免 speaking 状态带着
    // 上一段声音的包络立即“舞动”。
    void StopPlayback();

    // 播放指定条目。AudioService 未空闲或正在写入 Flash 时拒绝。
    bool PlayIndex(int index);
    bool Playing() const { return is_playing_.load(); }

    MotionData GetMotionData() const;

    // 由 XiaoZhi AudioService 的输出包络回调驱动（TTS/在线回复的真实响度）。
    void FeedTtsLevel(float level, float attack);
    // 在线 TTS 播放期间的响度数据（playing 仅在确有输出时置真）。
    MotionData GetTtsMotionData() const;
    float tts_level() const { return tts_level_.load(); }
    float tts_attack() const { return tts_attack_.load(); }

protected:
    bool PlayFile(int index);
    // 从 Opus 包大小估算响度：包越大越“响”，与真实 PCM 包络趋势一致。
    void UpdateEnvelope(size_t packet_bytes);

    AudioStore* store_ = nullptr;
    QueueHandle_t play_queue_ = nullptr;
    // 每次停止播放都会增加代次；旧播放看到代次变化后退出。
    std::atomic<uint32_t> playback_generation_{0};

    // 伪包络状态（从 Opus 包大小估算，非真实 PCM 电平）。
    std::atomic<bool> is_playing_{false};
    std::atomic<float> motion_level_{0.0f};
    std::atomic<float> motion_attack_{0.0f};
    std::atomic<uint32_t> motion_elapsed_ms_{0};
    std::atomic<uint32_t> motion_duration_ms_{0};
    std::atomic<int> motion_sound_index_{-1};
    // 在线 TTS 包络（来自 AudioService 输出回调）。
    std::atomic<float> tts_level_{0.0f};
    std::atomic<float> tts_attack_{0.0f};
    float fast_envelope_ = 0.0f;
    float slow_envelope_ = 0.0f;

    static void TaskEntry(void* arg);
    void Task();
};

}  // namespace wzh

#endif  // WZH_AUDIO_PLAYER_H_
