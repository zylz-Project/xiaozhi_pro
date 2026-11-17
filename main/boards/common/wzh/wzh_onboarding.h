#ifndef WZH_ONBOARDING_H_
#define WZH_ONBOARDING_H_

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
// A double-click acknowledges the online chapter without entering conversation.
void PrepareOnboardingDoubleClick();
// Consume the matching physical interaction while a tutorial window owns it.
bool CaptureOnboardingInteraction(OnboardingEvent event);
bool IsOnboardingActive();
// True only while an online tutorial sentence owns the audio output.
bool IsOnboardingVoiceMotionActive();
// Conversation stays blocked until the complete tutorial is finished.
bool OnboardingBlocksConversation();
void StopOnboarding();
// Undoes a StopOnboarding() that did not end in a reboot (for example a factory
// reset that failed at the Wi-Fi step). No-op while a worker is still running.
void ResumeOnboardingAfterAbort();

}  // namespace wzh
#endif  // WZH_ONBOARDING_H_
