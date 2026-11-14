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
    kNetworkLost
};

// These functions only update atomics or start a worker. Never decode audio or
// persist NVS on a sensor, power, audio, network, or application callback.
void StartOnboarding();
void NotifyOnboarding(OnboardingEvent event);
// A double-click can verify the service and resume the first online chapter.
void PrepareOnboardingDoubleClick();
bool ConsumeOnboardingConnectionProbe();
// Consume the matching physical interaction while a tutorial window owns it.
bool CaptureOnboardingInteraction(OnboardingEvent event);
// Called from the portal HTTP handler when a client fetches a page or asset.
// The tutorial uses it to know the phone actually reached the setup page.
void NotifyPortalRequest();
bool IsOnboardingActive();
// True only while an online tutorial sentence owns the audio output.
bool IsOnboardingVoiceMotionActive();
// Once the offline chapter has ended, conversation must be available to
// establish the service connection required by the online chapter.
bool OnboardingBlocksConversation();
void StopOnboarding();
// Undoes a StopOnboarding() that did not end in a reboot (for example a factory
// reset that failed at the Wi-Fi step). No-op while a worker is still running.
void ResumeOnboardingAfterAbort();

}  // namespace wzh
#endif  // WZH_ONBOARDING_H_
