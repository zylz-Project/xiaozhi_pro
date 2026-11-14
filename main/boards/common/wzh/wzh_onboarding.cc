#include "wzh_onboarding.h"

#include <atomic>
#include <mutex>
#include <string_view>

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
std::atomic<bool> service_verified{false};
std::atomic<bool> online_double_click{false};
std::atomic<bool> connection_probe{false};

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

void FinishWorker() {
    bool restart = false;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        active.store(false);
        worker_running.store(false);
        if (restart_after_stop.exchange(false)) {
            stopping.store(false);
            started.store(false);
            restart = true;
        }
    }
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
    if (progress.Done()) {
        ESP_LOGI(kTag, "Tutorial already completed; skipping");
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
            ESP_LOGI(kTag, "step=page waiting for hotspot client");
            // Wait for a phone on the AP unless the device is already online.
            // Keep the saved step while waiting.
            wifi_sta_list_t stations{};
            while (!stopping.load()) {
                if (WifiManager::GetInstance().IsConnected())
                    break;
                if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK && stations.num > 0 &&
                    Available()) {
                    active.store(true);
                    ESP_LOGI(kTag, "step=page hotspot client connected");
                    break;
                }
                // Waiting for the phone should not prevent idle power saving.
                active.store(false);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (stopping.load())
                break;
        }
        if (progress.step == 8) {
            ESP_LOGI(kTag, "step=connected waiting for double click and conversation service");
            const auto start = xTaskGetTickCount();
            while (!stopping.load() && xTaskGetTickCount() - start < pdMS_TO_TICKS(180000)) {
                if (!WifiManager::GetInstance().IsConnected()) {
                    service_verified.store(false);
                }
                if (online_double_click.load() && service_verified.load() && Available())
                    break;
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if ((!online_double_click.load() || !service_verified.load()) && !stopping.load()) {
                // Stop reserving the tutorial experience after the initial
                // wait, but keep the worker available for a later connection.
                active.store(false);
                ESP_LOGI(kTag, "step=connected waiting in background");
                while (!stopping.load()) {
                    if (!WifiManager::GetInstance().IsConnected())
                        service_verified.store(false);
                    if (online_double_click.load() && service_verified.load() && Available()) {
                        active.store(true);
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }
            if (stopping.load())
                break;
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
        if (!Play(Sound(progress.step), progress.step >= 8, progress.step)) {
            if (stopping.load())
                break;
            if (!SaveInterrupted(progress))
                break;
            ESP_LOGW(kTag, "Narration interrupted; saved step=%u attempts=%u",
                     unsigned(progress.step + 1), unsigned(progress.attempts));
            current_step.store(progress.step);
            continue;
        }
        // Offline demonstrations need time for interaction; the online chapter
        // is an introduction and can flow more naturally between sentences.
        WaitFor(counter, before, progress.step >= 8 ? 2500 : 6000, progress.step);
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
    active.store(true);
    worker_running.store(true);
    if (xTaskCreate(Worker, "wzh_onboarding", 4096, nullptr, 1, nullptr) != pdPASS) {
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
        case OnboardingEvent::kServiceReady:
            service_verified.store(true);
            break;
        case OnboardingEvent::kNetworkLost:
            service_verified.store(false);
            connection_probe.store(false);
            break;
    }
}
void PrepareOnboardingDoubleClick() {
#if CONFIG_WZH_SYSTEM_PROMPTS
    const auto step = current_step.load();
    if (worker_running.load() && step == 8 &&
        WifiManager::GetInstance().IsConnected()) {
        connection_probe.store(Application::GetInstance().GetDeviceState() == kDeviceStateIdle);
        online_double_click.store(true);
    }
#endif
}
bool ConsumeOnboardingConnectionProbe() {
    const bool requested = connection_probe.exchange(false);
    return requested && worker_running.load() && current_step.load() == 8;
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
bool IsOnboardingVoiceMotionActive() { return voice_motion_active.load(); }
bool OnboardingBlocksConversation() {
    return worker_running.load() && current_step.load() < 8;
}
void StopOnboarding() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    restart_after_stop.store(false);
    voice_motion_active.store(false);
    online_double_click.store(false);
    connection_probe.store(false);
    stopping.store(true);
}

void ResumeOnboardingAfterAbort() {
#if CONFIG_WZH_SYSTEM_PROMPTS
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        if (!stopping.load())
            return;
        service_verified.store(false);
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
    service_verified.store(false);
#endif
}

}  // namespace wzh
