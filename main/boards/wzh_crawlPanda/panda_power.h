#ifndef WZH_CRAWLPANDA_POWER_H_
#define WZH_CRAWLPANDA_POWER_H_

#include <atomic>
#include <functional>

#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>

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

    bool Initialize();
    // 关机序列中、断电前调用：让板级把舵机放回安全姿态。
    void RegisterShutdownHook(ShutdownHook hook);
    int battery_level() const { return battery_level_.load(); }
    int battery_voltage_mv() const { return battery_voltage_mv_.load(); }

private:
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t adc_cali_ = nullptr;
    std::atomic<int> battery_level_{0};
    std::atomic<int> battery_voltage_mv_{0};
    int battery_filtered_mv_ = 0;
    ShutdownHook shutdown_hook_;

    static void TaskEntry(void* arg);
    void Task();
    void Shutdown();
};

#endif  // WZH_CRAWLPANDA_POWER_H_