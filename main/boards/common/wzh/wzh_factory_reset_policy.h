#ifndef WZH_FACTORY_RESET_POLICY_H_
#define WZH_FACTORY_RESET_POLICY_H_

#include <functional>
#include <string>

namespace wzh {
// Executed by a single background reset task. A callback reports success only
// after its data is durably persisted. Wi-Fi is cleared last.
class FactoryResetPolicy {
public:
    enum class Phase { kChecking, kDeletingVoices, kClearingWifi, kDone, kFailed };
    struct Progress {
        Phase phase = Phase::kChecking;
        std::string error;
    };
    struct Operations {
        std::function<bool()> validate;
        std::function<bool()> reset_voices;
        std::function<bool()> clear_wifi;
        std::function<void(const Progress&)> report;
    };
    static bool Run(Operations& operations) {
        Progress progress;
        const auto update = [&] {
            if (operations.report)
                operations.report(progress);
        };
        const auto fail = [&](const char* reason) {
            progress.phase = Phase::kFailed;
            progress.error = reason;
            update();
            return false;
        };
        update();
        if (!operations.validate || !operations.validate())
            return fail("RESET_NOT_ALLOWED");
        progress.phase = Phase::kDeletingVoices;
        update();
        if (!operations.reset_voices || !operations.reset_voices())
            return fail("RESET_VOICE_FAILED");
        progress.phase = Phase::kClearingWifi;
        update();
        if (!operations.clear_wifi || !operations.clear_wifi())
            return fail("RESET_WIFI_FAILED");
        progress.phase = Phase::kDone;
        update();
        return true;
    }
};
}  // namespace wzh
#endif
