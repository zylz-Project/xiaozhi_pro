#include "wzh_servo_output.h"

#include <esp_log.h>
#include <algorithm>
#include <cmath>

namespace wzh {

void ServoOutput::Initialize(gpio_num_t power, const ledc_channel_t* channels, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    power_ = power;
    channels_ = channels;
    count_ = count;
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
    const bool cooling = !budget_.Allow(now);
    if (cooling_.exchange(cooling) != cooling)
        ESP_LOGI("WzhServo", "Motion power cooldown %s", cooling ? "ON" : "OFF");
    if (angles == nullptr || channels_ == nullptr || count != count_ || blocked()) {
        if (powered_.load())
            ReleaseLocked();
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(angles[i])) {
            ReleaseLocked();
            return false;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const float pulse_us = 500.0f + std::clamp(angles[i], 0.0f, 180.0f) * 2000.0f / 180.0f;
        const uint32_t duty = static_cast<uint32_t>(std::lround(pulse_us * 8191.0f / 20000.0f));
        ledc_set_duty(LEDC_LOW_SPEED_MODE, channels_[i], duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, channels_[i]);
    }
    gpio_set_level(power_, 1);
    budget_.PowerOn(now);
    powered_.store(true);
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
    const uint32_t now = esp_timer_get_time() / 1000;
    const bool cooling = !budget_.Allow(now);
    if (cooling_.exchange(cooling) != cooling)
        ESP_LOGI("WzhServo", "Motion power cooldown %s", cooling ? "ON" : "OFF");
    // Power task checks this independently every 20ms. A stalled motion task cannot
    // leave PWM holding torque for the entire continuous-power budget.
    if (powered_.load() && now - last_write_ms_ >= 500) {
        if ((faults_.fetch_or(kMotionTimeout) & kMotionTimeout) == 0)
            ESP_LOGW("WzhServo", "Motion watchdog: no update for 500ms; servo power released");
    }
    if (blocked() && powered_.load()) {
        ReleaseLocked();
    }
}

}  // namespace wzh
