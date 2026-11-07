#ifndef WZH_SYSTEM_PROMPTS_H_
#define WZH_SYSTEM_PROMPTS_H_

#include <functional>

namespace wzh {

// Once per confirmed low-battery episode. Invalid ADC samples suppress playback
// but do not re-arm it; only stable protection recovery starts a new episode.
class LowBatteryPromptGate {
public:
    void Observe(bool ready, bool low) {
        ready_low_ = ready && low;
        if (ready && !low)
            announced_ = false;
    }
    bool Take() {
        if (!ready_low_ || announced_)
            return false;
        announced_ = true;
        return true;
    }
    void RetryAfterInterruption() {
        if (ready_low_)
            announced_ = false;
    }

private:
    bool ready_low_ = false;
    bool announced_ = false;
};

// Calls from the power task never enqueue/decode audio. A single worker owns
// prompt production; the application/audio tasks keep their normal priorities.
void StartSystemPrompts();
void ObserveSystemPromptBattery(bool ready, bool low);
void BeginSystemPromptShutdown();
// Power task only; bounded wait includes production AND final PCM drain. The
// callback maintains battery protection while the power task waits for speech.
bool PlayShutdownPromptAndWait(const std::function<void()>& check_battery);

}  // namespace wzh
#endif  // WZH_SYSTEM_PROMPTS_H_
