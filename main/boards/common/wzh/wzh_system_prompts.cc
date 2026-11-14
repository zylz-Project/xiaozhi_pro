#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include "wzh_idle_power.h"
#include "wzh_onboarding.h"
#if CONFIG_WZH_SYSTEM_PROMPTS
#include "wzh_onboarding_store.h"
#endif
#include "wzh_prompt_audio_gate.h"
#include "wzh_system_prompts.h"

#if CONFIG_WZH_SYSTEM_PROMPTS || CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || \
    CONFIG_BOARD_TYPE_WZH_DINOSAUR || CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
#include <atomic>
#include <mutex>
#include <optional>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"

namespace wzh {
namespace {
constexpr const char* kTag = "SystemPrompts";
std::mutex gate_mutex;
LowBatteryPromptGate battery_gate;
std::atomic<bool> worker_started{false};
std::atomic<bool> startup_audio_started{false};
std::atomic<bool> power_on_prompt_queued{false};
std::atomic<bool> startup_onboarding_pending{false};
std::atomic<bool> onboarding_decided{false};
std::atomic<bool> startup_network_ready{false};
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
std::atomic<bool> startup_greeting_complete{false};
#else
std::atomic<bool> startup_greeting_complete{true};
#endif
std::atomic<bool> shutting_down{false};
std::atomic<StartupProtectionNotice> startup_protection{StartupProtectionNotice::kNone};
#if CONFIG_WZH_SYSTEM_PROMPTS
std::atomic<StartupProtectionNotice> startup_protection_pending{StartupProtectionNotice::kNone};
#endif
#if CONFIG_WZH_SYSTEM_PROMPTS
std::atomic<bool> wifi_setup_requested{false};
std::atomic<bool> wifi_setup_producing{false};
std::atomic<uint32_t> wifi_setup_requested_ms{0};
#endif
std::atomic<uint32_t> shutdown_serial{0};
std::atomic<uint32_t> shutdown_ready{0};
std::atomic<uint32_t> shutdown_requested{0};
std::atomic<uint32_t> shutdown_produced{0};
std::atomic<uint32_t> shutdown_generation{0};

uint32_t QueueShutdownPrompt() {
    const auto request = shutdown_serial.fetch_add(1) + 1;
    // Serialize after StopInteraction's queued cleanup, rather than assuming
    // that the servo parking delay also finished application work.
    Application::GetInstance().Schedule([request]() {
        if (request != shutdown_serial.load())
            return;
        auto& audio = Application::GetInstance().GetAudioService();
        audio.EnableWakeWordDetection(false);
        audio.EnableVoiceProcessing(false);
        audio.ResetDecoder();
        shutdown_generation.store(audio.GetPlaybackGeneration());
        shutdown_ready.store(request);
        shutdown_requested.store(request);
    });
    return request;
}

void Worker(void*) {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
#if CONFIG_WZH_SYSTEM_PROMPTS
    std::optional<uint32_t> low_generation;
    uint32_t low_drained_before = 0;
#endif
    while (true) {
#if CONFIG_WZH_SYSTEM_PROMPTS
        if (low_generation) {
            if (*low_generation != audio.GetPlaybackGeneration()) {
                // A generation change alone cannot tell "finished" from
                // "interrupted". Only re-arm when no natural drain was observed
                // since enqueue; otherwise a completed reminder would repeat.
                const bool completed = audio.GetPlaybackDrainedCount() != low_drained_before;
                if (!completed) {
                    std::lock_guard<std::mutex> lock(gate_mutex);
                    battery_gate.RetryAfterInterruption();
                }
                low_generation.reset();
            } else if (audio.IsPlaybackIdle()) {
                low_generation.reset();
            }
        }
#endif
        if (const auto request = shutdown_requested.exchange(0)) {
            const auto generation = shutdown_generation.load();
            if (request == shutdown_serial.load()) {
                ESP_LOGI(kTag, "Voice request: power_off text=我要休息啦，下次再一起玩吧！");
                audio.PlaySound(Lang::Sounds::OGG_POWER_OFF, generation);
                shutdown_produced.store(request);
            }
        }
#if CONFIG_WZH_SYSTEM_PROMPTS
        else if (!shutting_down.load() &&
                 startup_protection_pending.load() != StartupProtectionNotice::kNone) {
            const auto notice = startup_protection_pending.load();
            std::unique_lock<std::mutex> reservation(PromptAudioGate(), std::try_to_lock);
            if (notice == startup_protection.load() && reservation.owns_lock() &&
                app.GetDeviceState() == kDeviceStateIdle && audio.IsPlaybackIdle() &&
                !audio.IsAudioProcessorRunning() && !Board::GetInstance().IsAudioMaintenanceActive()) {
                auto expected = notice;
                if (startup_protection_pending.compare_exchange_strong(
                        expected, StartupProtectionNotice::kNone)) {
                    if (notice == StartupProtectionNotice::kLowBattery) {
                        std::lock_guard<std::mutex> lock(gate_mutex);
                        battery_gate.Observe(true, true);
                        battery_gate.Take(esp_log_timestamp());
                        low_generation = audio.GetPlaybackGeneration();
                        low_drained_before = audio.GetPlaybackDrainedCount();
                        ESP_LOGI(kTag, "Voice request: startup_low_battery text=我的电量不足啦，请帮我充充电吧！");
                    } else {
                        ESP_LOGI(kTag, "Prompt queued: startup_protection=%d sound=exclamation",
                                 static_cast<int>(notice));
                    }
                    audio.PlaySound(notice == StartupProtectionNotice::kLowBattery
                                        ? Lang::Sounds::OGG_LOW_BATTERY
                                        : Lang::Sounds::OGG_EXCLAMATION,
                                    audio.GetPlaybackGeneration());
                }
            }
        }
        else if (!shutting_down.load() && wifi_setup_requested.load()) {
            if (!onboarding_decided.load()) {
                const auto elapsed = esp_log_timestamp() - wifi_setup_requested_ms.load();
                if (elapsed >= 45000)
                    wifi_setup_requested.store(false);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (IsOnboardingActive()) {
                wifi_setup_requested.store(false);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            const auto now = esp_log_timestamp();
            const auto elapsed = now - wifi_setup_requested_ms.load();
            // Startup feedback owns audio first. Bound the wait even when the
            // controller, hardware or greeting audio cannot become ready.
            if (!startup_greeting_complete.load() && elapsed < 30000) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (elapsed >= 45000 || app.GetDeviceState() != kDeviceStateIdle) {
                wifi_setup_requested.store(false);
            } else if (elapsed >= 500 && audio.IsPlaybackIdle() &&
                       !audio.IsAudioProcessorRunning() &&
                       !Board::GetInstance().IsAudioMaintenanceActive()) {
                // Reserve this producer until the setup prompt is enqueued.
                wifi_setup_producing.store(true);
                wifi_setup_requested.store(false);
                ESP_LOGI(kTag, "Playing first Wi-Fi setup prompt");
                ESP_LOGI(kTag, "Voice request: wificonfig text=请用手机帮我配网。");
                audio.PlaySound(Lang::Sounds::OGG_WIFICONFIG, audio.GetPlaybackGeneration());
                wifi_setup_producing.store(false);
            }
        } else if (!shutting_down.load() && !low_generation) {
            // Capture BEFORE the state/idle checks: entering conversation in
            // between invalidates this generation and cancels the old prompt.
            const auto generation = audio.GetPlaybackGeneration();
            std::unique_lock<std::mutex> reservation(PromptAudioGate(), std::try_to_lock);
            if (reservation.owns_lock() && app.GetDeviceState() == kDeviceStateIdle &&
                audio.IsPlaybackIdle() && !audio.IsAudioProcessorRunning() &&
                !Board::GetInstance().IsAudioMaintenanceActive()) {
                bool play = false;
                {
                    std::lock_guard<std::mutex> lock(gate_mutex);
                    play = !shutting_down.load() && battery_gate.Take(esp_log_timestamp());
                }
                if (play) {
                    low_generation = generation;
                    low_drained_before = audio.GetPlaybackDrainedCount();
                    ESP_LOGI(kTag, "Voice request: low_battery text=我的电量不足啦，请帮我充充电吧！");
                    audio.PlaySound(Lang::Sounds::OGG_LOW_BATTERY, generation);
                }
            }
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}  // namespace

void StartSystemPrompts() {
    // Called after AudioService startup.
    if (!worker_started.load()) {
#if CONFIG_WZH_SYSTEM_PROMPTS
        OnboardingProgress progress;
        startup_onboarding_pending.store(!PeekOnboardingProgress(progress) || !progress.Done());
#endif
        const bool started =
            xTaskCreate(Worker, "system_prompts", 4096, nullptr, 1, nullptr) == pdPASS;
        worker_started.store(started);
        if (!started)
            ESP_LOGE(kTag, "Cannot start prompt worker; safety and shutdown remain available");
        else if (startup_greeting_complete.load() && power_on_prompt_queued.load() &&
                 !onboarding_decided.exchange(true)) {
            StartOnboarding();
        }
    }
}

void StartProductAudio() {
    if (shutting_down.load() || startup_audio_started.exchange(true))
        return;
    ESP_LOGI(kTag, "Power-on sound requested as soon as audio is ready");
    Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POWER_ON);
    StartSystemPrompts();
    MarkPowerOnPromptQueued();
}

void RequestWifiSetupPrompt() {
#if CONFIG_WZH_SYSTEM_PROMPTS
    wifi_setup_requested_ms.store(esp_log_timestamp());
    wifi_setup_requested.store(true);
#endif
}

void MarkStartupNetworkReady() { startup_network_ready.store(true); }

bool StartupGreetingAudioReady() {
    return worker_started.load() && power_on_prompt_queued.load() && !shutting_down.load();
}

bool StartupGreetingFinished() { return startup_greeting_complete.load(); }

void MarkPowerOnPromptQueued() {
    power_on_prompt_queued.store(true);
    if (startup_greeting_complete.load() && worker_started.load() &&
        !onboarding_decided.exchange(true))
        StartOnboarding();
}

bool StartupOnboardingPending() { return startup_onboarding_pending.load(); }

void MarkStartupGreetingComplete() {
    startup_greeting_complete.store(true);
    if (worker_started.load() && power_on_prompt_queued.load() &&
        !onboarding_decided.exchange(true))
        StartOnboarding();
}

void ObserveStartupProtection(StartupProtectionNotice notice) {
    if (startup_protection.exchange(notice) == notice)
        return;
#if CONFIG_WZH_SYSTEM_PROMPTS
    startup_protection_pending.store(notice);
#endif
}

const char* StartupProtectionMessage() {
    switch (startup_protection.load()) {
        case StartupProtectionNotice::kLowBattery:
            return "电池电量不足，暂时无法活动，请先充电。";
        case StartupProtectionNotice::kOverheat:
            return "设备温度过高，暂时无法活动，请等待冷却。";
        case StartupProtectionNotice::kMotionBlocked:
            return "动作暂时受保护，请将设备平稳放置后重试。";
        case StartupProtectionNotice::kCheckingBattery:
            return "正在检查电池状态，动作暂未启动，请稍候。";
        default:
            return "";
    }
}

void ObserveSystemPromptBattery(bool ready, bool low) {
    std::lock_guard<std::mutex> lock(gate_mutex);
    battery_gate.Observe(ready, low);
}

void BeginSystemPromptShutdown() {
    shutting_down.store(true);
    StopOnboarding();
    StopIdlePower();
}

bool PlayShutdownPromptAndWait(const std::function<void()>& check_battery) {
    if (!worker_started.load())
        return false;
    auto& audio = Application::GetInstance().GetAudioService();
    auto request = QueueShutdownPrompt();
    unsigned retries = 0;
    const uint32_t started = esp_log_timestamp();
    uint32_t last_battery_check = started;
    // Production and drain share one bounded deadline, including any retry.
    // The worker, not this battery/power task, can wait on the decode queue.
    while (esp_log_timestamp() - started < 8000) {
        const auto now = esp_log_timestamp();
        if (now - last_battery_check >= 1000) {
            last_battery_check = now;
            check_battery();
        }
        if (shutdown_ready.load() == request &&
            audio.GetPlaybackGeneration() != shutdown_generation.load()) {
            // Cancellation also empties queues and can advance the global drain
            // count. Neither proves that THIS shutdown sentence reached its end.
            if (retries++ != 0)
                break;
            ESP_LOGW(kTag, "Shutdown speech interrupted; retrying once");
            request = QueueShutdownPrompt();
        } else if (shutdown_produced.load() == request && audio.IsPlaybackIdle()) {
            vTaskDelay(pdMS_TO_TICKS(80));  // Drain the physical codec/DMA tail.
            if (audio.GetPlaybackGeneration() == shutdown_generation.load() &&
                audio.IsPlaybackIdle()) {
                ESP_LOGI(kTag, "Shutdown speech completed (%u retries)", retries);
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // Invalidate an unscheduled application callback AND a waiting producer.
    shutdown_serial.fetch_add(1);
    shutdown_requested.store(0);
    audio.ResetDecoder();
    ESP_LOGW(kTag, "Shutdown speech incomplete or timed out; releasing power");
    return false;
}
}  // namespace wzh
#endif  // WZH boards
