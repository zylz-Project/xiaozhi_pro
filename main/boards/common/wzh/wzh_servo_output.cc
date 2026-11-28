#include "wzh_servo_output.h"

#include <esp_err.h>
#include <esp_log.h>
#include <algorithm>
#include <cmath>

namespace wzh {

void ServoOutput::Initialize(gpio_num_t power, const ledc_channel_t* channels, size_t count,
                             uint32_t duty_max) {
    std::lock_guard<std::mutex> lock(mutex_);
    power_ = power;
    channels_ = channels;
    count_ = count;
    duty_max_ = duty_max;
    ReleaseLocked();
}

bool ServoOutput::Write(const int* angles, size_t count) {
    if (angles == nullptr || count > 5) {
        return false;
    }
    float degrees[5];
    for (size_t i = 0; i < count; ++i) {
        degrees[i] = angles[i];
    }
    return WriteDegrees(degrees, count);
}

bool ServoOutput::WriteDegrees(const float* angles, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t now = esp_timer_get_time() / 1000;
    CheckWatchdogLocked(now);
    const bool already_powered = powered_.load();
    if (angles == nullptr || channels_ == nullptr || count != count_ || blocked()) {
        if (already_powered)
            ReleaseLocked();
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(angles[i])) {
            ReleaseLocked();
            return false;
        }
    }
    // LEDC repeats the programmed waveform while the rail is on. After
    // release, all channels must be started again even for the same pose.
    const bool cache_duties = count <= last_duties_.size();
    for (size_t i = 0; i < count; ++i) {
        const float pulse_us = 500.0f + std::clamp(angles[i], 0.0f, 180.0f) * 2000.0f / 180.0f;
        // LEDC divides by 2^N timer counts, not its largest duty value 2^N-1.
        const uint32_t duty =
            static_cast<uint32_t>(std::lround(pulse_us * (duty_max_ + 1.f) / 20000.0f));
        if (cache_duties && already_powered && duty == last_duties_[i])
            continue;
        if (ledc_set_duty(LEDC_LOW_SPEED_MODE, channels_[i], duty) != ESP_OK ||
            ledc_update_duty(LEDC_LOW_SPEED_MODE, channels_[i]) != ESP_OK) {
            ReleaseLocked();
            ESP_LOGW("WzhServo", "PWM write failed on channel %d", channels_[i]);
            return false;
        }
        if (cache_duties)
            last_duties_[i] = duty;
    }
    if (!already_powered) {
        if (gpio_set_level(power_, 1) != ESP_OK) {
            ReleaseLocked();
            ESP_LOGW("WzhServo", "Could not enable servo power on GPIO %d", power_);
            return false;
        }
        budget_.PowerOn(now);
        powered_.store(true);
    }
    last_write_ms_ = now;
    return true;
}

void ServoOutput::ReleaseLocked() {
    if (power_ != GPIO_NUM_NC) {
        gpio_set_level(power_, 0);
    }
    for (size_t i = 0; i < count_; ++i) {
        ledc_stop(LEDC_LOW_SPEED_MODE, channels_[i], 0);
    }
    budget_.Release();
    powered_.store(false);
}

void ServoOutput::Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (powered_.load())
        ReleaseLocked();
}

void ServoOutput::SetFault(MotionFault fault, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool previous = (faults_.load() & fault) != 0;
    if (previous == enabled)
        return;
    if (enabled) {
        faults_.fetch_or(fault);
        ReleaseLocked();
    } else {
        faults_.fetch_and(~static_cast<uint32_t>(fault));
    }
    if (enabled && (fault == kLowBattery || fault == kOverheat || fault == kFalling)) {
        ESP_LOGW("WzhServo", "Protection=%s ON; servo power released", MotionFaultName(fault));
    } else {
        ESP_LOGI("WzhServo", "Protection=%s %s", MotionFaultName(fault), enabled ? "ON" : "OFF");
    }
}

void ServoOutput::Watchdog() {
    std::lock_guard<std::mutex> lock(mutex_);
    CheckWatchdogLocked(esp_timer_get_time() / 1000);
}

void ServoOutput::CheckWatchdogLocked(uint32_t now) {
    const bool cooling = !budget_.Allow(now);
    const bool cooling_changed = cooling_.exchange(cooling) != cooling;
    // Check before accepting data too: a late write must not erase an expired
    // deadline while the independent power task is delayed.
    const bool timed_out = powered_.load() && now - last_write_ms_ >= 500;
    if (timed_out)
        faults_.fetch_or(kMotionTimeout);
    if (blocked() && powered_.load()) {
        ReleaseLocked();
    }
    if (cooling_changed)
        ESP_LOGI("WzhServo", "Motion power cooldown %s", cooling ? "ON" : "OFF");
    if (timed_out)
        ESP_LOGW("WzhServo", "Motion watchdog: no update for 500ms; servo power released");
}

}  // namespace wzh
