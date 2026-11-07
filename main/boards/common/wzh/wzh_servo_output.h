#ifndef WZH_SERVO_OUTPUT_H_
#define WZH_SERVO_OUTPUT_H_

#include <atomic>
#include <mutex>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include "wzh_protection.h"

namespace wzh {

// The only owner of PWM/power writes after board channel initialization.
class ServoOutput {
public:
    void Initialize(gpio_num_t power, const ledc_channel_t* channels, size_t count);
    bool Write(const int* angles, size_t count);
    bool WriteDegrees(const float* angles, size_t count);
    void Release();
    void SetFault(MotionFault fault, bool enabled);
    // Called independently by the power task, even during a long motion clip.
    void Watchdog();
    bool blocked() const { return faults_.load() != 0 || cooling_.load(); }
    bool powered() const { return powered_.load(); }
    uint32_t faults() const { return faults_.load() | (cooling_.load() ? 16U : 0U); }

private:
    std::mutex mutex_;
    std::atomic<uint32_t> faults_{kBatteryPending};
    std::atomic<bool> powered_{false};
    std::atomic<bool> cooling_{false};
    gpio_num_t power_ = GPIO_NUM_NC;
    const ledc_channel_t* channels_ = nullptr;
    size_t count_ = 0;
    ServoPowerBudget budget_;
    uint32_t last_write_ms_ = 0;
    void ReleaseLocked();
};

}  // namespace wzh
#endif  // WZH_SERVO_OUTPUT_H_
