#include "wzh_onboarding.h"

#include <atomic>
#include <mutex>
#include <string_view>

#include <esp_log.h>
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
constexpr const char* kTag = "Onboarding";
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
enum class WaitReason : uint8_t { kNone, kStart, kBle, kWifi, kService };
std::atomic<WaitReason> wait_reason{WaitReason::kNone};
std::atomic<bool> ble_connected{false};
std::atomic<bool> wifi_connected{false};
std::atomic<bool> wifi_requested{false};
std::atomic<bool> wifi_narrated{false};
std::atomic<bool> service_verified{false};
std::atomic<bool> online_double_click{false};

#if CONFIG_WZH_SYSTEM_PROMPTS
const char* StepName(uint8_t step) {
    static constexpr const char* kNames[] = {"intro", "touch", "motion", "command", "network_invite",
                                             "wake", "bluetooth", "network", "connected", "ability",
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

bool Available() {
    auto& app = Application::GetInstance();
    return !stopping.load() && app.GetDeviceState() == kDeviceStateIdle &&
           !Board::GetInstance().IsAudioMaintenanceActive();
}

bool Play(std::string_view sound, bool voice_motion, uint8_t step) {
    auto& audio = Application::GetInstance().GetAudioService();
    std::unique_lock<std::mutex> reservation(PromptAudioGate(), std::defer_lock);
    const TickType_t wait_start = xTaskGetTickCount();
    while (!stopping.load()) {
        if (reservation.try_lock()) {
            if (Available() && audio.IsPlaybackIdle() &&
                !Board::GetInstance().IsLocalPlaybackPending() &&
                (!Board::GetInstance().IsOnboardingFeedbackPending() ||
                 xTaskGetTickCount() - wait_start >= pdMS_TO_TICKS(15000)) &&
                !audio.IsAudioProcessorRunning())
                break;
            reservation.unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (stopping.load())
        return false;
    const auto generation = audio.GetPlaybackGeneration();
    voice_motion_active.store(voice_motion);
    ESP_LOGI(kTag, "Voice request: tutorial=%s text=%s", StepName(step),
             OnboardingTranscript(step));
    audio.PlaySound(sound, generation);
    while (!stopping.load() && Available() && audio.GetPlaybackGeneration() == generation &&
           !audio.IsPlaybackIdle())
        vTaskDelay(pdMS_TO_TICKS(100));
    voice_motion_active.store(false);
    return !stopping.load() && Available() && audio.GetPlaybackGeneration() == generation &&
           audio.IsPlaybackIdle();
}

bool WaitFor(std::atomic<uint32_t>* counter, uint32_t before, uint32_t timeout_ms,
             uint8_t step) {
    const bool demonstration = step == 1 || step == 2 || step == 3 || step == 5;
    bool feedback_sent = false;
    if (demonstration)
        feedback_window.store(step + 1);
    const TickType_t start = xTaskGetTickCount();
    while (!stopping.load()) {
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
    progress.Interrupted();
    // Factory reset stops the worker before clearing NVS. Other maintenance
    // delays the write until it releases the shared audio and storage paths.
    while (!stopping.load() && Board::GetInstance().IsAudioMaintenanceActive())
        vTaskDelay(pdMS_TO_TICKS(100));
    return !stopping.load() && SaveOnboardingProgress(progress);
}

void Worker(void*);

bool WaitReady(WaitReason reason) {
    switch (reason) {
        case WaitReason::kStart:
            return true;
        case WaitReason::kBle:
            return ble_connected.load() || wifi_connected.load();
        case WaitReason::kWifi:
            return wifi_requested.load() || wifi_connected.load();
        case WaitReason::kService:
            return wifi_connected.load() && service_verified.load() && online_double_click.load();
        default:
            return false;
    }
}

bool StartWorkerLocked(WaitReason previous_wait) {
    wait_reason.store(WaitReason::kNone);
    active.store(true);
    worker_running.store(true);
    if (xTaskCreate(Worker, "wzh_onboarding", 4096, nullptr, 1, nullptr) == pdPASS)
        return true;
    active.store(false);
    worker_running.store(false);
    wait_reason.store(previous_wait);
    ESP_LOGE(kTag, "Cannot start tutorial worker; will retry");
    return false;
}

void FinishWorker() {
    bool restart = false;
    bool retry_wait = false;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        active.store(false);
        worker_running.store(false);
        if (restart_after_stop.exchange(false)) {
            stopping.store(false);
            started.store(false);
            restart = true;
        } else {
            retry_wait = started.load() && !stopping.load() && WaitReady(wait_reason.load());
        }
    }
    if (restart)
        StartOnboarding();
    else if (retry_wait)
        TryResumeOnboarding();
}

void Worker(void*) {
    OnboardingProgress progress;
    if (!LoadOnboardingProgress(progress)) {
        ESP_LOGE(kTag, "Cannot load tutorial progress");
        started.store(false);
        FinishWorker();
        vTaskDelete(nullptr);
        return;
    }
    if (progress.Done()) {
        ESP_LOGI(kTag, "Tutorial already completed; skipping");
        current_step.store(OnboardingProgress::kStepCount);
        started.store(false);
        FinishWorker();
        vTaskDelete(nullptr);
        return;
    }
    current_step.store(progress.step);
    ESP_LOGI(kTag, "Tutorial start/resume: step=%u name=%s", unsigned(progress.step + 1),
             StepName(progress.step));
    while (!stopping.load() && progress.step < OnboardingProgress::kStepCount) {
        if (!Available()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (progress.step == 7) {
            if (!ble_connected.load() && !wifi_connected.load()) {
                ESP_LOGI(kTag, "Waiting for mini-program BLE connection");
                wait_reason.store(WaitReason::kBle);
                break;
            }
        }
        if (progress.step == 8) {
            if (!WaitReady(WaitReason::kService)) {
                ESP_LOGI(kTag, "Waiting for network, service and double-click");
                wait_reason.store(WaitReason::kService);
                break;
            }
        }
        const auto counter = progress.step == 1   ? &touch_events
                             : progress.step == 2 ? &motion_events
                             : progress.step == 3 ? &command_events
                             : progress.step == 5 ? &wake_events
                                                  : nullptr;
        // Sampled before the prompt: an action the child already performed while
        // the sentence was playing still counts. Portal status polling cannot
        // inflate this counter because it only tracks page navigations.
        const uint32_t before = counter != nullptr ? counter->load() : 0;
        ESP_LOGI(kTag, "step=%u/%u name=%s narration=start", unsigned(progress.step + 1),
                 unsigned(OnboardingProgress::kStepCount), StepName(progress.step));
        const bool already_narrated = progress.step == 7 && wifi_narrated.load();
        if (!already_narrated && !Play(Sound(progress.step), progress.step >= 8, progress.step)) {
            if (stopping.load())
                break;
            if (!SaveInterrupted(progress)) {
                if (!stopping.load()) {
                    ESP_LOGE(kTag, "Cannot save interrupted tutorial; allowing normal use");
                    started.store(false);
                }
                break;
            }
            ESP_LOGW(kTag, "Narration interrupted; saved step=%u attempts=%u",
                     unsigned(progress.step + 1), unsigned(progress.attempts));
            current_step.store(progress.step);
            continue;
        }
        // Offline demonstrations need time for interaction; the online chapter
        // is an introduction and can flow more naturally between sentences.
        if (!already_narrated)
            WaitFor(counter, before, progress.step >= 8 ? 2500 : 6000, progress.step);
        if (progress.step == 7 && !wifi_requested.load() && !wifi_connected.load()) {
            wifi_narrated.store(true);
            wait_reason.store(WaitReason::kWifi);
            break;
        }
        if (progress.step == 7)
            wifi_narrated.store(false);
        bool satisfied = true;
        if (stopping.load())
            break;
        // An interaction can enter conversation during the window. The
        // sentence already finished, so retain it and wait for idle next step.
        while (!stopping.load() && Board::GetInstance().IsAudioMaintenanceActive())
            vTaskDelay(pdMS_TO_TICKS(100));
        if (stopping.load())
            break;
        const uint8_t finished_step = progress.step;
        progress.Finish(satisfied);
        if (!SaveOnboardingProgress(progress)) {
            ESP_LOGE(kTag, "Cannot save tutorial progress; allowing normal use");
            started.store(false);
            break;
        }
        if (progress.Done())
            ESP_LOGI(kTag, "step=%s saved; tutorial completed", StepName(finished_step));
        else
            ESP_LOGI(kTag, "step=%s saved; next=%u", StepName(finished_step),
                     unsigned(progress.step + 1));
        current_step.store(progress.step);
        if (progress.step == OnboardingProgress::kStepCount && !progress.Done()) {
            // Interrupted/skipped chapters remain incomplete. Retry them
            // instead of allowing conversation to bypass the remaining guide.
            progress.Resume();
            current_step.store(progress.step);
            if (!SaveOnboardingProgress(progress)) {
                ESP_LOGE(kTag, "Cannot save tutorial retry; allowing normal use");
                started.store(false);
                break;
            }
        }
    }
    if (progress.Done())
        started.store(false);
    FinishWorker();
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
    wifi_connected.store(WifiManager::GetInstance().IsConnected());
    StartWorkerLocked(WaitReason::kStart);
#else
    // No tutorial worker without a voice pack; keep the atomics coherent so a
    // later StopOnboarding()/NotifyOnboarding() stays harmless.
    wait_reason.store(WaitReason::kNone);
#endif
}

void TryResumeOnboarding() {
#if CONFIG_WZH_SYSTEM_PROMPTS
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    const auto reason = wait_reason.load();
    if (!started.load() || stopping.load() || worker_running.load() || !WaitReady(reason))
        return;
    StartWorkerLocked(reason);
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
        case OnboardingEvent::kServiceReady:
            ESP_LOGI(kTag, "Conversation service connected");
            service_verified.store(true);
            break;
        case OnboardingEvent::kServiceConfigured:
            ESP_LOGI(kTag, "Conversation service configured; connection deferred until chat");
            service_verified.store(true);
            break;
        case OnboardingEvent::kServiceLost:
            service_verified.store(false);
            break;
        case OnboardingEvent::kNetworkConnected:
            wifi_connected.store(true);
            break;
        case OnboardingEvent::kNetworkLost:
            wifi_connected.store(false);
            service_verified.store(false);
            break;
        case OnboardingEvent::kBleConnected:
            ble_connected.store(true);
            break;
        case OnboardingEvent::kBleDisconnected:
            ble_connected.store(false);
            break;
        case OnboardingEvent::kWifiRequest:
            wifi_requested.store(true);
            break;
    }
    TryResumeOnboarding();
}
void PrepareOnboardingDoubleClick() {
#if CONFIG_WZH_SYSTEM_PROMPTS
    const auto step = current_step.load();
    if (started.load() && step == 5)
        ++wake_events;
    if (started.load() && step == 8) {
        online_double_click.store(true);
        TryResumeOnboarding();
    }
#endif
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
bool IsOnboardingActive() { return active.load(); }
bool IsOnboardingVoiceMotionActive() { return voice_motion_active.load(); }
bool OnboardingBlocksConversation() {
    return started.load() && !stopping.load() && current_step.load() < OnboardingProgress::kStepCount;
}
void StopOnboarding() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    restart_after_stop.store(false);
    voice_motion_active.store(false);
    online_double_click.store(false);
    wait_reason.store(WaitReason::kNone);
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
