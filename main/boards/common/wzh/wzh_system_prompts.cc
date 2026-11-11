#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

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
std::atomic<bool> startup_network_ready{false};
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
std::atomic<bool> startup_greeting_complete{false};
#else
std::atomic<bool> startup_greeting_complete{true};
#endif
std::atomic<bool> shutting_down{false};
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
                audio.PlaySound(Lang::Sounds::OGG_POWER_OFF, generation);
                shutdown_produced.store(request);
            }
        }
#if CONFIG_WZH_SYSTEM_PROMPTS
        else if (!shutting_down.load() && wifi_setup_requested.load()) {
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
                audio.PlaySound(Lang::Sounds::OGG_WIFICONFIG, audio.GetPlaybackGeneration());
                wifi_setup_producing.store(false);
            }
        } else if (!shutting_down.load() && !low_generation) {
            // Capture BEFORE the state/idle checks: entering conversation in
            // between invalidates this generation and cancels the old prompt.
            const auto generation = audio.GetPlaybackGeneration();
            if (app.GetDeviceState() == kDeviceStateIdle && audio.IsPlaybackIdle() &&
                !audio.IsAudioProcessorRunning() &&
                !Board::GetInstance().IsAudioMaintenanceActive()) {
                bool play = false;
                {
                    std::lock_guard<std::mutex> lock(gate_mutex);
                    play = !shutting_down.load() && battery_gate.Take(esp_log_timestamp());
                }
                if (play) {
                    low_generation = generation;
                    low_drained_before = audio.GetPlaybackDrainedCount();
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
    // Exactly one board power task calls this once, after AudioService startup.
    if (!worker_started.load()) {
        const bool started =
            xTaskCreate(Worker, "system_prompts", 4096, nullptr, 1, nullptr) == pdPASS;
        worker_started.store(started);
        if (!started)
            ESP_LOGE(kTag, "Cannot start prompt worker; safety and shutdown remain available");
    }
}

void RequestWifiSetupPrompt() {
#if CONFIG_WZH_SYSTEM_PROMPTS
    wifi_setup_requested_ms.store(esp_log_timestamp());
    wifi_setup_requested.store(true);
#endif
}

void MarkStartupNetworkReady() { startup_network_ready.store(true); }

bool StartupGreetingAudioReady() {
    return worker_started.load() && !shutting_down.load();
}

void MarkStartupGreetingComplete() { startup_greeting_complete.store(true); }

void ObserveSystemPromptBattery(bool ready, bool low) {
    std::lock_guard<std::mutex> lock(gate_mutex);
    battery_gate.Observe(ready, low);
}

void BeginSystemPromptShutdown() { shutting_down.store(true); }

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
