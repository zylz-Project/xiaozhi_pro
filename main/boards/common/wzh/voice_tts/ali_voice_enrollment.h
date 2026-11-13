#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice_enrollment.h"

namespace voice_tts {
class AliVoiceEnrollment final : public VoiceEnrollment {
public:
    bool Start(std::function<void(bool)> capture_switch,
               std::function<void(bool)> complete) override;
    bool StartNamed(const std::string& name, std::function<void(bool)> capture_switch,
                    std::function<void(bool)> complete) override;
    bool Capture(const int16_t* samples, size_t count, bool speech) override;
    void Cancel() override;
    const char* Status() const override;
    const char* Failure() const override;
    bool IsConfigured() const override;
    bool SelectionPending() const override;
    bool StartSelection(const std::string& text, std::function<void(bool)> complete) override;

private:
    static void TaskEntry(void* arg);
#if CONFIG_WZH_VOICE_FLASH_DISPATCH_EXPERIMENT
    bool EnsureWorker(const char* name);
#endif
    void Run(uint32_t generation, const std::string& name);
    void RunSelection(uint32_t generation, const std::string& text);
    bool Speak(const char* text, uint32_t generation);
    std::mutex mutex_;
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> active_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> audio_done_{false};
    std::atomic<bool> prompt_done_{false};
    std::atomic<bool> prompt_ok_{false};
    std::atomic<int64_t> selection_deadline_us_{0};
    std::string selection_text_;
    std::string requested_name_;
    bool selection_task_ = false;
    enum class Phase { kIdle, kPrompt, kRecording, kProcessing, kSuccess, kFailed, kCancelled };
    enum class Error {
        kNone,
        kBusy,
        kConfig,
        kPrompt,
        kMemory,
        kShort,
        kCloud,
        kAuth,
        kLimit,
        kVoiceFull,
        kUncertain,
        kSave
    };
    std::atomic<Phase> phase_{Phase::kIdle};
    std::atomic<Error> error_{Error::kNone};
    void PublishPhase(Phase phase, uint32_t generation);
    TaskHandle_t task_ = nullptr;
#if CONFIG_WZH_VOICE_FLASH_DISPATCH_EXPERIMENT
    TaskHandle_t worker_task_ = nullptr;
#endif
    int16_t* samples_ = nullptr;
    size_t count_ = 0;
    size_t speech_samples_ = 0;
    size_t first_speech_ = 0;
    size_t last_speech_ = 0;
    size_t silence_samples_ = 0;
    std::function<void(bool)> capture_switch_;
    std::function<void(bool)> complete_;
};
}  // namespace voice_tts
