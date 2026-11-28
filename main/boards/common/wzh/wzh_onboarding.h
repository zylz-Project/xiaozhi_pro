#ifndef WZH_ONBOARDING_H_
#define WZH_ONBOARDING_H_

#include <functional>

namespace wzh {

enum class OnboardingEvent {
    kTouch,
    kMotion,
    // The offline wake phrase was spoken: proof that "叫我就行" works.
    kWake,
    kCommand,
    kServiceReady,
    kServiceConfigured,
    kServiceLost,
    kNetworkConnected,
    kNetworkLost,
    kBleConnected,
    kBleDisconnected,
    kWifiRequest
};

// These functions only update atomics or start a worker. Never decode audio or
// persist NVS on a sensor, power, audio, network, or application callback.
void StartOnboarding();
// Retries only when a suspended step's required event has arrived.
void TryResumeOnboarding();
void NotifyOnboarding(OnboardingEvent event);
// During the offline chapter, the double-click can acknowledge the wake demo.
void PrepareOnboardingDoubleClick();
// Consume the matching physical interaction while a tutorial window owns it.
bool CaptureOnboardingInteraction(OnboardingEvent event);
bool IsOnboardingActive();
bool IsOnboardingWorkerRunning();
bool IsOnboardingIntroPending();
// Run the pending online prompts before opening the first conversation.
bool DeferOnboardingConversation(std::function<void(bool)> resume);
// True only while an online tutorial sentence owns the audio output.
bool IsOnboardingVoiceMotionActive();
// The offline chapter blocks conversation until the initial setup is complete.
bool OnboardingBlocksConversation();
void StopOnboarding();
// Undoes a StopOnboarding() that did not end in a reboot (for example a factory
// reset that failed at the Wi-Fi step). No-op while a worker is still running.
void ResumeOnboardingAfterAbort();

}  // namespace wzh
#endif  // WZH_ONBOARDING_H_
