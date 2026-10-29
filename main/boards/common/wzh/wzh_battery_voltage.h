#ifndef WZH_BATTERY_VOLTAGE_H_
#define WZH_BATTERY_VOLTAGE_H_

#include <algorithm>
#include <array>
#include <cstdint>

#include "wzh_protection.h"

namespace wzh {

struct BatterySamplingStatus {
    int live_mv = 0;
    int protection_mv = 0;
    bool valid = false;
    bool under_load = true;
    bool rest_ready = false;
    int64_t rest_age_ms = -1;
};

// A temporal median rejects a single loaded/switching dip; the existing three
// confirmations then apply to the stable voltage, not individual ADC bursts.
// Critical undervoltage keeps its independent three-live-sample emergency path.
class StableBatteryProtection {
public:
    void Update(int millivolts, bool rested, int low_mv, int recover_mv, int critical_mv) {
        live_.Update(millivolts, low_mv, recover_mv, critical_mv);
        readings_[position_] = millivolts;
        position_ = (position_ + 1) % readings_.size();
        count_ = std::min<unsigned>(count_ + 1, readings_.size());
        if (count_ < readings_.size())
            return;
        auto sorted = readings_;
        std::sort(sorted.begin(), sorted.end());
        voltage_ = sorted[1];
        stable_.Update(voltage_, low_mv, recover_mv, critical_mv, rested);
    }
    void Invalidate() {
        live_.Invalidate();
        stable_.Invalidate();
        count_ = position_ = 0;
        voltage_ = 0;
    }
    bool ready() const { return stable_.ready(); }
    bool low() const { return stable_.low(); }
    bool critical() const { return live_.critical(); }
    int millivolts() const { return voltage_; }

private:
    BatteryProtection live_, stable_;
    std::array<int, 3> readings_{};
    unsigned count_ = 0, position_ = 0;
    int voltage_ = 0;
};

// UI estimate only. Protection must continue to use every valid live reading,
// including while servos/audio draw current. Owned by the power task, no heap.
class BatteryRestVoltage {
public:
    static constexpr uint32_t kSettleMs = 1200;
    static constexpr uint32_t kHistoryMaxGapMs = 30000;

    // Observe every power-task tick, so a short action between ADC reads also
    // restarts the quiet interval. "Rest" means servos off and playback drained.
    void ObserveLoad(bool loaded, uint32_t now) {
        if (loaded) {
            quiet_ = false;
        } else if (!quiet_) {
            quiet_ = true;
            quiet_since_ = now;
        }
    }

    bool RestReady(uint32_t now) const { return quiet_ && now - quiet_since_ >= kSettleMs; }

    bool Update(int millivolts, uint32_t now) {
        if (!RestReady(now))
            return false;
        if (count_ && now - sampled_ms_ > kHistoryMaxGapMs)
            count_ = position_ = 0;
        sampled_ms_ = now;
        samples_[position_] = millivolts;
        position_ = (position_ + 1) % samples_.size();
        count_ = std::min<unsigned>(count_ + 1, samples_.size());
        if (count_ < samples_.size())
            return false;
        auto sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        const int target = sorted[1] * 8;
        if (!filtered_ || now - updated_ms_ > kHistoryMaxGapMs)
            filtered_ = target;
        else
            filtered_ += (target - filtered_) / 8;
        updated_ms_ = now;
        return true;
    }

    void Invalidate() {
        quiet_ = false;
        count_ = position_ = 0;
    }
    int millivolts() const { return (filtered_ + 4) / 8; }
    uint32_t updated_ms() const { return updated_ms_; }

    // Reject isolated ADC conversion outliers without hiding a sustained drop.
    // All reads belong to one short burst, not a slow protection filter.
    static int TrimmedRaw(std::array<int, 32> readings) {
        std::sort(readings.begin(), readings.end());
        int sum = 0;
        for (unsigned i = 4; i < readings.size() - 4; ++i)
            sum += readings[i];
        return (sum + 12) / 24;
    }

private:
    bool quiet_ = false;
    uint32_t quiet_since_ = 0, sampled_ms_ = 0, updated_ms_ = 0;
    std::array<int, 3> samples_{};
    unsigned count_ = 0, position_ = 0;
    int filtered_ = 0;
};

}  // namespace wzh
#endif
