#include "dinosaur_power.h"

#include <algorithm>

#include <driver/gpio.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "assets/lang_config.h"
#include "config.h"

#define TAG "DinosaurPower"

namespace {
int AdcToMv(adc_cali_handle_t cali, int raw) {
    if (cali != nullptr) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
            return mv;
        }
    }
    return raw * 3300 / 4096;
}

// 按键状态机（与参考工程 testBoard_wifiweb 一致）。
enum class BtnState : uint8_t {
    kIdle,            // 空闲
    kDebouncePress,   // 按下消抖确认中
    kPressed,         // 已确认按下，长按计时中
    kDebounceRelease, // 松开消抖确认中
};
}  // namespace

bool DinosaurPower::Initialize() {
    // 电源控制引脚：尽早拉高锁存脚，否则松开物理电源键后整机可能立即掉电。
    // （IO7 同时是舵机电源链路上的主板供电锁存。）
    gpio_config_t latch = {};
    latch.pin_bit_mask = 1ULL << POWER_CTRL_GPIO;
    latch.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&latch));
    ESP_ERROR_CHECK(gpio_set_level(POWER_CTRL_GPIO, 1));
    ESP_LOGI(TAG, "POWER_CTRL IO%d HIGH, power latched", POWER_CTRL_GPIO);

    // 电源键输入：内部上拉 + 保持 IO 电平，避免休眠期间浮空抖动。
    gpio_config_t button = {};
    button.pin_bit_mask = 1ULL << POWER_BUTTON_GPIO;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    button.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
    gpio_hold_en(POWER_BUTTON_GPIO);

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

    // ADC 曲线校准，让电池电压换算更准确。
    adc_cali_curve_fitting_config_t cali = {};
    cali.unit_id = ADC_UNIT_1;
    cali.chan = POWER_BUTTON_ADC_CHANNEL;
    cali.atten = ADC_ATTEN_DB_12;
    cali.bitwidth = ADC_BITWIDTH_12;
    adc_cali_create_scheme_curve_fitting(&cali, &adc_cali_);

    return xTaskCreate(TaskEntry, "dino_power", 3072, this, 1, nullptr) == pdPASS;
}

void DinosaurPower::RegisterShutdownHook(ShutdownHook hook) {
    shutdown_hook_ = std::move(hook);
}

void DinosaurPower::TaskEntry(void* arg) { static_cast<DinosaurPower*>(arg)->Task(); }

void DinosaurPower::Shutdown() {
    ESP_LOGW(TAG, "Long press %d ms: power-off sequence...", POWER_LONG_PRESS_MS);
    // 0. 关机提示音。
    Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_VIBRATION);
    vTaskDelay(pdMS_TO_TICKS(400));
    // 1. 舵机回中（板级回调，断电前回到安全姿态）。
    if (shutdown_hook_) {
        shutdown_hook_();
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    // 2. 断开舵机电源。
    gpio_set_level(SERVO_POWER_GPIO, 0);
    ESP_LOGI(TAG, "Servo power IO%d -> LOW", SERVO_POWER_GPIO);
    vTaskDelay(pdMS_TO_TICKS(300));
    // 3. 切断电源自锁，等待断电。
    gpio_set_level(POWER_CTRL_GPIO, 0);
    ESP_LOGW(TAG, "Power latch IO%d -> LOW, system powering off", POWER_CTRL_GPIO);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void DinosaurPower::Task() {
    constexpr int kPollMs = POWER_POLL_MS;
    // 状态机（时间戳计时，不受调度抖动影响）。
    BtnState state = BtnState::kIdle;
    uint32_t state_since_ms = 0;  // 进入当前状态的时刻
    uint32_t press_start_ms = 0;  // 确认按下时刻
    int hold_tip = 0;             // 长按进度提示档位（500ms/档）
    // 上电防误关机：开机瞬间按键被按住（开机路径本身要按着），
    // 必须等按键先释放一次，长按关机才生效。
    bool armed = false;
    bool boot_hold_logged = false;
    // 双击检测。
    TickType_t last_click_tick = 0;
    int click_count = 0;
    int battery_tick = 0;

    vTaskDelay(pdMS_TO_TICKS(3000));  // 等待系统稳定

    // 开机提示音由 flash 开机叫声（+2s）和激活成功音（OGG_SUCCESS）承担，
    // 这里不再重复播放，避免与连上大模型的提示音相撞。
    ESP_LOGI(TAG, "Power task ready");

    while (true) {
        ++battery_tick;
        int raw = 4095;
        adc_oneshot_read(adc_, POWER_BUTTON_ADC_CHANNEL, &raw);
        const bool pressed = AdcToMv(adc_cali_, raw) < 1000;
        const uint32_t now = esp_log_timestamp();
        int hold_ms = 0;

        switch (state) {
            case BtnState::kIdle:
                if (pressed) {
                    state = BtnState::kDebouncePress;
                    state_since_ms = now;
                } else if (!armed) {
                    // 上电后第一次确认释放 → 武装长按关机。
                    armed = true;
                    boot_hold_logged = false;
                    ESP_LOGI(TAG, "Power key released; long-press shutdown armed");
                }
                break;

            case BtnState::kDebouncePress:
                if (!pressed) {
                    state = BtnState::kIdle;  // 毛刺，复位
                } else if (now - state_since_ms >= POWER_DEBOUNCE_MS) {
                    if (!armed) {
                        // 上电期间一直按着：不进入长按计时，等松开。
                        if (!boot_hold_logged) {
                            boot_hold_logged = true;
                            ESP_LOGI(TAG, "Power key held during boot; long-press ignored");
                        }
                        state = BtnState::kIdle;
                        break;
                    }
                    state = BtnState::kPressed;  // 消抖确认按下
                    press_start_ms = now;
                    hold_tip = 0;
                    ESP_LOGI(TAG, "Power key pressed (raw=%d)", raw);
                }
                break;

            case BtnState::kPressed:
                if (!pressed) {
                    state = BtnState::kDebounceRelease;  // 进入松开消抖
                    state_since_ms = now;
                } else {
                    hold_ms = static_cast<int>(now - press_start_ms);
                    if (hold_ms >= POWER_LONG_PRESS_MS) {
                        Shutdown();  // 长按到达 → 关机（此函数不返回）
                    }
                    // 长按进度提示（每 500ms 一次），松开可取消。
                    const int tip = hold_ms / 500;
                    if (tip != hold_tip && tip > 0) {
                        hold_tip = tip;
                        ESP_LOGI(TAG, "Long press %d.%d s (need %d s), release to cancel",
                                 hold_ms / 1000, (hold_ms % 1000) / 100,
                                 POWER_LONG_PRESS_MS / 1000);
                    }
                }
                break;

            case BtnState::kDebounceRelease:
                if (pressed) {
                    state = BtnState::kPressed;  // 没松干净，回到按住
                } else if (now - state_since_ms >= POWER_DEBOUNCE_MS) {
                    const int dur =
                        press_start_ms != 0 ? static_cast<int>(now - press_start_ms) : 0;
                    ESP_LOGI(TAG, "Power key released (held %d ms)", dur);
                    if (dur < POWER_LONG_PRESS_MS) {
                        // 短按：双击检测，切换对话并给提示音。
                        const TickType_t tick_now = xTaskGetTickCount();
                        if (tick_now - last_click_tick <= pdMS_TO_TICKS(400)) {
                            ++click_count;
                        } else {
                            click_count = 1;
                        }
                        last_click_tick = tick_now;
                        if (click_count >= 2) {
                            click_count = 0;
                            ESP_LOGI(TAG, "Power key double click: toggle chat");
                            auto& app = Application::GetInstance();
                            app.GetAudioService().PlaySound(
                                app.GetDeviceState() == kDeviceStateIdle
                                    ? Lang::Sounds::OGG_SUCCESS
                                    : Lang::Sounds::OGG_POPUP);
                            app.ToggleChatState();
                        }
                    }
                    state = BtnState::kIdle;
                    press_start_ms = 0;
                }
                break;
        }

        // 电池分压采样：每 5 秒一次，32 次平均 + IIR 滤波。
        if (battery_tick >= BATTERY_READ_TICKS) {
            battery_tick = 0;
            int64_t sum = 0;
            for (int i = 0; i < 32; ++i) {
                int bat_raw = 0;
                adc_oneshot_read(adc_, BATTERY_ADC_CHANNEL, &bat_raw);
                sum += bat_raw;
            }
            const int pin_mv = AdcToMv(adc_cali_, static_cast<int>(sum / 32));
            const int battery_mv = pin_mv * BATTERY_DIVIDER_RATIO;
            if (battery_filtered_mv_ == 0) {
                battery_filtered_mv_ = battery_mv;
            } else {
                battery_filtered_mv_ += (battery_mv - battery_filtered_mv_) / 5;
            }
            battery_voltage_mv_.store(battery_filtered_mv_);
            // 3.2～4.2 V 线性估算电量，只用于 UI 粗略显示，并非精确 SOC。
            const int level = std::clamp((battery_filtered_mv_ - BATTERY_EMPTY_VOLTAGE_MV) * 100 /
                                             (BATTERY_FULL_VOLTAGE_MV - BATTERY_EMPTY_VOLTAGE_MV),
                                         0, 100);
            battery_level_.store(level);
            ESP_LOGI(TAG, "[BAT] %dmV (filt=%dmV) level=%d%%", battery_mv,
                     battery_filtered_mv_, level);
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}