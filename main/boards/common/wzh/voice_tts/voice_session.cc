#include "voice_session.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <algorithm>
#include <chrono>
#include <new>

#include "ali_provider.h"
#if CONFIG_WZH_ALI_STREAM_EXPERIMENT
#include "ali_stream_client.h"
#endif
#include "application.h"
#include "board.h"
#include "ogg_demuxer.h"
#include "voice_config.h"
#include "voice_identity.h"
#include "voice_flash_dispatcher.h"
#include "voice_text_batch.h"
#include "voice_text_filter.h"

namespace voice_tts {
namespace {
[[maybe_unused]] constexpr char kTag[] = "VoiceTts";
constexpr size_t kMaxPendingSentences = 64;
constexpr size_t kMaxPendingBytes = 16 * 1024;

// AudioService owns the bounded decode and playout queues. Preserve the
// 48 kHz Ogg timeline, packet durations, and per-stream head/tail trimming.
class AliOpusPlayback {
public:
    AliOpusPlayback(AudioService& audio, uint32_t generation, int64_t turn_started)
        : audio_(audio), generation_(generation), turn_started_(turn_started) {
        demuxer_.OnOpusPacket([this](const uint8_t* bytes, size_t size,
                                     const OpusStreamMetadata& info) {
            if (!accepted_)
                return;
            const auto samples = OpusPacketSamples48k(bytes, size);
            if (!samples) {
                accepted_ = false;
                return;
            }
            auto packet = std::unique_ptr<AudioStreamPacket>(new (std::nothrow) AudioStreamPacket);
            if (!packet) {
                accepted_ = false;
                return;
            }
            packet->sample_rate = 48000;
            packet->frame_duration = std::max<uint32_t>(5, (samples + 47) / 48);
            packet->stream = info;
            // Each HTTPS sentence has its own decoder state and trim metadata.
            // Keep speaker DSP continuous until the whole response drains.
            packet->stream.continuous_output = true;
            packet->stream.end = false;
#if CONFIG_BOARD_TYPE_WZH_TAILREDPANDA || CONFIG_WZH_DIALOGUE_PARITY
            packet->external_opus.reset(static_cast<uint8_t*>(
                heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
            if (!packet->external_opus) {
                accepted_ = false;
                return;
            }
            std::copy_n(bytes, size, packet->external_opus.get());
            packet->external_opus_size = size;
#else
            packet->payload.assign(bytes, bytes + size);
#endif
            accepted_ = audio_.PushPacketToDecodeQueue(std::move(packet), true, generation_);
            if (!got_packet_ && accepted_) {
                ESP_LOGI(kTag, "Ali first Opus packet queued: request_ms=%lld turn_ms=%lld",
                         static_cast<long long>((esp_timer_get_time() - request_started_) / 1000),
                         static_cast<long long>((esp_timer_get_time() - turn_started_) / 1000));
            }
            got_packet_ = true;
        });
    }

    void BeginRequest(int64_t started) {
        demuxer_.Reset();
        request_started_ = started;
        accepted_ = true;
        got_packet_ = false;
    }

    bool Write(const uint8_t* bytes, size_t size) {
        return !size || (accepted_ && demuxer_.Process(bytes, size) == size && accepted_);
    }

    bool Complete() const {
        // Only call after the provider has confirmed a complete HTTP/SSE response.
        // A whole Ogg stream may end at a verified page boundary, including an
        // empty EOS page, even when no emitted packet has the EOS flag.
        return accepted_ && got_packet_ && demuxer_.AtVerifiedPageBoundary();
    }

    bool HasEndOfStream() const { return demuxer_.saw_end_of_stream(); }

private:
    AudioService& audio_;
    const uint32_t generation_;
    const int64_t turn_started_;
    int64_t request_started_ = 0;
    bool accepted_ = false, got_packet_ = false;
    OggDemuxer demuxer_{true};
};
}  // namespace

bool VoiceSession::Ready() {
    if (bypass_next_.exchange(false)) {
        ESP_LOGW(kTag, "External speech unavailable: one-turn fallback after earlier failure");
        return false;
    }
    return ReadyForVoice("");
}

bool VoiceSession::ReadyForVoice(const std::string& voice) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_ && (!stopping_ || pending_)) {
        ESP_LOGW(kTag, "External speech unavailable: previous task is still stopping");
        return false;
    }
    const auto selected = voice.empty() ? LoadAliVoice() : voice;
    if (selected.empty() || selected == kNativeVoiceId) {
        ESP_LOGD(kTag, "Using server voice: no Ali voice selected");
        return false;
    }
    if (LoadAliKey().empty()) {
        ESP_LOGW(kTag, "External speech unavailable: Ali key missing for selected voice");
        return false;
    }
    // TLS/AES and the BLE controller need internal DMA memory even when TLS
    // buffers and the PCM player live in PSRAM. Avoid starting a second TLS
    // session when it could exhaust memory used by the existing connection.
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if CONFIG_WZH_VOICE_PSRAM_WORKER_EXPERIMENT
#if CONFIG_WZH_VOICE_FLASH_DISPATCH_EXPERIMENT
    const size_t kRequiredLargest = FlashDispatcherReady() ? 6 * 1024 : 8 * 1024;
#else
    constexpr size_t kRequiredLargest = 6 * 1024;
#endif
#else
    constexpr size_t kRequiredLargest = 8 * 1024;
#endif
    if (internal < 24 * 1024 || internal_largest < kRequiredLargest) {
        ESP_LOGW(kTag, "External speech deferred: internal=%u largest=%u",
                 static_cast<unsigned>(internal), static_cast<unsigned>(internal_largest));
        return false;
    }
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (free_bytes < 3 * 1024 * 1024 || largest < 1024 * 1024) {
        ESP_LOGW(kTag, "Insufficient PSRAM for voice cloning: free=%u largest=%u",
                 static_cast<unsigned>(free_bytes), static_cast<unsigned>(largest));
        return false;
    }
    return true;
}

bool VoiceSession::Begin(uint32_t playback_generation, std::function<void(bool)> completion) {
    return BeginWithVoice(playback_generation, "", std::move(completion));
}

bool VoiceSession::BeginWithVoice(uint32_t playback_generation, const std::string& voice_override,
                                  std::function<void(bool)> completion) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string provider = "ali";
    const auto key = LoadAliKey();
    const auto voice = voice_override.empty() ? LoadAliVoice() : voice_override;
    if ((task_ && (!stopping_ || pending_)) || !completion || key.empty() || voice.empty() ||
        voice == kNativeVoiceId)
        return false;
    if (task_) {
        // The cancelled worker may still be unwinding an HTTP read. Keep a
        // single bounded next turn without blocking the application task.
        auto pending = std::unique_ptr<PendingTurn>(new (std::nothrow) PendingTurn);
        if (!pending)
            return false;
        pending->provider = provider;
        pending->api_key = key;
        pending->voice_id = voice;
        pending->completion = std::move(completion);
        pending->playback_generation = playback_generation;
        pending->generation = generation_.fetch_add(1) + 1;
        queue_.clear();
        queued_bytes_ = 0;
        finished_ = failed_ = had_text_ = overflow_ = false;
        pending_ = std::move(pending);
        ESP_LOGI(kTag, "External speech queued while previous request stops");
        xTaskNotifyGive(task_);
        return true;
    }
    provider_ = provider;
    api_key_ = key;
    voice_id_ = voice;
    queue_.clear();
    queued_bytes_ = 0;
    finished_ = failed_ = had_text_ = overflow_ = false;
    completion_ = std::move(completion);
    playback_generation_ = playback_generation;
    active_generation_.store(generation_.fetch_add(1) + 1);
    stopping_ = false;
    ESP_LOGI(kTag, "Starting voice cloning provider=%s", provider_.c_str());
#if CONFIG_WZH_VOICE_PSRAM_WORKER_EXPERIMENT
    bool created = worker_task_ != nullptr;
    if (!created) {
#if CONFIG_WZH_VOICE_FLASH_DISPATCH_EXPERIMENT
        if (!FlashDispatcherReady())
            created = xTaskCreate(TaskEntry, "voice_tts", 8192, this, 3, &worker_task_) == pdPASS;
        else
#endif
            created = xTaskCreateWithCaps(TaskEntry, "voice_tts", 8192, this, 3, &worker_task_,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }
#else
    const bool created = xTaskCreate(TaskEntry, "voice_tts", 8192, this, 3, &task_) == pdPASS;
#endif
    if (!created) {
        task_ = nullptr;
        completion_ = {};
        api_key_.clear();
        return false;
    }
#if CONFIG_WZH_VOICE_PSRAM_WORKER_EXPERIMENT
    task_ = worker_task_;
    xTaskNotifyGive(worker_task_);
#endif
    return true;
}

bool VoiceSession::ShouldUseServerAudio(const std::string& text) const {
    return IsMusicStart(text);
}

bool VoiceSession::ShouldResumeExternalAudio(const std::string& text) const {
    return IsPostMusicAnnouncement(text);
}

bool VoiceSession::Enqueue(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!task_ || finished_ || failed_ || overflow_ || text.empty()) {
        return false;
    }
    if (IsToolStatus(text)) {
        had_text_ = true;  // A status-only turn completes without triggering fallback.
        ESP_LOGI(kTag, "Skipping tool status");
        return true;
    }
    if (text.size() > 512) {
        overflow_ = true;
        ESP_LOGW(kTag, "Speech sentence exceeds the 512-byte limit; retaining queued sentences");
        return false;
    }
    if (queue_.size() >= kMaxPendingSentences || text.size() > kMaxPendingBytes - queued_bytes_) {
        overflow_ = true;
        ESP_LOGW(kTag, "Speech text queue full; retaining %u sentences (%u bytes)",
                 static_cast<unsigned>(queue_.size()), static_cast<unsigned>(queued_bytes_));
        return false;
    }
    queue_.push_back(text);
    queued_bytes_ += text.size();
    had_text_ = true;
    xTaskNotifyGive(task_);
    return true;
}

void VoiceSession::Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
    if (task_)
        xTaskNotifyGive(task_);
}

void VoiceSession::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_.fetch_add(1);
    pending_.reset();
    stopping_ = task_ != nullptr;
    queue_.clear();
    queued_bytes_ = 0;
    completion_ = {};
    finished_ = true;
    if (task_)
        xTaskNotifyGive(task_);
}

void VoiceSession::TaskEntry(void* context) {
    auto* self = static_cast<VoiceSession*>(context);
#if CONFIG_WZH_VOICE_PSRAM_WORKER_EXPERIMENT
    // This worker never writes flash and is kept alive, avoiding the SDK's
    // self-delete cleanup task, which itself needs scarce internal memory.
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            uint32_t generation;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (!self->task_)
                    break;
                generation = self->active_generation_.load();
            }
            self->Run(generation);
        }
    }
#else
    self->Run(self->active_generation_.load());
    vTaskDelete(nullptr);
#endif
}

void VoiceSession::Run(uint32_t generation) {
    auto cancelled = [this, generation]() { return generation != generation_.load(); };
    auto& audio = Application::GetInstance().GetAudioService();
    const int64_t turn_started = esp_timer_get_time();
    const auto turn_output_clock_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto playback = std::unique_ptr<AliOpusPlayback>(
        new (std::nothrow) AliOpusPlayback(audio, playback_generation_, turn_started));
    if (!playback) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            failed_ = true;
        }
        CompleteWorker(generation);
        return;
    }
    const auto initial = audio.GetPlaybackBufferStatus();
    audio.ConfigureExternalSpeechPlayout(playback_generation_);
    AliProvider provider;
    int64_t last_activity = turn_started;
    int64_t batch_started = 0;
    bool sent_http_request = false;
    uint32_t last_header_ms = 0;
#if CONFIG_WZH_ALI_STREAM_EXPERIMENT
    bool use_http = true;
    if (!cancelled()) {
        StreamRequest stream;
        stream.api_key = api_key_;
        stream.voice_id = voice_id_;
        stream.cancelled = cancelled;
        stream.next_text = [this, &cancelled](std::string& sentence, bool& done) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled() || failed_) {
                done = true;
            } else if (!queue_.empty()) {
                sentence = PopTextBatch(queue_, queued_bytes_, true);
            } else {
                done = finished_;
            }
        };
        playback->BeginRequest(esp_timer_get_time());
        stream.opus_sink = [&playback](const uint8_t* bytes, size_t size) {
            return playback->Write(bytes, size);
        };
        StreamResponse response;
        const Result result = SynthesizeStream(stream, response);
        if (result == Result::kOk && playback->Complete()) {
            use_http = false;
        } else if (response.text_committed || result == Result::kOk) {
            use_http = false;
            if (!cancelled()) {
                ESP_LOGW(kTag, "Duplex synthesis failed after text submission: result=%d",
                         static_cast<int>(result));
                std::lock_guard<std::mutex> lock(mutex_);
                failed_ = true;
                queue_.clear();
                queued_bytes_ = 0;
            }
        } else if (!cancelled()) {
            ESP_LOGW(kTag, "Duplex synthesis unavailable before text; retrying with HTTP");
        }
    }
    while (use_http && !cancelled()) {
#else
    while (!cancelled()) {
#endif
        std::string sentence;
        bool done = false;
        int64_t batching_delay_us = 0;
        const auto buffered_ms = sent_http_request ? audio.GetPlaybackBufferStatus().buffered_ms : 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled())
                break;
            if (failed_) {
                queue_.clear();
                queued_bytes_ = 0;
                done = finished_;
            } else if (!queue_.empty()) {
                const int64_t now = esp_timer_get_time();
                // Give the next HTTPS request a head start when the current
                // audio is nearly depleted. Merge briefly only with enough cover.
                if (buffered_ms >= std::max<uint32_t>(500, last_header_ms) + 120 &&
                    !finished_ && queue_.size() == 1 &&
                    queue_.front().size() < 96) {
                    if (!batch_started)
                        batch_started = now;
                    batching_delay_us = 120000 - (now - batch_started);
                }
                if (batching_delay_us <= 0) {
                    sentence = PopTextBatch(queue_, queued_bytes_, true);
                    batch_started = 0;
                }
            } else {
                done = finished_;
            }
        }
        if (done)
            break;
        if (batching_delay_us > 0) {
            ulTaskNotifyTake(pdTRUE, std::max<TickType_t>(1, pdMS_TO_TICKS(batching_delay_us / 1000)));
            continue;
        }
        if (sentence.empty()) {
            if (esp_timer_get_time() - last_activity > 60000000) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!cancelled())
                    failed_ = true;
                break;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        last_activity = esp_timer_get_time();
        const int64_t request_started = esp_timer_get_time();
        Request request;
        request.api_key = api_key_;
        request.text = std::move(sentence);
        request.voice_id = voice_id_;
        request.cancelled = cancelled;
        playback->BeginRequest(request_started);
        bool first_audio = true;
        request.opus_sink = [&](const uint8_t* bytes, size_t size) {
            if (size && first_audio) {
                first_audio = false;
                ESP_LOGI(kTag, "Ali first Ogg bytes: elapsed_ms=%lld",
                         static_cast<long long>((esp_timer_get_time() - request_started) / 1000));
            }
            return playback->Write(bytes, size);
        };
        Response response;
        sent_http_request = true;
        const auto result = provider.Synthesize(request, response);
        last_header_ms = response.header_ms;
        last_activity = esp_timer_get_time();
        const bool played = result == Result::kOk && playback->Complete();
        if (response.http_status == 200) {
            ESP_LOGI(kTag,
                     "Ali Ogg stream: detail=%s eos=%d request_headers_ms=%u elapsed_ms=%lld bytes=%u "
                     "events=%u read_max_ms=%u process_max_ms=%u",
                     played ? response.stream_detail
                            : result == Result::kOk ? "ogg_incomplete" : response.stream_detail,
                     playback->HasEndOfStream(),
                     response.header_ms, static_cast<long long>((last_activity - request_started) / 1000),
                     static_cast<unsigned>(response.stream_bytes),
                     static_cast<unsigned>(response.stream_events), response.stream_max_read_ms,
                     response.stream_max_process_ms);
        }
        if (!played && !cancelled()) {
            ESP_LOGW(kTag, "Voice synthesis failed: result=%d HTTP=%d", static_cast<int>(result),
                     response.http_status);
            std::lock_guard<std::mutex> lock(mutex_);
            if (!cancelled()) {
                failed_ = true;
                queue_.clear();
                queued_bytes_ = 0;
            }
        }
    }
    provider.Close();
    if (!cancelled())
        audio.FinishPlayback(playback_generation_);
    while (!cancelled() && !audio.IsPlaybackIdle())
        vTaskDelay(pdMS_TO_TICKS(20));
    if (cancelled()) {
        ESP_LOGI(kTag, "Voice request cancelled");
    } else {
        const auto status = audio.GetPlaybackBufferStatus();
        ESP_LOGI(kTag,
                 "Ali Ogg playback: first_output_ms=%lld rebuffers=%u gap_ms=%lld "
                 "long_writes=%u longest_write_ms=%u",
                 status.first_output_ms >= turn_output_clock_ms && status.first_output_ms > 0
                     ? static_cast<long long>(status.first_output_ms - turn_output_clock_ms) : -1LL,
                 static_cast<unsigned>(status.rebuffer_count - initial.rebuffer_count),
                 static_cast<long long>(status.gap_ms - initial.gap_ms),
                 static_cast<unsigned>(status.long_write_count),
                 static_cast<unsigned>(status.longest_write_ms));
#if CONFIG_BOARD_TYPE_WZH_TAILREDPANDA || CONFIG_WZH_DIALOGUE_PARITY
        ESP_LOGI(kTag, "Ali playout pressure: pcm_starves_with_decode_work=%u late_writes=%u "
                       "short_writes=%u capture_frames_dropped=%u",
                 static_cast<unsigned>(status.decode_pending_rebuffers),
                 static_cast<unsigned>(status.late_write_count),
                 static_cast<unsigned>(status.short_write_count),
                 static_cast<unsigned>(status.encode_drop_count - initial.encode_drop_count));
#endif
    }
    CompleteWorker(generation);
}

void VoiceSession::CompleteWorker(uint32_t generation) {
    std::function<void(bool)> completion;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == generation_.load()) {
            completion = std::move(completion_);
            ok = !failed_ && !overflow_ && had_text_;
            if (!ok)
                bypass_next_.store(true);
        }
        task_ = nullptr;
        api_key_.clear();
        voice_id_.clear();
        if (pending_ && pending_->generation == generation_.load()) {
            provider_ = std::move(pending_->provider);
            api_key_ = std::move(pending_->api_key);
            voice_id_ = std::move(pending_->voice_id);
            completion_ = std::move(pending_->completion);
            playback_generation_ = pending_->playback_generation;
            active_generation_.store(pending_->generation);
            pending_.reset();
            stopping_ = false;
            ESP_LOGI(kTag, "Starting queued voice cloning provider=%s", provider_.c_str());
#if CONFIG_WZH_VOICE_PSRAM_WORKER_EXPERIMENT
            task_ = worker_task_;
#else
            if (xTaskCreate(TaskEntry, "voice_tts", 8192, this, 3, &task_) != pdPASS) {
                completion = std::move(completion_);
                ok = false;
                bypass_next_.store(true);
                api_key_.clear();
                voice_id_.clear();
                queue_.clear();
                queued_bytes_ = 0;
            }
#endif
        } else {
            pending_.reset();
            stopping_ = false;
            queue_.clear();
            queued_bytes_ = 0;
        }
    }
    if (completion)
        completion(ok);
}
}  // namespace voice_tts
