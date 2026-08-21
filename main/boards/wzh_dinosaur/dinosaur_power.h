#ifndef WZH_DINOSAUR_POWER_H_
#define WZH_DINOSAUR_POWER_H_

#include <atomic>

#include <esp_adc/adc_oneshot.h>

// 管理自锁电源、模拟电源键和电池电压采样。
// 采样工作放在低优先级任务中，读取接口通过原子变量返回最近一次结果。
class DinosaurPower {
public:
    bool Initialize();
    int battery_level() const { return battery_level_.load(); }
    int battery_voltage_mv() const { return battery_voltage_mv_.load(); }

private:
    adc_oneshot_unit_handle_t adc_ = nullptr;
    std::atomic<int> battery_level_{0};
    std::atomic<int> battery_voltage_mv_{0};

    static void TaskEntry(void* arg);
    void Task();
};

#endif  // WZH_DINOSAUR_POWER_H_
