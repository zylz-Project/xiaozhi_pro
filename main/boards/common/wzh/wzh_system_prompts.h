#ifndef WZH_SYSTEM_PROMPTS_H_
#define WZH_SYSTEM_PROMPTS_H_

#include <cstdint>
#include <functional>

namespace wzh {

// Remind at most once per 10 minutes while low voltage persists. Invalid ADC
// samples suppress playback without clearing the current episode.
class LowBatteryPromptGate {
public:
    void Observe(bool ready, bool low) {
        ready_low_ = ready && low;
        if (ready && !low)
            announced_ = false;
    }
    static constexpr uint32_t kRepeatIntervalMs = 10U * 60U * 1000U;
    bool Take(uint32_t now_ms) {
        if (!ready_low_ ||
            (announced_ && now_ms - last_announcement_ms_ < kRepeatIntervalMs))
            return false;
        announced_ = true;
        last_announcement_ms_ = now_ms;
        return true;
    }
    void RetryAfterInterruption() {
        if (ready_low_)
            announced_ = false;
    }

private:
    bool ready_low_ = false;
    bool announced_ = false;
    uint32_t last_announcement_ms_ = 0;
};

// Calls from the power task never enqueue/decode audio. A single worker owns
// prompt production; the application/audio tasks keep their normal priorities.
void StartSystemPrompts();
// Call once when the audio service starts; battery and motion protection run independently.
void StartProductAudio();
// First provisioning prompt waits for startup audio and initialization cleanup.
void RequestWifiSetupPrompt();
// Wait until network startup has decided whether to request first-time setup.
void MarkStartupNetworkReady();
// Startup gesture waits until the power-on prompt has been queued.
bool StartupGreetingAudioReady();
bool StartupGreetingFinished();
void MarkPowerOnPromptQueued();
bool StartupOnboardingPending();
// Release the first setup prompt after the one-shot greeting has finished.
void MarkStartupGreetingComplete();
enum class StartupProtectionNotice : uint8_t { kNone, kLowBattery, kOverheat, kMotionBlocked, kCheckingBattery };
void ObserveStartupProtection(StartupProtectionNotice notice);
const char* StartupProtectionMessage();
void ObserveSystemPromptBattery(bool ready, bool low);
void BeginSystemPromptShutdown();
// Power task only; bounded wait includes production AND final PCM drain. The
// callback maintains battery protection while the power task waits for speech.
bool PlayShutdownPromptAndWait(const std::function<void()>& check_battery);

}  // namespace wzh
#endif  // WZH_SYSTEM_PROMPTS_H_
