#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include "wzh_idle_power.h"
#include "wzh_onboarding.h"
#include "wzh_onboarding_transcript.h"
#include "wzh_prompt_audio_gate.h"

#if CONFIG_WZH_IDLE_POWER_ENABLE &&                                        \
    (CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
     CONFIG_BOARD_TYPE_WZH_TAILREDPANDA)
#include <atomic>
#include <functional>
#include <mutex>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"

namespace wzh {
namespace {
constexpr const char* kTag = "IdlePower";
constexpr uint32_t kTickMs = 100;
constexpr uint32_t kAcquireDeadlineMs = 2000;
constexpr uint32_t kSpeechDeadlineMs = 7000;

std::atomic<bool> started{false};
std::atomic<bool> stopping{false};
std::atomic<bool> suppressed{false};
std::atomic<bool> shutdown_issued{false};
// Zero means "no pending reset"; the worker adopts the first sample as its base.
std::atomic<uint32_t> pending_interaction{0};
IdlePower timeline;
std::function<void()> request_shutdown;
std::mutex lifecycle_mutex;

enum class Gain { kRetry, kReserved };

// The worker's own state lives here rather than on its stack: the state machine
// must survive every yield point of the tick loop.
// Shutdown() owns its own wait for the goodbye sentence, so the countdown
// only needs "armed" and "waiting for the miss sentence to drain".
enum class Phase { kIdle, kWaitingMiss };
Phase phase = Phase::kIdle;
uint32_t acquire_start = 0, speech_start = 0, speech_generation = 0;
bool retry_armed = false;

uint32_t Now() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool Available() {
    return !stopping.load() && Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
           !Board::GetInstance().IsAudioMaintenanceActive();
}

// Non-blocking by design: the countdown must keep running even while audio is
// busy, so a single tick only decides "can I speak right now?".
[[maybe_unused]] Gain TrySpeak(const std::string_view& sound, uint32_t& generation) {
    auto& audio = Application::GetInstance().GetAudioService();
    std::unique_lock<std::mutex> reservation(PromptAudioGate(), std::try_to_lock);
    if (!reservation.owns_lock() || !Available() || !audio.IsPlaybackIdle() ||
        audio.IsAudioProcessorRunning() || Board::GetInstance().IsLocalPlaybackPending())
        return Gain::kRetry;
    generation = audio.GetPlaybackGeneration();
    ESP_LOGI(kTag, "Voice request: idle_miss text=%s", OnboardingTranscript(13));
    audio.PlaySound(sound, generation);
    return Gain::kReserved;
}

void Worker(void*) {
    ESP_LOGI(kTag, "Idle countdown armed (%u min greeting / %u min auto-off)",
             unsigned(IdlePower::kMissMs / 60000U), unsigned(IdlePower::kOffMs / 60000U));
    while (!stopping.load()) {
        if (const auto stamp = pending_interaction.exchange(0)) {
            timeline.Interaction(stamp);
            suppressed.store(false);
            // A fresh interaction re-arms the whole timeline, including auto-off
            // and the bounded retry window for the next miss sentence.
            shutdown_issued.store(false);
            phase = Phase::kIdle;
            retry_armed = false;
            acquire_start = 0;
        }
        const uint32_t now = Now();
        // The tutorial, an ongoing conversation and a maintenance operation all
        // mean someone is using the device; none of them counts as silence.
        if (IsOnboardingActive() || !Available()) {
            timeline.Interaction(now);
            phase = Phase::kIdle;
            retry_armed = false;
            vTaskDelay(pdMS_TO_TICKS(kTickMs));
            continue;
        }

        if (phase == Phase::kWaitingMiss) {
            auto& audio = Application::GetInstance().GetAudioService();
            const bool cancelled =
                !Available() || audio.GetPlaybackGeneration() != speech_generation;
            if (cancelled || audio.IsPlaybackIdle()) {
                if (cancelled)
                    ESP_LOGW(kTag, "Idle-miss prompt interrupted; quiet mode still starts");
                else
                    ESP_LOGI(kTag, "Idle-miss prompt finished; idle sounds are now silent");
                suppressed.store(true);
                phase = Phase::kIdle;
                continue;
            }
            if (now - speech_start >= kSpeechDeadlineMs) {
                ESP_LOGW(kTag, "Idle-miss prompt timed out; quiet mode still starts");
                suppressed.store(true);
                phase = Phase::kIdle;
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(kTickMs));
            continue;
        }

        if (timeline.MissDue(now)) {
            const auto token = timeline.BeginMiss();
            // The sentence only exists when the product voice pack is compiled in;
            // without it gen_lang.py omits the symbol entirely.
#if CONFIG_WZH_IDLE_MISS_PROMPT && CONFIG_WZH_SYSTEM_PROMPTS
            if (TrySpeak(Lang::Sounds::OGG_IDLE_MISS, speech_generation) == Gain::kReserved) {
                // Mark the episode handled as soon as it is enqueued; the retry
                // window must not fire a second sentence behind the first.
                timeline.FinishMiss(token, true);
                retry_armed = false;
                speech_start = now;
                phase = Phase::kWaitingMiss;
                vTaskDelay(pdMS_TO_TICKS(kTickMs));
                continue;
            }
            if (!retry_armed) {
                // First tick of this episode: start the bounded wait for audio.
                retry_armed = true;
                acquire_start = now;
            }
            if (now - acquire_start < kAcquireDeadlineMs) {
                timeline.FinishMiss(token, false);
                vTaskDelay(pdMS_TO_TICKS(kTickMs));
                continue;
            }
#else
            (void)acquire_start;
            (void)retry_armed;
            ESP_LOGI(kTag, "Idle-miss prompt disabled; silencing idle sounds");
#endif
            // Quiet starts at the miss mark either way: motion stays at full
            // amplitude, but a pet that keeps talking to an empty room reads as broken.
            timeline.FinishMiss(token, true);
            suppressed.store(true);
            // The next episode gets its own retry window; leaving this set would
            // make every later attempt give up on its first tick.
            retry_armed = false;
            acquire_start = 0;
            vTaskDelay(pdMS_TO_TICKS(kTickMs));
            continue;
        }

        if (timeline.OffDue(now) && !shutdown_issued.load()) {
            suppressed.store(true);
            shutdown_issued.store(true);
#if CONFIG_WZH_IDLE_AUTO_OFF
            ESP_LOGI(kTag, "No interaction for %u min: requesting shutdown",
                     unsigned(IdlePower::kOffMs / 60000U));
            // Shutdown() owns the goodbye sentence; playing it here as well would
            // say it twice. Keep the loop alive: a touch or a power-key press in
            // the seconds before the latch drops is a real interaction.
            if (request_shutdown)
                request_shutdown();
#else
            // Auto-off disabled: stay silent-but-animated instead of shuttering
            // the pet; only an explicit power press may turn it off.
            ESP_LOGI(kTag, "No interaction for %u min; auto-off is disabled",
                     unsigned(IdlePower::kOffMs / 60000U));
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
    started.store(false);
    vTaskDelete(nullptr);
}
}  // namespace

void NotifyIdleInteraction() {
    if (!started.load() || stopping.load())
        return;
    pending_interaction.store(Now());
    suppressed.store(false);
}

bool IdleSoundSuppressed() { return suppressed.load(); }

void StartIdlePower(std::function<void()> shutdown_request) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    if (started.load() || stopping.load())
        return;
    request_shutdown = std::move(shutdown_request);
    // Anchor the timeline at power-on: silence is measured from boot, not from
    // the first task tick. The phase state is owned here too, so the worker
    // loop can yield without losing progress.
    timeline.Interaction(Now());
    phase = Phase::kIdle;
    acquire_start = speech_start = speech_generation = 0;
    retry_armed = false;
    started.store(true);
    if (xTaskCreate(Worker, "wzh_idle_power", 4096, nullptr, 1, nullptr) != pdPASS) {
        started.store(false);
        ESP_LOGE(kTag, "Cannot start idle countdown worker");
    }
}

void StopIdlePower() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    stopping.store(true);
}
}  // namespace wzh

#else  // switch disabled or unrelated board: everything is a no-op

namespace wzh {
void NotifyIdleInteraction() {}
bool IdleSoundSuppressed() { return false; }
void StartIdlePower(std::function<void()>) {}
void StopIdlePower() {}
}  // namespace wzh

#endif
