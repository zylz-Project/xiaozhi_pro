#ifndef WZH_ONBOARDING_STORE_H_
#define WZH_ONBOARDING_STORE_H_

#include "wzh_onboarding_progress.h"

namespace wzh {

// A single versioned NVS record keeps progress coherent across power loss.
// Old installations with wzh_web/setup_guide=1 migrate as already complete.
bool LoadOnboardingProgress(OnboardingProgress& progress);
// Read-only variant for HTTP handlers and other polling callers: never migrates,
// never writes NVS. A missing record simply reports "nothing done".
bool PeekOnboardingProgress(OnboardingProgress& progress);
bool SaveOnboardingProgress(const OnboardingProgress& progress);

}  // namespace wzh
#endif  // WZH_ONBOARDING_STORE_H_
