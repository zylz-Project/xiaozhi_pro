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
    kNetworkLost
};

// These functions only update atomics or start a worker. Never decode audio or
// persist NVS on a sensor, power, audio, network, or application callback.
void StartOnboarding();
void NotifyOnboarding(OnboardingEvent event);
// Reserve the tutorial worker for the first conversation entry. Resume on the
// application task with the result once playback finishes or is cancelled.
bool DeferOnboardingConversation(std::function<void(bool)> resume);
bool IsOnboardingIntroPending();
bool IsOnboardingConversationIntroActive();
// Consume the matching physical interaction while a tutorial window owns it.
bool CaptureOnboardingInteraction(OnboardingEvent event);
// Called from the portal HTTP handler when a client fetches a page or asset.
// The tutorial uses it to know the phone actually reached the setup page.
void NotifyPortalRequest();
// True while the tutorial reserves motion and audio for narration or feedback.
bool IsOnboardingActive();
// Also true while the worker waits in the background or finishes saving.
bool IsOnboardingWorkerRunning();
// True only while an online tutorial sentence owns the audio output.
bool IsOnboardingVoiceMotionActive();
// The first double click is accepted after the offline chapter.
bool OnboardingBlocksConversation();
void StopOnboarding();
// Undoes a StopOnboarding() that did not end in a reboot (for example a factory
// reset that failed at the Wi-Fi step). No-op while a worker is still running.
void ResumeOnboardingAfterAbort();

}  // namespace wzh
#endif  // WZH_ONBOARDING_H_
