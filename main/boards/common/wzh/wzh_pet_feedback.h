#ifndef WZH_PET_FEEDBACK_H_
#define WZH_PET_FEEDBACK_H_

#include <cJSON.h>
#include "wzh_pet_behavior.h"
#include "wzh_protection.h"

namespace wzh {
inline const char* PetModeName(PetMode mode) {
    switch (mode) {
        case PetMode::kQuiet:
            return "quiet";
        case PetMode::kRest:
            return "rest";
        case PetMode::kStopped:
            return "stopped";
        default:
            return "normal";
    }
}
inline const char* MotionUnavailableReason(uint32_t faults) {
    for (const auto fault : {kShutdown, kMaintenance, kFalling, kOverheat, kLowBattery,
                             kBatteryPending, kPetPaused, kMotionTimeout}) {
        if ((faults & fault) != 0)
            return MotionFaultName(fault);
    }
    return (faults & 16U) != 0 ? "power_cooldown" : "busy_or_cooldown";
}
// accepted describes dispatch only; this hardware has no physical completion feedback.
inline cJSON* PetStatus(PetMode mode, uint32_t faults, bool powered, bool busy, bool conversation,
                        int accepted = -1) {
    cJSON* result = cJSON_CreateObject();
    if (result == nullptr)
        return nullptr;
    cJSON_AddStringToObject(result, "mode", PetModeName(mode));
    cJSON_AddNumberToObject(result, "motion_faults", faults);
    cJSON_AddBoolToObject(result, "servo_powered", powered);
    cJSON_AddBoolToObject(result, "motion_busy", busy);
    cJSON_AddBoolToObject(result, "conversation", conversation);
    if (accepted >= 0) {
        cJSON_AddBoolToObject(result, "accepted", accepted != 0);
        cJSON_AddStringToObject(result, "result",
                                accepted ? "request_accepted" : "request_rejected");
        cJSON_AddStringToObject(result, "reason", accepted ? "" : MotionUnavailableReason(faults));
        cJSON_AddBoolToObject(result, "physical_completion_verified", false);
    } else if (faults != 0) {
        cJSON_AddStringToObject(result, "reason", MotionUnavailableReason(faults));
    }
    return result;
}
}  // namespace wzh
#endif
