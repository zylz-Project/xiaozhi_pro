#include "panda_power.h"

#include <algorithm>

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"

#define TAG "PandaPower"

bool PandaPower::Initialize() {
    // 尽早拉高锁存脚，否则松开物理电源键后整机可能立即掉电。
    gpio_config_t latch = {};
    latch.pin_bit_mask = 1ULL << POWER_CTRL_GPIO;
    latch.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&latch));
    ESP_ERROR_CHECK(gpio_set_level(POWER_CTRL_GPIO, 1));

    // 电源键和电池分压共用 ADC1，后台任务按时间片依次采样。
    adc_oneshot_unit_init_cfg_t unit_config = {};
    unit_config.unit_id = ADC_UNIT_1;
    unit_config.ulp_mode = ADC_ULP_MODE_DISABLE;
    esp_err_t err = adc_oneshot_new_unit(&unit_config, &adc_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC initialization failed: %s", esp_err_to_name(err));
        return false;
    }
    adc_oneshot_chan_cfg_t channel_config = {};
    channel_config.atten = ADC_ATTEN_DB_12;
    channel_config.bitwidth = ADC_BITWIDTH_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_, POWER_BUTTON_ADC_CHANNEL, &channel_config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_, BATTERY_ADC_CHANNEL, &channel_config));
    return xTaskCreate(TaskEntry, "panda_power", 3072, this, 1, nullptr) == pdPASS;
}

void PandaPower::TaskEntry(void* arg) { static_cast<PandaPower*>(arg)->Task(); }

void PandaPower::Task() {
    constexpr int kPollMs = 20;
    int pressed_ms = 0;
    bool was_pressed = false;
    int battery_tick = 0;
    while (true) {
        int button_raw = 4095;
        adc_oneshot_read(adc_, POWER_BUTTON_ADC_CHANNEL, &button_raw);
        const bool pressed = button_raw < POWER_BUTTON_ADC_THRESHOLD;
        if (pressed) {
            pressed_ms += kPollMs;
        } else if (was_pressed) {
            // 在松手沿判断长按，避免按住过程中重复拉低锁存脚。
            if (pressed_ms >= POWER_LONG_PRESS_MS) {
                ESP_LOGW(TAG, "Power key long press: releasing power latch");
                gpio_set_level(POWER_CTRL_GPIO, 0);
            }
            pressed_ms = 0;
        }
        was_pressed = pressed;

        // 20 ms * 250 = 5 s。电池电量变化很慢，无需高频占用 ADC。
        if (++battery_tick >= 250) {
            battery_tick = 0;
            int64_t sum = 0;
            // 多次平均可减轻舵机动作和音频功放带来的电源纹波。
            for (int i = 0; i < 32; ++i) {
                int raw = 0;
                adc_oneshot_read(adc_, BATTERY_ADC_CHANNEL, &raw);
                sum += raw;
            }
            const int pin_mv = static_cast<int>(sum / 32) * 3300 / 4095;
            const int battery_mv = pin_mv * BATTERY_DIVIDER_NUMERATOR / BATTERY_DIVIDER_DENOMINATOR;
            battery_voltage_mv_.store(battery_mv);
            // 当前采用 3.2～4.2 V 线性估算，只用于 UI 粗略显示，并非精确 SOC。
            const int level = std::clamp((battery_mv - BATTERY_EMPTY_VOLTAGE_MV) * 100 /
                                             (BATTERY_FULL_VOLTAGE_MV - BATTERY_EMPTY_VOLTAGE_MV),
                                         0, 100);
            battery_level_.store(level);
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}