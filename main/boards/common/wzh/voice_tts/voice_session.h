#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "external_tts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace voice_tts {
class VoiceSession final : public ExternalTts {
public:
    bool Ready() override;
    bool Begin(uint32_t playback_generation, std::function<void(bool)> completion) override;
    bool ReadyForVoice(const std::string& voice) override;
    bool BeginWithVoice(uint32_t playback_generation, const std::string& voice,
                        std::function<void(bool)> completion) override;
    bool Enqueue(const std::string& text) override;
    bool ShouldUseServerAudio(const std::string& text) const override;
    bool ShouldResumeExternalAudio(const std::string& text) const override;
    void Finish() override;
    void Cancel() override;

private:
    static void TaskEntry(void* context);
    void Run(uint32_t generation);
    void CompleteWorker(uint32_t generation);
    struct PendingTurn {
        std::string provider, api_key, voice_id;
        std::function<void(bool)> completion;
        uint32_t playback_generation = 0;
        uint32_t generation = 0;
    };
    std::mutex mutex_;
    std::deque<std::string> queue_;
    std::function<void(bool)> completion_;
    std::atomic<uint32_t> generation_{0};
    std::atomic<uint32_t> active_generation_{0};
    std::atomic<bool> bypass_next_{false};
    TaskHandle_t task_ = nullptr;
#if CONFIG_WZH_VOICE_PSRAM_WORKER_EXPERIMENT
    // Persistent worker is owned by the board singleton and never self-deletes.
    TaskHandle_t worker_task_ = nullptr;
#endif
    std::unique_ptr<PendingTurn> pending_;
    bool stopping_ = false;
    uint32_t playback_generation_ = 0;
    bool finished_ = false;
    bool failed_ = false;
    bool had_text_ = false;
    bool overflow_ = false;
    size_t queued_bytes_ = 0;
    std::string provider_;
    std::string api_key_;
    std::string voice_id_;
};
}  // namespace voice_tts
