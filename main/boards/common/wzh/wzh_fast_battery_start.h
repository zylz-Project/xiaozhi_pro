#ifndef WZH_FAST_BATTERY_START_H_
#define WZH_FAST_BATTERY_START_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "wzh_battery_voltage.h"

namespace wzh {
// An optional early qualification. The ordinary 15 s protection path remains
// the fallback. Bounds are conservative engineering assumptions for a single
// cell and the V4-2.2 divider, not hardware measurements or certification.
class FastBatteryStart {
public:
    static constexpr uint32_t kProbeIntervalMs = 200;
    static constexpr uint32_t kMinBootMs = 2000;
    static constexpr uint32_t kMaxSampleGapMs = 500;
    static constexpr float kMaxTimeConstantMs = 6000.0f;
    static constexpr int kMaxInitialBatteryMv = 4400;
    static constexpr int kAdcErrorMarginMv = 200;
    static constexpr int kMaxSpreadMv = 120;
    static constexpr size_t kSamples = 5;

    bool Observe(uint32_t now_ms, bool valid, int measured_mv, int recover_mv) {
        if (!valid || now_ms < kMinBootMs || measured_mv <= 0 || measured_mv > 4600 ||
            (count_ && now_ms - last_ms_ > kMaxSampleGapMs)) {
            count_ = 0;
            return false;
        }
        if (count_ && now_ms - last_ms_ < kProbeIntervalMs)
            return false;
        // Worst case: the capacitor initially held 4.4 V (battery-referred),
        // the divider settles no faster than tau=6 s, and ADC overreads by
        // 200 mV. Require even that lower bound to reach recovery voltage.
        const float residual = std::exp(-static_cast<float>(now_ms) / kMaxTimeConstantMs);
        const float required =
            recover_mv + kAdcErrorMarginMv + (kMaxInitialBatteryMv - recover_mv) * residual;
        if (measured_mv < required) {
            count_ = 0;
            return false;
        }
        // A caller may have been unable to commit a qualified window (for
        // example the board has not registered its servo output yet).
        if (count_ >= kSamples)
            count_ = 0;
        samples_[count_++] = measured_mv;
        last_ms_ = now_ms;
        if (count_ < kSamples)
            return false;
        const auto bounds = std::minmax_element(samples_.begin(), samples_.end());
        if (*bounds.second - *bounds.first > kMaxSpreadMv) {
            count_ = 0;
            return false;
        }
        return true;
    }

    bool qualified() const { return count_ == kSamples; }
    const std::array<int, kSamples>& samples() const { return samples_; }

private:
    std::array<int, kSamples> samples_{};
    size_t count_ = 0;
    uint32_t last_ms_ = 0;
};

inline bool CommitFastBatteryStart(const FastBatteryStart& startup,
                                   StableBatteryProtection& protection, bool rested, int low_mv,
                                   int recover_mv, int critical_mv) {
    if (!startup.qualified())
        return false;
    for (const int value : startup.samples())
        protection.Update(value, rested, low_mv, recover_mv, critical_mv);
    return protection.ready() && !protection.low() && !protection.critical();
}

// The startup probe is observation only: it does not update low/critical
// protection and cannot power a servo. Require real calibration to qualify.
template <typename Reader, typename Calibrator>
bool ReadFastBatteryProbe(Reader&& read, Calibrator&& calibrate, int& battery_mv) {
    std::array<int, 32> readings{};
    for (size_t i = 0; i < readings.size() + 2; ++i) {
        int raw = 0;
        if (!read(raw))
            return false;
        if (i >= 2) {
            if (raw <= 0 || raw >= 4095)
                return false;
            readings[i - 2] = raw;
        }
    }
    int pin_mv = 0;
    if (!calibrate(BatteryRestVoltage::TrimmedRaw(readings), pin_mv) || pin_mv <= 0)
        return false;
    battery_mv = pin_mv * 2;
    return true;
}
}  // namespace wzh
#endif
