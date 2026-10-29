#ifndef WZH_CRAWLPANDA_POWER_H_
#define WZH_CRAWLPANDA_POWER_H_

#include <atomic>
#include <cstdint>
#include <functional>

#include <driver/temperature_sensor.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>

#include "wzh_battery_voltage.h"
#include "wzh_servo_output.h"

// 管理自锁电源、模拟电源键和电池电压采样。
//
// 电源键状态机（参照 testBoard_wifiweb）：
//   IDLE → 按下消抖 → PRESSED → 松开消抖 → IDLE，用时间戳计时；
//   上电防误关机（armed）：开机瞬间按键被按住时，先等一次释放才生效；
//   长按（按住过程中实时判断）触发关机序列，松开可取消；
//   短按做双击检测切换对话并给提示音。
//
// 关机序列：关机提示音 → 舵机回中（板级回调）→ 断开舵机电源 →
//           断开电源自锁 → 等待断电。
class PandaPower {
public:
    using ShutdownHook = std::function<void()>;
    using TouchHook =
        std::function<void(uint8_t code, uint8_t pressed_mask, uint8_t released_mask)>;

    bool Initialize();
    void RegisterServoOutput(wzh::ServoOutput* output) { servo_output_.store(output); }
    // 关机序列中、断电前调用：让板级把舵机放回安全姿态。
    void RegisterShutdownHook(ShutdownHook hook);
    // 仅在经过稳定判定的有效触摸编码发生变化时调用；回调运行于电源任务，
    // 只能做非阻塞投递。
    void RegisterTouchHook(TouchHook hook);
    bool GetTemperature(float& celsius) const {
        celsius = temperature_celsius_.load();
        return temperature_valid_.load();
    }
    int battery_level() const { return battery_level_.load(); }
    int battery_voltage_mv() const { return battery_voltage_mv_.load(); }
    wzh::BatterySamplingStatus battery_sampling() const;
    bool touch_valid() const { return touch_valid_.load(); }
    int touch_code() const { return touch_code_.load(); }
    int touch_voltage_mv() const { return touch_voltage_mv_.load(); }

private:
    std::atomic<wzh::ServoOutput*> servo_output_{nullptr};
    wzh::StableBatteryProtection battery_protection_;
    temperature_sensor_handle_t temperature_ = nullptr;
    bool overheated_ = false;
    std::atomic<float> temperature_celsius_{0.0f};
    std::atomic<bool> temperature_valid_{false};
    void EmergencyShutdown();
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t power_adc_cali_ = nullptr;
    adc_cali_handle_t battery_adc_cali_ = nullptr;
    adc_cali_handle_t touch_adc_cali_ = nullptr;
    std::atomic<int> battery_level_{0};
    std::atomic<int> battery_voltage_mv_{0};
    std::atomic<bool> touch_valid_{false};
    std::atomic<int> touch_code_{0};
    std::atomic<int> touch_voltage_mv_{-1};
    wzh::BatteryRestVoltage battery_rest_voltage_;
    std::atomic<int> battery_live_mv_{0};
    std::atomic<int> battery_protection_mv_{0};
    std::atomic<bool> battery_sample_valid_{false};
    std::atomic<bool> battery_load_active_{true};
    std::atomic<bool> battery_rest_ready_{false};
    std::atomic<uint32_t> battery_rest_updated_ms_{0};
    void ReadBattery(uint32_t now);
    ShutdownHook shutdown_hook_;
    std::atomic<bool> shutdown_hook_ready_{false};
    TouchHook touch_hook_;
    std::atomic<bool> touch_hook_ready_{false};

    static void TaskEntry(void* arg);
    void Task();
    void Shutdown();
};

#endif  // WZH_CRAWLPANDA_POWER_H_
