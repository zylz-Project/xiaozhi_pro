#include "wzh_audio_player.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <esp_log.h>

#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "ogg_demuxer.h"

#define TAG "WzhAudioPlayer"

namespace wzh {
namespace {

constexpr size_t kReadChunk = 4096;
// 估算响度用的参考包大小：48k 单声道 60ms 的典型语音包约 150~250 字节。
constexpr float kLoudnessRefBytes = 24000.0f;

}  // namespace

bool AudioPlayer::Start(AudioStore* store)
{
    store_ = store;
    // SPI 读取和 OGG 解复用可能持续数百毫秒，必须放到独立任务；20 KiB
    // 栈主要供 OggDemuxer 的包缓存和 4 KiB Flash 读取缓冲区使用。
    play_queue_ = xQueueCreate(3, sizeof(int));
    if (play_queue_ == nullptr ||
        xTaskCreate(TaskEntry, "wzh_audio", 20480, this, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start offline audio task");
        return false;
    }
    return true;
}

void AudioPlayer::StopPlayback()
{
    playback_generation_.fetch_add(1);
    if (play_queue_ != nullptr) {
        int discarded = -1;
        while (xQueueReceive(play_queue_, &discarded, 0) == pdTRUE) {
        }
    }
    is_playing_.store(false);
    motion_level_.store(0.0f);
    motion_attack_.store(0.0f);
    // 本地声音的输出也会喂 tts_level_，进入对话时若不清零，speaking
    // 状态会带着上一段本地声音的包络立刻“舞动”。
    tts_level_.store(0.0f);
    tts_attack_.store(0.0f);
    Application::GetInstance().GetAudioService().ResetDecoder();
}

bool AudioPlayer::PlayIndex(int index)
{
    if (play_queue_ == nullptr || store_ == nullptr) {
        return false;
    }
    // Flash 正在写入（同步/删除）时不插入本地声音，避免读到刚被擦除的数据。
    if (store_->IsWriting()) {
        return false;
    }
    // XiaoZhi 的提示音或上一段声音尚未结束时拒绝，避免混音。
    if (!Application::GetInstance().GetAudioService().IsPlaybackIdle()) {
        return false;
    }
    // 队列已满就放弃本次声音，不允许调用者在这里等待。
    return xQueueSend(play_queue_, &index, 0) == pdTRUE;
}

MotionData AudioPlayer::GetMotionData() const
{
    MotionData data = {};
    data.playing = is_playing_.load();
    data.level = motion_level_.load();
    data.attack = motion_attack_.load();
    data.elapsed_ms = motion_elapsed_ms_.load();
    data.duration_ms = motion_duration_ms_.load();
    data.sound_index = motion_sound_index_.load();
    return data;
}

void AudioPlayer::FeedTtsLevel(float level, float attack)
{
    tts_level_.store(level);
    tts_attack_.store(attack);
}

MotionData AudioPlayer::GetTtsMotionData() const
{
    MotionData data = {};
    data.playing = tts_level_.load() > 0.01f;
    data.level = tts_level_.load();
    data.attack = tts_attack_.load();
    data.sound_index = -1;
    return data;
}

void AudioPlayer::UpdateEnvelope(size_t packet_bytes)
{
    const uint32_t frame_ms = 60;
    const float kbps =
        static_cast<float>(packet_bytes) * 8.0f * 1000.0f / static_cast<float>(frame_ms);
    float raw = kbps / kLoudnessRefBytes;
    raw = std::clamp(raw, 0.0f, 1.0f);
    fast_envelope_ += (raw - fast_envelope_) * (raw > fast_envelope_ ? 0.48f : 0.12f);
    slow_envelope_ += (raw - slow_envelope_) * 0.035f;
    float onset = (fast_envelope_ - slow_envelope_ * 1.08f) * 3.2f;
    onset = std::clamp(onset, 0.0f, 1.0f);
    motion_level_.store(slow_envelope_);
    motion_attack_.store(onset);
}

void AudioPlayer::TaskEntry(void* arg)
{
    static_cast<AudioPlayer*>(arg)->Task();
}

void AudioPlayer::Task()
{
    int index = -1;
    while (true) {
        if (xQueueReceive(play_queue_, &index, portMAX_DELAY) == pdTRUE) {
            PlayFile(index);
        }
    }
}

bool AudioPlayer::PlayFile(int index)
{
    AudioFileInfo file;
    if (store_ == nullptr || !store_->GetFile(index, &file)) {
        return false;
    }
    auto& app = Application::GetInstance();
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    const uint32_t generation = playback_generation_.load();
    motion_sound_index_.store(index);
    motion_elapsed_ms_.store(0);
    motion_duration_ms_.store(file.duration_ms);
    motion_level_.store(0.0f);
    motion_attack_.store(0.0f);
    fast_envelope_ = 0.0f;
    slow_envelope_ = 0.0f;
    is_playing_.store(true);

    // OggDemuxer 只拆出 Opus 包；真正的解码和 I2S 输出仍由 AudioService 完成。
    bool ok = true;
    OggDemuxer demuxer;
    demuxer.OnDemuxerFinished(
        [this, &app, &ok, generation](const uint8_t* data, int sample_rate, size_t size) {
            if (playback_generation_.load() != generation) {
                ok = false;
                return;
            }
            UpdateEnvelope(size);
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = 60;
            packet->payload.assign(data, data + size);
            if (!app.GetAudioService().PushPacketToDecodeQueue(std::move(packet), true)) {
                ok = false;
            }
        });

    // 分块顺序读取，避免为完整音频文件申请大块 RAM。
    uint8_t buffer[kReadChunk];
    ESP_LOGI(TAG, "Playing offline sound: %s", file.name);
    const uint32_t base = store_->data_start() + file.offset;
    for (uint32_t offset = 0;
         ok && playback_generation_.load() == generation && offset < file.size;) {
        const size_t chunk = std::min<size_t>(sizeof(buffer), file.size - offset);
        if (wzh_flash_read(base + offset, buffer, chunk) != ESP_OK) {
            ok = false;
            break;
        }
        demuxer.Process(buffer, chunk);
        offset += chunk;
        motion_elapsed_ms_.store(
            static_cast<uint32_t>(offset * file.duration_ms / std::max<uint32_t>(1, file.size)));
    }
    is_playing_.store(false);
    motion_level_.store(0.0f);
    motion_attack_.store(0.0f);
    return ok;
}

}  // namespace wzh
