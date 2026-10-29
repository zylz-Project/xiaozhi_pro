#include "panda_power.h"

#include <algorithm>

#include <driver/gpio.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "assets/lang_config.h"
#include "config.h"
#include "wzh_power_startup.h"
#include "wzh_touch_input.h"

#define TAG "PandaPower"

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

adc_cali_handle_t CreateCalibration(adc_channel_t channel) {
    adc_cali_curve_fitting_config_t config = {};
    config.unit_id = ADC_UNIT_1;
    config.chan = channel;
    config.atten = ADC_ATTEN_DB_12;
    config.bitwidth = ADC_BITWIDTH_12;
    adc_cali_handle_t handle = nullptr;
    const esp_err_t err = adc_cali_create_scheme_curve_fitting(&config, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC channel %d calibration unavailable: %s", channel, esp_err_to_name(err));
    }
    return handle;
}

// 按键状态机（与参考工程 testBoard_wifiweb 一致）。
enum class BtnState : uint8_t {
    kIdle,             // 空闲
    kDebouncePress,    // 按下消抖确认中
    kPressed,          // 已确认按下，长按计时中
    kDebounceRelease,  // 松开消抖确认中
};
}  // namespace

bool PandaPower::Initialize() {
    // 暂时维持 CPU 供电以确认长按；音频、运动及网络尚未启动。
    // 短按释放会在资格检查中撤销锁存，不进入产品运行状态。
    gpio_config_t latch = {};
    latch.pin_bit_mask = 1ULL << POWER_CTRL_GPIO;
    latch.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&latch));
    ESP_ERROR_CHECK(gpio_set_level(POWER_CTRL_GPIO, 1));
    ESP_LOGI(TAG, "POWER_CTRL IO%d HIGH, power latched", POWER_CTRL_GPIO);
    gpio_config_t servo_power = {};
    servo_power.pin_bit_mask = 1ULL << SERVO_POWER_GPIO;
    servo_power.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&servo_power));
    ESP_ERROR_CHECK(gpio_set_level(SERVO_POWER_GPIO, 0));

    // 电源键输入：内部上拉 + 保持 IO 电平，避免休眠期间浮空抖动。
    gpio_config_t button = {};
    button.pin_bit_mask = 1ULL << POWER_BUTTON_GPIO;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    button.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
    gpio_hold_en(POWER_BUTTON_GPIO);

    // 电源键、电池分压和触摸电阻网络共用 ADC1，后台任务按时间片依次采样。
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
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_, TOUCH_ADC_CHANNEL, &channel_config));

    // 曲线拟合校准包含通道参数，三个输入必须分别创建校准句柄。
    power_adc_cali_ = CreateCalibration(POWER_BUTTON_ADC_CHANNEL);
    battery_adc_cali_ = CreateCalibration(BATTERY_ADC_CHANNEL);
    touch_adc_cali_ = CreateCalibration(TOUCH_ADC_CHANNEL);

    // Firmware/OTA software restarts preserve normal recovery. A cold power
    // key start requires a full second of valid held samples before startup.
    if (esp_reset_reason() != ESP_RST_SW) {
        wzh::PowerStartupGate gate;
        auto result = wzh::PowerStartupGate::Result::kWaiting;
        ESP_LOGI(TAG, "Hold power key for 1000 ms to start");
        while (result == wzh::PowerStartupGate::Result::kWaiting) {
            int raw = 4095;
            const auto read = adc_oneshot_read(adc_, POWER_BUTTON_ADC_CHANNEL, &raw);
            const int mv = AdcToMv(power_adc_cali_, raw);
            const bool valid =
                read == ESP_OK && (mv <= POWER_BUTTON_PRESS_MV || mv >= POWER_BUTTON_RELEASE_MV);
            result = gate.Update(esp_log_timestamp(), valid, mv <= POWER_BUTTON_PRESS_MV);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (result != wzh::PowerStartupGate::Result::kReady) {
            ESP_LOGI(TAG, "Short power press rejected; startup cancelled");
            gpio_set_level(POWER_CTRL_GPIO, 0);
            while (true)
                vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "Power hold accepted; starting product");
    }

    // ESP32 die temperature is available on S3; it is not servo winding temperature.
    temperature_sensor_config_t temperature_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    if (temperature_sensor_install(&temperature_config, &temperature_) == ESP_OK) {
        if (temperature_sensor_enable(temperature_) != ESP_OK) {
            temperature_sensor_uninstall(temperature_);
            temperature_ = nullptr;
        }
    }
    return xTaskCreate(TaskEntry, "panda_power", 3072, this, 1, nullptr) == pdPASS;
}

void PandaPower::RegisterShutdownHook(ShutdownHook hook) {
    shutdown_hook_ = std::move(hook);
    shutdown_hook_ready_.store(true, std::memory_order_release);
}

void PandaPower::RegisterTouchHook(TouchHook hook) {
    touch_hook_ = std::move(hook);
    touch_hook_ready_.store(true, std::memory_order_release);
}

void PandaPower::TaskEntry(void* arg) { static_cast<PandaPower*>(arg)->Task(); }

void PandaPower::Shutdown() {
    ESP_LOGI(TAG, "Power-off requested; stop interaction and park active motion");
    // Stop the old call/choreography first, before the shutdown prompt.
    if (shutdown_hook_ready_.load(std::memory_order_acquire) && shutdown_hook_)
        shutdown_hook_();
    Application::GetInstance().StopInteraction();
    const uint32_t started = esp_log_timestamp();
    while (esp_log_timestamp() - started < 2400) {
        if (auto* output = servo_output_.load())
            output->Watchdog();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (auto* output = servo_output_.load())
        output->SetFault(wzh::kShutdown, true);
    gpio_set_level(SERVO_POWER_GPIO, 0);
    Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POWER_OFF);
    vTaskDelay(pdMS_TO_TICKS(700));
    gpio_set_level(POWER_CTRL_GPIO, 0);
    ESP_LOGI(TAG, "Power latch released");
    while (true)
        vTaskDelay(pdMS_TO_TICKS(1000));
}

void PandaPower::EmergencyShutdown() {
    ESP_LOGE(TAG, "Critical battery voltage: immediate shutdown without servo movement");
    if (auto* output = servo_output_.load()) {
        output->SetFault(wzh::kShutdown, true);
    }
    gpio_set_level(SERVO_POWER_GPIO, 0);
    gpio_set_level(POWER_CTRL_GPIO, 0);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

wzh::BatterySamplingStatus PandaPower::battery_sampling() const {
    const int64_t age =
        battery_voltage_mv_.load() > 0
            ? static_cast<uint32_t>(esp_log_timestamp() - battery_rest_updated_ms_.load())
            : int64_t(-1);
    return {battery_live_mv_.load(),      battery_protection_mv_.load(),
            battery_sample_valid_.load(), battery_load_active_.load(),
            battery_rest_ready_.load(),   age};
}

void PandaPower::ReadBattery(uint32_t now) {
    std::array<int, 32> readings;
    bool valid = true;
    // Discard the first two conversions after switching from the touch channel.
    // No delays: the power key and watchdog keep their existing polling budget.
    for (unsigned i = 0; i < readings.size() + 2; ++i) {
        int raw = 0;
        if (adc_oneshot_read(adc_, BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
            valid = false;
            break;
        }
        if (i >= 2)
            readings[i - 2] = raw;
    }
    if (!valid) {
        battery_protection_.Invalidate();
        battery_rest_voltage_.Invalidate();
        battery_sample_valid_.store(false);
        battery_live_mv_.store(0);
        battery_protection_mv_.store(0);
        battery_rest_ready_.store(false);
        if (auto* output = servo_output_.load())
            output->SetFault(wzh::kBatteryPending, true);
        return;
    }
    const int raw = wzh::BatteryRestVoltage::TrimmedRaw(readings);
    const int battery_mv = AdcToMv(battery_adc_cali_, raw) * BATTERY_DIVIDER_RATIO;
    battery_live_mv_.store(battery_mv);
    battery_sample_valid_.store(true);
    // Normal protection uses the temporal median plus three confirmations;
    // recovery requires rest. Critical undervoltage has its own live path.
    battery_protection_.Update(battery_mv, battery_rest_voltage_.RestReady(now), BATTERY_LOW_MV,
                               BATTERY_RECOVER_MV, BATTERY_CRITICAL_MV);
    battery_protection_mv_.store(battery_protection_.millivolts());
    if (auto* output = servo_output_.load()) {
        output->SetFault(wzh::kLowBattery, battery_protection_.low());
        output->SetFault(wzh::kBatteryPending, !battery_protection_.ready());
    }
    if (battery_protection_.critical())
        EmergencyShutdown();
    if (battery_rest_voltage_.Update(battery_mv, now)) {
        const int rest_mv = battery_rest_voltage_.millivolts();
        battery_voltage_mv_.store(rest_mv);
        battery_rest_updated_ms_.store(battery_rest_voltage_.updated_ms());
        // Approximate UI percentage; protection never uses this estimate.
        battery_level_.store(std::clamp((rest_mv - BATTERY_EMPTY_VOLTAGE_MV) * 100 /
                                            (BATTERY_FULL_VOLTAGE_MV - BATTERY_EMPTY_VOLTAGE_MV),
                                        0, 100));
    }
}

void PandaPower::Task() {
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
    uint32_t last_sample_ms = 0;
    bool power_adc_fault = false;
    // 双击检测。
    TickType_t last_click_tick = 0;
    int click_count = 0;
    uint32_t last_battery_sample_ms = 0;
    bool battery_settled = false;
    wzh::TouchInputFilter touch_filter(TOUCH_STABLE_SAMPLES);

    vTaskDelay(pdMS_TO_TICKS(1200));  // 等待音频服务就绪

    // 开机提示音：内嵌上行叮咚（不读外部 Flash，硬件就绪即可响，早于
    // 联网流程）。叮咚先播，控制器 +2s 的 Flash 开机叫声经 IsPlaybackIdle
    // 门控自动排在叮咚之后，两者不重叠。
    Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POWER_ON);
    ESP_LOGI(TAG, "Power task ready");
    uint32_t last_battery_log_ms = 0;

    while (true) {
        if (auto* output = servo_output_.load()) {
            output->Watchdog();
        }
        int raw = 4095;
        const esp_err_t power_err = adc_oneshot_read(adc_, POWER_BUTTON_ADC_CHANNEL, &raw);
        const int power_mv = AdcToMv(power_adc_cali_, raw);
        // 灰区电压代表接触状态未知，既不算按下也不算松开；采样间隔过大
        // 同样不能把中断前的按下和当前位置拼成一次长按。
        const bool power_sample_valid =
            power_err == ESP_OK &&
            (power_mv <= POWER_BUTTON_PRESS_MV || power_mv >= POWER_BUTTON_RELEASE_MV);
        const bool pressed = power_mv <= POWER_BUTTON_PRESS_MV;
        const uint32_t now = esp_log_timestamp();
        const bool sample_gap =
            last_sample_ms != 0 && now - last_sample_ms > POWER_MAX_SAMPLE_GAP_MS;
        last_sample_ms = now;
        int hold_ms = 0;

        if (!power_sample_valid || sample_gap) {
            if (!power_adc_fault)
                ESP_LOGW(TAG, "Power key sample invalid: %s raw=%d value=%dmV",
                         esp_err_to_name(power_err), raw, power_mv);
            power_adc_fault = true;
            state = BtnState::kIdle;
            armed = false;
            press_start_ms = 0;
            click_count = 0;
        } else {
            if (power_adc_fault)
                ESP_LOGI(TAG, "Power key sample valid again; wait for a stable release");
            power_adc_fault = false;
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
                            // 短按只统计双击；对话切换及提示音由主任务串行处理。
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
                                Application::GetInstance().ToggleConversation();
                            }
                        }
                        state = BtnState::kIdle;
                        press_start_ms = 0;
                    }
                    break;
            }
        }

        // IO5 上的 4-bit R-2R 触摸网络：每轮取 3 次 ADC 中值，再进行
        // 5 样本（100ms）稳定判定，避免手指接触边缘时反复跳码。
        int touch_raw[3] = {};
        bool touch_read_valid = true;
        for (int& sample : touch_raw) {
            if (adc_oneshot_read(adc_, TOUCH_ADC_CHANNEL, &sample) != ESP_OK)
                touch_read_valid = false;
        }
        const int touch_mv =
            touch_read_valid
                ? AdcToMv(touch_adc_cali_,
                          wzh::TouchInputFilter::Median3(touch_raw[0], touch_raw[1], touch_raw[2]))
                : -1;
        touch_voltage_mv_.store(touch_mv);
        wzh::TouchInputFilter::Event touch_event = {};
        if (touch_filter.Update(touch_mv, touch_event)) {
            const bool accept_interaction = touch_filter.AcceptInteraction(touch_event);
            if (touch_event.code < 0) {
                touch_valid_.store(false);
                ESP_LOGW(TAG, "[TOUCH] invalid voltage=%dmV; cancel contact until released",
                         touch_event.voltage_mv);
                if (touch_hook_ready_.load(std::memory_order_acquire) && touch_hook_)
                    touch_hook_(0xFF, 0, 0);
            } else {
                touch_valid_.store(true);
                touch_code_.store(touch_event.code);
                const uint8_t previous =
                    touch_event.previous_code >= 0 ? touch_event.previous_code : 0;
                const uint8_t code = static_cast<uint8_t>(touch_event.code);
                const uint8_t pressed_mask = code & static_cast<uint8_t>(~previous);
                const uint8_t released_mask = previous & static_cast<uint8_t>(~code);
                ESP_LOGD(TAG, "[TOUCH] code=0x%02X voltage=%dmV pressed=0x%02X released=0x%02X",
                         code, touch_event.voltage_mv, pressed_mask, released_mask);
                if (accept_interaction && touch_hook_ready_.load(std::memory_order_acquire) &&
                    touch_hook_) {
                    if (pressed_mask)
                        ESP_LOGI(TAG, "Input=touch code=0x%02X voltage=%dmV", code,
                                 touch_event.voltage_mv);
                    touch_hook_(code, pressed_mask, released_mask);
                }
            }
        }

        // UI uses rest voltage; stable live voltage protects even during motion.
        const auto* battery_output = servo_output_.load();
        const bool battery_loaded = battery_output == nullptr || battery_output->powered() ||
                                    !Application::GetInstance().GetAudioService().IsPlaybackIdle();
        battery_rest_voltage_.ObserveLoad(battery_loaded, now);
        battery_load_active_.store(battery_loaded);
        battery_rest_ready_.store(battery_rest_voltage_.RestReady(now));
        // The high-impedance divider needs its original 15 s startup settling.
        if ((battery_settled || now >= 15000) &&
            now - last_battery_sample_ms >= BATTERY_READ_INTERVAL_MS) {
            battery_settled = true;
            last_battery_sample_ms = now;
            ReadBattery(now);
            if (last_battery_log_ms == 0 || now - last_battery_log_ms >= 30000) {
                ESP_LOGI(TAG, "Battery rest=%dmV live=%dmV level=%d%% valid=%d load=%d",
                         battery_voltage_mv_.load(), battery_live_mv_.load(), battery_level_.load(),
                         battery_sample_valid_.load(), battery_loaded);
                last_battery_log_ms = now;
            }
        }
        if (temperature_ != nullptr) {
            float celsius = 0;
            if (temperature_sensor_get_celsius(temperature_, &celsius) == ESP_OK) {
                temperature_celsius_.store(celsius);
                temperature_valid_.store(true);
                if (celsius >= 90.0f) {
                    overheated_ = true;
                } else if (celsius <= 85.0f) {
                    overheated_ = false;
                }
                if (auto* output = servo_output_.load()) {
                    output->SetFault(wzh::kOverheat, overheated_);
                }
            } else {
                temperature_valid_.store(false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}
