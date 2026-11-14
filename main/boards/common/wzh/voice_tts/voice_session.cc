#include "voice_session.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <new>

#include "ali_provider.h"
#if CONFIG_WZH_ALI_STREAM_EXPERIMENT
#include "ali_stream_client.h"
#endif
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "voice_config.h"
#include "voice_flash_dispatcher.h"
#include "voice_text_batch.h"
#include "voice_text_filter.h"
#include "wav_playback.h"

namespace voice_tts {
namespace {
[[maybe_unused]] constexpr char kTag[] = "VoiceTts";
constexpr size_t kMaxPendingSentences = 64;
constexpr size_t kMaxPendingBytes = 16 * 1024;

// A single bounded PSRAM ring decouples network reception from I2S playback.
// It also lets the producer start the next sentence while audio remains queued.
class AliPcmBuffer {
public:
    AliPcmBuffer(AudioService& audio, uint32_t generation, std::function<bool()> cancelled)
        : audio_(audio), generation_(generation), cancelled_(std::move(cancelled)) {}
    bool Start() {
        data_ =
            static_cast<uint8_t*>(heap_caps_malloc(kCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!data_) {
            ESP_LOGW(
                kTag, "Ali PCM allocation failed: psram_free=%u largest=%u",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(
                    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
            return false;
        }
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM
        external_task_ = true;
        const BaseType_t created = xTaskCreateWithCaps(Entry, "ali_pcm", 8192, this, 3, &task_,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        const BaseType_t created = xTaskCreate(Entry, "ali_pcm", 8192, this, 3, &task_);
#endif
        if (created != pdPASS) {
            ESP_LOGW(kTag, "Ali PCM task creation failed: internal_free=%u largest=%u",
                     static_cast<unsigned>(
                         heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                     static_cast<unsigned>(
                         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
            free(data_);
            data_ = nullptr;
            return false;
        }
        return true;
    }
    bool Write(const uint8_t* input, size_t length) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (length && !cancelled_() && !failed_) {
            if (size_ == kCapacity) {
                cv_.wait_for(lock, std::chrono::milliseconds(50));
                continue;
            }
            const size_t count = std::min({length, kCapacity - size_, kCapacity - write_});
            std::memcpy(data_ + write_, input, count);
            write_ = (write_ + count) % kCapacity;
            size_ += count;
            written_ += count;
            input += count;
            length -= count;
            cv_.notify_all();
        }
        return !length && !cancelled_() && !failed_;
    }
    void CompleteSegment() {
        std::lock_guard<std::mutex> lock(mutex_);
        completed_ = written_;
        cv_.notify_all();
    }
    bool Finish() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
            cv_.notify_all();
        }
        while (!done_.load())
            vTaskDelay(pdMS_TO_TICKS(20));
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM
        if (external_task_) {
            while (eTaskGetState(task_) != eSuspended)
                vTaskDelay(pdMS_TO_TICKS(1));
            vTaskDeleteWithCaps(task_);
            task_ = nullptr;
        }
#endif
        free(data_);
        data_ = nullptr;
        return !failed_ && !cancelled_();
    }

private:
    static constexpr size_t kCapacity = 192000;  // four seconds of 24 kHz PCM16
    static constexpr size_t kPrefix = 48000;     // 1000 ms of 24 kHz PCM16
    static void Entry(void* context) {
        static_cast<AliPcmBuffer*>(context)->Play();
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM
        // The producer reclaims the task after observing done_, avoiding the
        // SDK self-delete path which needs another internal-memory task.
        vTaskSuspend(nullptr);
#else
        vTaskDelete(nullptr);
#endif
    }
    void Play() {
        bool priming = true;
        bool have_output = false;
        unsigned underruns = 0;
        [[maybe_unused]] uint64_t starvation_ms = 0;
        while (!cancelled_()) {
            uint8_t bytes[2880];
            size_t count = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (priming && !finished_) {
                    // Start the deadline when audio actually arrives, not while
                    // the HTTPS handshake is still in progress. A completed
                    // short sentence can play immediately without filling the
                    // whole prefix.
                    while (size_ < 2 && !finished_ && !cancelled_())
                        cv_.wait_for(lock, std::chrono::milliseconds(50));
                    const size_t prefix = have_output ? 24000 : kPrefix;
                    const auto until = std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(have_output ? 1000 : 2000);
                    while (size_ < prefix && completed_ <= consumed_ && !finished_ &&
                           !cancelled_()) {
                        if (cv_.wait_until(lock, until) == std::cv_status::timeout)
                            break;
                    }
                    priming = false;
                }
                const auto wait_start = std::chrono::steady_clock::now();
                while (size_ < 2 && !finished_ && !cancelled_()) {
                    cv_.wait_for(lock, std::chrono::milliseconds(50));
                    if (size_ >= 2) {
                        if (have_output) {
                            ++underruns;
                            starvation_ms += std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - wait_start)
                                                 .count();
                        }
                        priming = true;
                        break;
                    }
                }
                if (priming)
                    continue;
                if (cancelled_() || (finished_ && size_ < 2)) {
                    if (finished_ && size_ != 0)
                        failed_ = true;
                    break;
                }
                // Do not submit tiny network fragments as separate audio
                // writes. Accumulate one 60 ms frame, except for the tail of
                // a completed sentence or the end of the turn.
                while (size_ < sizeof(bytes) && completed_ <= consumed_ && !finished_ &&
                       !cancelled_()) {
                    cv_.wait_for(lock, std::chrono::milliseconds(50));
                }
                if (cancelled_())
                    break;
                count = std::min(size_ & ~size_t(1), sizeof(bytes));
                const size_t first = std::min(count, kCapacity - read_);
                std::memcpy(bytes, data_ + read_, first);
                std::memcpy(bytes + first, data_, count - first);
                read_ = (read_ + count) % kCapacity;
                size_ -= count;
                consumed_ += count;
                cv_.notify_all();
            }
            std::vector<int16_t> pcm(count / 2);
            for (size_t i = 0; i < pcm.size(); ++i) {
                const uint16_t word = uint16_t(bytes[2 * i]) | (uint16_t(bytes[2 * i + 1]) << 8);
                pcm[i] = word < 0x8000 ? static_cast<int16_t>(word)
                                       : static_cast<int16_t>(static_cast<int32_t>(word) - 65536);
            }
            if (!audio_.PushPcmToPlaybackQueue(std::move(pcm), 24000, generation_, true)) {
                std::lock_guard<std::mutex> lock(mutex_);
                failed_ = true;
                cv_.notify_all();
                break;
            }
            if (!have_output)
                ESP_LOGI(kTag, "Ali PCM first frame queued");
            have_output = true;
        }
        ESP_LOGI(kTag, "Ali PCM buffer underruns=%u starvation_ms=%llu", underruns,
                 static_cast<unsigned long long>(starvation_ms));
        done_.store(true);
    }
    AudioService& audio_;
    uint32_t generation_;
    std::function<bool()> cancelled_;
    std::mutex mutex_;
    std::condition_variable cv_;
    uint8_t* data_ = nullptr;
    size_t read_ = 0, write_ = 0, size_ = 0;
    uint64_t written_ = 0, completed_ = 0, consumed_ = 0;
    bool finished_ = false, failed_ = false;
    std::atomic<bool> done_{false};
    TaskHandle_t task_ = nullptr;
    bool external_task_ = false;
};
}  // namespace

bool VoiceSession::Ready() { return ReadyForVoice(""); }

bool VoiceSession::ReadyForVoice(const std::string& voice) {
    if (bypass_next_.exchange(false)) {
        ESP_LOGW(kTag, "External speech unavailable: one-turn fallback after earlier failure");
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_ && (!stopping_ || pending_)) {
        ESP_LOGW(kTag, "External speech unavailable: previous task is still stopping");
        return false;
    }
    if (LoadAliKey().empty() || (voice.empty() && LoadAliVoice().empty())) {
        ESP_LOGW(kTag, "External speech unavailable: provider configuration incomplete");
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
    if ((task_ && (!stopping_ || pending_)) || !completion || key.empty() || voice.empty())
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
    const int output_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
    AliProvider provider;
    AliPcmBuffer ali_buffer(audio, playback_generation_, cancelled);
    const bool ali_started = !cancelled() && output_rate == 24000 && ali_buffer.Start();
    if (!ali_started && !cancelled()) {
        ESP_LOGW(kTag, "Ali PCM buffer unavailable");
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cancelled())
            failed_ = true;
    }
    int64_t last_activity = esp_timer_get_time();
#if CONFIG_WZH_ALI_STREAM_EXPERIMENT
    bool use_http = true;
    if (ali_started && !cancelled()) {
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
        stream.pcm_sink = [&ali_buffer](const uint8_t* data, size_t size) {
            return ali_buffer.Write(data, size);
        };
        stream.sentence_end = [&ali_buffer]() { ali_buffer.CompleteSegment(); };
        StreamResponse response;
        const Result result = SynthesizeStream(stream, response);
        if (result == Result::kOk) {
            use_http = false;
        } else if (response.text_committed) {
            use_http = false;
            if (!cancelled()) {
                ESP_LOGW(kTag, "Duplex synthesis failed after text was committed: result=%d",
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
    while (use_http && !cancelled() && ali_started) {
#else
    while (!cancelled() && ali_started) {
#endif
        std::string sentence;
        bool done = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled())
                break;
            if (failed_) {
                queue_.clear();
                queued_bytes_ = 0;
                done = finished_;
            } else if (!queue_.empty()) {
                sentence = PopTextBatch(queue_, queued_bytes_, true);
            } else {
                done = finished_;
            }
        }
        if (done)
            break;
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
        bool has_pending_byte = false;
        bool logged_first_pcm = false;
        {
            request.pcm_sink = [&](const uint8_t* bytes, size_t size) {
                if (output_rate != 24000)
                    return false;
                if (size && !logged_first_pcm) {
                    ESP_LOGI(
                        kTag, "Ali first PCM received: elapsed_ms=%u",
                        static_cast<unsigned>((esp_timer_get_time() - request_started) / 1000));
                    logged_first_pcm = true;
                }
                has_pending_byte = has_pending_byte != ((size & 1) != 0);
                return ali_buffer.Write(bytes, size);
            };
        }
        Response response;
        ESP_LOGI(kTag, "Requesting speech provider=%s", provider_.c_str());
        auto result = provider.Synthesize(request, response);
        last_activity = esp_timer_get_time();
        if (response.http_status == 200) {
            ESP_LOGI(kTag,
                     "Ali stream stats: detail=%s bytes=%u events=%u read_max_ms=%u "
                     "process_max_ms=%u",
                     response.stream_detail, static_cast<unsigned>(response.stream_bytes),
                     static_cast<unsigned>(response.stream_events), response.stream_max_read_ms,
                     response.stream_max_process_ms);
        }
        if (result == Result::kOk && !cancelled()) {
            if (response.streamed) {
                ESP_LOGI(kTag, "Ali PCM stream complete: HTTP=%d elapsed_ms=%u",
                         response.http_status,
                         static_cast<unsigned>((last_activity - request_started) / 1000));
            } else {
                ESP_LOGI(kTag, "Speech response: HTTP=%d audio_bytes=%u elapsed_ms=%u",
                         response.http_status, static_cast<unsigned>(response.wav.size()),
                         static_cast<unsigned>((last_activity - request_started) / 1000));
            }
        }
        const bool played =
            result == Result::kOk && (response.streamed ? !has_pending_byte
                                                        : PlayWav(audio, response.wav, output_rate,
                                                                  playback_generation_, cancelled));
        if (played && response.streamed)
            ali_buffer.CompleteSegment();
        if (played && !cancelled())
            ESP_LOGI(kTag, "Speech audio queued for playback");
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
    if (ali_started && !ali_buffer.Finish() && !cancelled()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cancelled())
            failed_ = true;
    }
    if (!cancelled())
        audio.FinishPlayback(playback_generation_);
    while (!cancelled() && !audio.IsPlaybackIdle())
        vTaskDelay(pdMS_TO_TICKS(20));
    if (cancelled())
        ESP_LOGI(kTag, "Voice request cancelled");
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
