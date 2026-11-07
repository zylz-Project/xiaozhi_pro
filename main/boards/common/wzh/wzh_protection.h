#ifndef WZH_PROTECTION_H_
#define WZH_PROTECTION_H_

#include <cstdint>

namespace wzh {

enum MotionFault : uint32_t {
    kLowBattery = 1 << 0,
    kOverheat = 1 << 1,
    kFalling = 1 << 2,
    kShutdown = 1 << 3,
    kBatteryPending = 1 << 5,
    kPetPaused = 1 << 6,
    kMaintenance = 1 << 7,
    kMotionTimeout = 1 << 8,
};

inline const char* MotionFaultName(MotionFault fault) {
    switch (fault) {
        case kLowBattery:
            return "low_battery";
        case kOverheat:
            return "chip_overheat";
        case kFalling:
            return "free_fall";
        case kShutdown:
            return "shutdown";
        case kBatteryPending:
            return "battery_initializing";
        case kPetPaused:
            return "stopped_or_resting";
        case kMaintenance:
            return "maintenance";
        case kMotionTimeout:
            return "motion_watchdog";
        default:
            return "unknown";
    }
}

// Use raw, valid battery samples for protection, rather than the slow UI filter.
class BatteryProtection {
public:
    // Defaults match the original protection policy. Boards can pass their own
    // demo thresholds from config.h.
    void Update(int millivolts, int low_mv = 3300, int recover_mv = 3400, int critical_mv = 3000,
                bool allow_recovery = true) {
        valid_count_ = Next(valid_count_);
        low_count_ = millivolts <= low_mv ? Next(low_count_) : 0;
        critical_count_ = millivolts <= critical_mv ? Next(critical_count_) : 0;
        recovery_count_ = allow_recovery && millivolts >= recover_mv ? Next(recovery_count_) : 0;
        if (low_count_ >= 3) {
            low_ = true;
        } else if (recovery_count_ >= 3) {
            low_ = false;
        }
        critical_ = critical_count_ >= 3;
    }
    void Invalidate() {
        low_count_ = critical_count_ = recovery_count_ = valid_count_ = 0;
        critical_ = false;
    }
    bool ready() const { return valid_count_ >= 3; }
    bool low() const { return low_; }
    bool critical() const { return critical_; }

private:
    static int Next(int count) { return count < 3 ? count + 1 : count; }
    int valid_count_ = 0;
    int low_count_ = 0;
    int critical_count_ = 0;
    int recovery_count_ = 0;
    bool low_ = false;
    bool critical_ = false;
};

// A maximum powered interval is a backstop, not a measured stall detector.
class ServoPowerBudget {
public:
    bool Allow(uint32_t now) {
        if (cooling_) {
            if (now - cooldown_since_ < 5000) {
                return false;
            }
            cooling_ = false;
        }
        if (powered_ && now - powered_since_ >= 60000) {
            powered_ = false;
            cooling_ = true;
            cooldown_since_ = now;
            return false;
        }
        return true;
    }
    void PowerOn(uint32_t now) {
        if (!powered_) {
            powered_ = true;
            powered_since_ = now;
        }
    }
    void Release() { powered_ = false; }

private:
    bool powered_ = false;
    bool cooling_ = false;
    uint32_t powered_since_ = 0;
    uint32_t cooldown_since_ = 0;
};

}  // namespace wzh
#endif  // WZH_PROTECTION_H_
