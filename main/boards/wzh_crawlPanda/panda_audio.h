#ifndef WZH_CRAWLPANDA_AUDIO_H_
#define WZH_CRAWLPANDA_AUDIO_H_

#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum class PandaAction : uint8_t;

// 供动作引擎读取的“声音活跃度”。本地声音没有真正的 PCM 包络（解码和
// I2S 输出都由 XiaoZhi AudioService 完成），这里用 Opus 包字节数估算
// 响度，足以驱动头部/前肢的跟随舞动。
struct PandaMotionData {
    bool playing;
    float level;   // 0..1 平滑响度
    float attack;  // 0..1 起始能量
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    int sound_index;
};

// 管理 W25Q256（SPI NOR）中预置的离线声音。
//
// 本模块只读外部 Flash，不负责联网同步、下载或改写音频。读取由板级
// w25n01gv 驱动完成，读取到的 OGG/Opus 数据最终仍交给 XiaoZhi 的
// AudioService 解码和播放，避免额外创建一套 I2S 驱动与在线 TTS 抢占 ES8311。
class PandaAudio {
public:
    bool Initialize();
    bool PlayForAction(PandaAction action);
    bool PlayRandomSound(const char* category);
    void StopPlayback();
    PandaMotionData GetMotionData() const;

    // 由 XiaoZhi AudioService 的输出包络回调驱动（TTS/在线回复的真实响度）。
    void FeedTtsLevel(float level, float attack);
    // 在线 TTS 播放期间的响度数据（playing 仅在确有输出时置真）。
    PandaMotionData GetTtsMotionData() const;

    int file_count() const { return file_count_; }
    int SoundCountByCategory(const char* category) const;
    const char* SoundName(int index) const;
    int SoundDurationMs(int index) const;
    const char* SoundCategory(int index) const;

private:
    // PNDA TOC 的板端表示。offset 相对于 PANDA_FLASH_DATA_START，
    // 而不是相对于整颗 Flash 的起始地址。
    struct FileInfo {
        char name[64];
        uint32_t offset;
        uint32_t size;
        uint32_t sample_rate;
        uint32_t duration_ms;
        char category[16];
    };

    static constexpr int kMaxFiles = 32;
    static constexpr size_t kEntrySize = 96;

    QueueHandle_t play_queue_ = nullptr;
    std::atomic<uint32_t> playback_generation_{0};
    FileInfo files_[kMaxFiles] = {};
    int file_count_ = 0;

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

    static void AudioTaskEntry(void* arg);
    void AudioTask();
    bool Read(uint32_t address, void* data, size_t size);
    bool ReadToc();
    bool PlayFile(int index);
    int SelectFile(PandaAction action) const;
    int RandomIndexInCategory(const char* category) const;
};

#endif  // WZH_CRAWLPANDA_AUDIO_H_