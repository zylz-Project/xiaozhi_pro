#ifndef WZH_DINOSAUR_AUDIO_H_
#define WZH_DINOSAUR_AUDIO_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum class DinoAction : uint8_t;

// 管理 W25Q256 中预置的离线声音。
//
// 本模块只读外部 Flash，不负责联网同步、下载或改写音频。读取到的
// OGG/Opus 数据最终仍交给 XiaoZhi 的 AudioService 解码和播放，避免
// 额外创建一套 I2S 驱动与在线 TTS 抢占 ES8311。
class DinosaurAudio {
public:
    // 初始化 SPI、读取 PNDA 文件表并启动播放任务。Flash 中没有有效
    // 文件表时返回 false，但不影响恐龙继续执行无声动作。
    bool Initialize();

    // 按动作类型挑选一个合适的本地声音并放入播放队列；不会阻塞调用者。
    bool PlayForAction(DinoAction action);

    // 切入在线对话前停止本地声音并清理解码队列，防止干扰收音或 TTS。
    void StopPlayback();
    int file_count() const { return file_count_; }

private:
    // PNDA TOC 的板端表示。offset 相对于 DINO_FLASH_DATA_START，
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

    spi_device_handle_t flash_ = nullptr;
    // 队列中存放 files_ 的下标，实际 SPI 读取在独立任务中完成。
    QueueHandle_t play_queue_ = nullptr;
    // 每次停止播放都会增加代次；正在读 Flash 的旧任务看到代次变化后退出。
    std::atomic<uint32_t> playback_generation_{0};
    FileInfo files_[kMaxFiles] = {};
    int file_count_ = 0;

    static void AudioTaskEntry(void* arg);
    void AudioTask();
    bool Read(uint32_t address, void* data, size_t size);
    bool ReadToc();
    bool PlayFile(int index);
    int SelectFile(DinoAction action) const;
};

#endif  // WZH_DINOSAUR_AUDIO_H_
