#include "wzh_onboarding.h"

#include <atomic>
#include <mutex>
#include <string_view>
#include <utility>

#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "wzh_onboarding_progress.h"
#include "wzh_onboarding_store.h"
#include "wzh_onboarding_transcript.h"
#include "wzh_prompt_audio_gate.h"

namespace wzh {
namespace {
#if CONFIG_WZH_SYSTEM_PROMPTS
constexpr const char* kTag = "Onboarding";
constexpr uint8_t kFirstConversationStep = 8;
#endif
std::atomic<bool> active{false};
std::atomic<bool> voice_motion_active{false};
std::atomic<bool> started{false};
std::atomic<bool> stopping{false};
std::atomic<bool> worker_running{false};
std::atomic<bool> restart_after_stop{false};
std::atomic<uint8_t> current_step{0};
std::atomic<uint8_t> feedback_window{0};
std::mutex lifecycle_mutex;
std::atomic<uint32_t> touch_events{0}, motion_events{0}, wake_events{0}, command_events{0};
std::atomic<uint32_t> portal_requests{0};
std::atomic<uint32_t> intro_request{0};
std::atomic<bool> intro_network_lost{false};
#if CONFIG_WZH_SYSTEM_PROMPTS
uint32_t next_intro_request = 0;  // Protected by lifecycle_mutex.
#endif
std::function<void(bool)> resume_conversation;  // Protected by lifecycle_mutex.

#if CONFIG_WZH_SYSTEM_PROMPTS
const char* StepName(uint8_t step) {
    static constexpr const char* kNames[] = {"intro", "touch", "motion", "command", "network_invite",
                                             "wake", "hotspot", "page", "connected", "ability",
                                             "interrupt", "voice", "farewell"};
    return step < OnboardingProgress::kStepCount ? kNames[step] : "unknown";
}

std::string_view Sound(uint8_t step) {
    switch (step) {
        case 0:
            return Lang::Sounds::OGG_INTRO;
        case 1:
            return Lang::Sounds::OGG_TOUCH;
        case 2:
            return Lang::Sounds::OGG_MOTION;
        case 3:
            return Lang::Sounds::OGG_CMD1;
        case 4:
            return Lang::Sounds::OGG_NET_INVITE;
        case 5:
            return Lang::Sounds::OGG_WAKE;
        case 6:
            return Lang::Sounds::OGG_AP;
        case 7:
            return Lang::Sounds::OGG_PAGE;
        case 8:
            return Lang::Sounds::OGG_CONNECTED;
        case 9:
            return Lang::Sounds::OGG_ABILITY;
        case 10:
            return Lang::Sounds::OGG_INTERRUPT;
        case 11:
            return Lang::Sounds::OGG_CLONE;
        case 12:
            return Lang::Sounds::OGG_FAREWELL;
        default:
            return {};
    }
}

bool Available(uint32_t request = 0) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    if (stopping.load() || board.IsAudioMaintenanceActive())
        return false;
    return request == 0
               ? app.GetDeviceState() == kDeviceStateIdle
               : intro_request.load() == request && !intro_network_lost.load() &&
                     app.GetDeviceState() == kDeviceStateConnecting && board.IsNetworkAvailable();
}

bool IsWifiStep(uint8_t step) { return step == 4 || step == 6 || step == 7; }

bool SkipWifiStep(uint8_t step) {
    return IsWifiStep(step) && Board::GetInstance().UsesCellularNetwork();
}

bool Play(std::string_view sound, bool voice_motion, uint8_t step, const char* message = nullptr,
          uint32_t request = 0, bool wait_for_audio = true) {
    auto& audio = Application::GetInstance().GetAudioService();
    auto& board = Board::GetInstance();
    std::unique_lock<std::mutex> reservation(PromptAudioGate(), std::defer_lock);
    const TickType_t wait_start = xTaskGetTickCount();
    uint32_t generation = 0;
    while (!stopping.load() && !SkipWifiStep(step) && (request == 0 || Available(request))) {
        if (request != 0 && xTaskGetTickCount() - wait_start >= pdMS_TO_TICKS(8000))
            return false;
        if (reservation.try_lock()) {
            generation = audio.GetPlaybackGeneration();
            if (Available(request) && (!IsWifiStep(step) || !board.IsNetworkProbing()) &&
                audio.IsPlaybackIdle() && !board.IsLocalPlaybackPending() &&
                (!board.IsOnboardingFeedbackPending() ||
                 xTaskGetTickCount() - wait_start >= pdMS_TO_TICKS(15000)) &&
                !audio.IsAudioProcessorRunning())
                break;
            reservation.unlock();
        }
        if (!wait_for_audio)
            return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!reservation.owns_lock() || !Available(request) || SkipWifiStep(step))
        return false;
    voice_motion_active.store(voice_motion);
    ESP_LOGI(kTag, "Voice request: tutorial=%s text=%s", StepName(step),
             message ? message : OnboardingTranscript(step));
    audio.PlaySound(sound, generation);
    while (!SkipWifiStep(step) && Available(request) &&
           audio.GetPlaybackGeneration() == generation && !audio.IsPlaybackIdle())
        vTaskDelay(pdMS_TO_TICKS(100));
    voice_motion_active.store(false);
    if (SkipWifiStep(step)) {
        if (!audio.IsPlaybackIdle())
            audio.ResetDecoder(generation);
        return false;
    }
    return Available(request) && audio.GetPlaybackGeneration() == generation &&
           audio.IsPlaybackIdle();
}

bool WaitFor(std::atomic<uint32_t>* counter, uint32_t before, uint32_t timeout_ms, uint8_t step) {
    const bool demonstration = step == 1 || step == 2 || step == 3 || step == 5;
    bool feedback_sent = false;
    if (demonstration)
        feedback_window.store(step + 1);
    const TickType_t start = xTaskGetTickCount();
    while (!stopping.load() && !SkipWifiStep(step)) {
        const auto elapsed = xTaskGetTickCount() - start;
        const bool answered = counter && counter->load() != before;
        if (demonstration && !feedback_sent && (answered || elapsed >= pdMS_TO_TICKS(timeout_ms)) &&
            Available() && !Board::GetInstance().IsLocalPlaybackPending() &&
            Application::GetInstance().GetAudioService().IsPlaybackIdle()) {
            feedback_sent = Board::GetInstance().TriggerOnboardingFeedback(step);
            if (feedback_sent)
                ESP_LOGI(kTag, "step=%s feedback=%s", StepName(step),
                         answered ? "interaction" : "demo");
        }
        if (elapsed >= pdMS_TO_TICKS(timeout_ms) &&
            (!demonstration || feedback_sent || elapsed >= pdMS_TO_TICKS(timeout_ms + 3000)))
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    feedback_window.store(0);
    if (demonstration && !feedback_sent && !stopping.load())
        ESP_LOGW(kTag, "step=%s feedback unavailable", StepName(step));
    return counter == nullptr || counter->load() != before;
}

bool SaveInterrupted(OnboardingProgress& progress) {
    if (progress.step < 8) {
        // The first chapter must finish before double-click is enabled.
        // Retry an interrupted sentence instead of skipping past the gate.
        progress.attempts = (progress.attempts + 1) % OnboardingProgress::kMaxAttempts;
    } else {
        progress.Interrupted();
    }
    // Factory reset stops the worker before clearing NVS. Other maintenance
    // delays the write until it releases the shared audio and storage paths.
    while (!stopping.load() && Board::GetInstance().IsAudioMaintenanceActive())
        vTaskDelay(pdMS_TO_TICKS(100));
    return !stopping.load() && SaveOnboardingProgress(progress);
}

void FinishIntro(uint32_t request, bool played = false) {
    std::function<void(bool)> resume;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        if (request == 0 || intro_request.load() != request)
            return;
        intro_request.store(0);
        resume = std::move(resume_conversation);
    }
    if (resume)
        Application::GetInstance().Schedule(
            [resume = std::move(resume), played]() { resume(played); });
}

void FinishWorker(bool played = false) {
    bool restart = false;
    uint32_t request = 0;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        // A worker that cannot continue must release the conversation gate.
        // The persisted step still determines where the next attempt begins.
        current_step.store(OnboardingProgress::kStepCount);
        active.store(false);
        worker_running.store(false);
        request = intro_request.load();
        if (restart_after_stop.exchange(false)) {
            stopping.store(false);
            started.store(false);
            restart = true;
        }
    }
    FinishIntro(request, played);
    if (restart)
        StartOnboarding();
}

void Worker(void*) {
    OnboardingProgress progress;
    if (!LoadOnboardingProgress(progress)) {
        ESP_LOGE(kTag, "Cannot load tutorial progress");
        FinishWorker();
        vTaskDelete(nullptr);
        return;
    }
    current_step.store(progress.step);
    if (progress.Done()) {
        ESP_LOGI(kTag, "Tutorial already completed; skipping");
        FinishWorker();
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(kTag, "Tutorial start/resume: step=%u name=%s", unsigned(progress.step + 1),
             StepName(progress.step));
    bool waiting_for_intro = false;
    bool prompted_cellular_wait = false;
    uint32_t request = 0;
    while (!stopping.load() && progress.step < OnboardingProgress::kStepCount) {
        if (progress.step < kFirstConversationStep && !Available()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // Skip Wi-Fi setup instructions when cellular is the selected network.
        if (SkipWifiStep(progress.step)) {
            ESP_LOGI(kTag, "Skipping Wi-Fi-dependent step=%s for cellular",
                     StepName(progress.step));
            progress.Finish(true);
            if (!SaveOnboardingProgress(progress)) {
                ESP_LOGE(kTag, "Cannot save tutorial progress");
                break;
            }
            current_step.store(progress.step);
            continue;
        }
        if (progress.step == 7) {
            ESP_LOGI(kTag, "step=page waiting for hotspot client");
            // Wait for a phone on the AP unless the device is already online.
            // Keep the saved step while waiting.
            wifi_sta_list_t stations{};
            while (!stopping.load() && !SkipWifiStep(progress.step)) {
                if (Available()) {
                    if (Board::GetInstance().IsNetworkAvailable())
                        break;
                    if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK && stations.num > 0) {
                        ESP_LOGI(kTag, "step=page hotspot client connected");
                        break;
                    }
                }
                // Waiting for the phone should not prevent idle power saving.
                active.store(false);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (stopping.load())
                break;
            if (SkipWifiStep(progress.step))
                continue;
            active.store(true);
        }
        if (progress.step >= kFirstConversationStep) {
            if (!waiting_for_intro) {
                waiting_for_intro = true;
                ESP_LOGI(kTag, "Waiting for the first conversation double click");
            }
            request = intro_request.load();
            if (request == 0 || !Available(request)) {
                // The worker can resume on the first double click while the
                // normal idle behavior uses the motion and audio lanes.
                active.store(false);
                FinishIntro(request);
                auto& board = Board::GetInstance();
                if (progress.step == kFirstConversationStep && !prompted_cellular_wait &&
                    board.UsesCellularNetwork() && !board.IsNetworkAvailable() && Available()) {
                    const auto grace_start = xTaskGetTickCount();
                    while (!stopping.load() && !board.IsNetworkAvailable() &&
                           xTaskGetTickCount() - grace_start < pdMS_TO_TICKS(15000))
                        vTaskDelay(pdMS_TO_TICKS(100));
                    prompted_cellular_wait = true;
                    if (!board.IsNetworkAvailable() && Available()) {
                        // This optional cue must yield to local playback and
                        // leave the worker available for a new intro request.
                        active.store(true);
                        Play(Lang::Sounds::OGG_ADV_WIFI_WAITING, false, progress.step,
                             "还没连上，我等着呢。", 0, false);
                        active.store(false);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            active.store(true);
        }
        const auto counter = progress.step == 1   ? &touch_events
                             : progress.step == 2 ? &motion_events
                             : progress.step == 3 ? &command_events
                             : progress.step == 5 ? &wake_events
                             : progress.step == 7 ? &portal_requests
                                                  : nullptr;
        // Sampled before the prompt: an action the child already performed while
        // the sentence was playing still counts. Portal status polling cannot
        // inflate this counter because it only tracks page navigations.
        const uint32_t before = counter != nullptr ? counter->load() : 0;
        ESP_LOGI(kTag, "step=%u/%u name=%s narration=start", unsigned(progress.step + 1),
                 unsigned(OnboardingProgress::kStepCount), StepName(progress.step));
        if (!Play(Sound(progress.step), progress.step >= kFirstConversationStep, progress.step,
                  nullptr, request)) {
            if (stopping.load())
                break;
            if (SkipWifiStep(progress.step))
                continue;  // Save the skip before advancing on the next iteration.
            if (request != 0) {
                FinishIntro(request);
                continue;
            }
            if (!SaveInterrupted(progress))
                break;
            ESP_LOGW(kTag, "Narration interrupted; saved step=%u attempts=%u",
                     unsigned(progress.step + 1), unsigned(progress.attempts));
            current_step.store(progress.step);
            continue;
        }
        if (progress.step < kFirstConversationStep)
            WaitFor(counter, before, 6000, progress.step);
        if (stopping.load())
            break;
        // An interaction can enter conversation during the window. The
        // sentence already finished, so retain it and wait for idle next step.
        while (!stopping.load() && Board::GetInstance().IsAudioMaintenanceActive())
            vTaskDelay(pdMS_TO_TICKS(100));
        if (stopping.load())
            break;
        const uint8_t finished_step = progress.step;
        progress.Finish(true);
        if (!SaveOnboardingProgress(progress)) {
            ESP_LOGE(kTag, "Cannot save tutorial progress");
            break;
        }
        if (progress.Done())
            ESP_LOGI(kTag, "step=%s saved; tutorial completed", StepName(finished_step));
        else
            ESP_LOGI(kTag, "step=%s saved; next=%u", StepName(finished_step),
                     unsigned(progress.step + 1));
        current_step.store(progress.step);
    }
    FinishWorker(progress.Done());
    vTaskDelete(nullptr);
}
#endif
}  // namespace

void StartOnboarding() {
#if CONFIG_WZH_SYSTEM_PROMPTS
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    if (stopping.load())
        return;
    if (started.exchange(true))
        return;
    current_step.store(0);
    active.store(true);
    worker_running.store(true);
    if (xTaskCreate(Worker, "wzh_onboarding", 4096, nullptr, 1, nullptr) != pdPASS) {
        current_step.store(OnboardingProgress::kStepCount);
        active.store(false);
        worker_running.store(false);
        started.store(false);
        ESP_LOGE(kTag, "Cannot start tutorial worker");
    }
#else
    // No tutorial worker without a voice pack; keep the atomics coherent so a
    // later StopOnboarding()/NotifyOnboarding() stays harmless.
    portal_requests.store(0);
#endif
}

void NotifyOnboarding(OnboardingEvent event) {
    switch (event) {
        case OnboardingEvent::kTouch:
            ++touch_events;
            break;
        case OnboardingEvent::kMotion:
            ++motion_events;
            break;
        case OnboardingEvent::kWake:
            ++wake_events;
            break;
        case OnboardingEvent::kCommand:
            ++command_events;
            break;
        case OnboardingEvent::kNetworkLost:
            intro_network_lost.store(true);
            break;
    }
}
bool DeferOnboardingConversation(std::function<void(bool)> resume) {
#if CONFIG_WZH_SYSTEM_PROMPTS
    if (!Board::GetInstance().IsNetworkAvailable() ||
        Application::GetInstance().GetDeviceState() != kDeviceStateConnecting)
        return false;
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    const auto step = current_step.load();
    if (stopping.load() || !worker_running.load() || step < kFirstConversationStep ||
        step >= OnboardingProgress::kStepCount)
        return false;
    resume_conversation = std::move(resume);
    if (++next_intro_request == 0)
        ++next_intro_request;
    intro_network_lost.store(false);
    intro_request.store(next_intro_request);
    return true;
#else
    (void)resume;
    return false;
#endif
}

bool IsOnboardingIntroPending() {
#if CONFIG_WZH_SYSTEM_PROMPTS
    return !stopping.load() && current_step.load() < OnboardingProgress::kStepCount &&
           (!started.load() || worker_running.load());
#else
    return false;
#endif
}
bool IsOnboardingConversationIntroActive() {
    return intro_request.load() != 0;
}
bool CaptureOnboardingInteraction(OnboardingEvent event) {
    const auto window = feedback_window.load();
    const bool match = (window == 2 && event == OnboardingEvent::kTouch) ||
                       (window == 3 && event == OnboardingEvent::kMotion) ||
                       (window == 4 && event == OnboardingEvent::kCommand) ||
                       (window == 6 && event == OnboardingEvent::kWake);
    if (match)
        NotifyOnboarding(event);
    return match;
}
void NotifyPortalRequest() { ++portal_requests; }

bool IsOnboardingActive() { return active.load(); }
bool IsOnboardingWorkerRunning() { return worker_running.load(); }
bool IsOnboardingVoiceMotionActive() { return voice_motion_active.load(); }
bool OnboardingBlocksConversation() {
#if CONFIG_WZH_SYSTEM_PROMPTS
    return !stopping.load() && current_step.load() < kFirstConversationStep;
#else
    return false;
#endif
}
void StopOnboarding() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    restart_after_stop.store(false);
    voice_motion_active.store(false);
    intro_request.store(0);
    resume_conversation = {};
    stopping.store(true);
}

void ResumeOnboardingAfterAbort() {
#if CONFIG_WZH_SYSTEM_PROMPTS
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        if (!stopping.load())
            return;
        if (worker_running.load()) {
            restart_after_stop.store(true);
            return;
        }
        stopping.store(false);
        started.store(false);
    }
    StartOnboarding();
#else
    stopping.store(false);
    started.store(false);
#endif
}

}  // namespace wzh
