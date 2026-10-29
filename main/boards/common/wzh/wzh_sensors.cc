#include "wzh_sensors.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mcp_server.h"
#include "wzh_pet_behavior.h"
#include "wzh_posture_input.h"

namespace wzh {
namespace {
constexpr const char* kTag = "WzhSensors";
int Orientation(int x, int y, int z) {
    const int magnitudes[] = {std::abs(x), std::abs(y), std::abs(z)};
    const int values[] = {x, y, z};
    int axis = 0;
    for (int i = 1; i < 3; ++i) {
        if (magnitudes[i] > magnitudes[axis]) {
            axis = i;
        }
    }
    const int second = std::max(magnitudes[(axis + 1) % 3], magnitudes[(axis + 2) % 3]);
    return magnitudes[axis] >= 700 && magnitudes[axis] - second >= 150
               ? axis * 2 + (values[axis] < 0 ? 1 : 0)
               : -1;
}
}  // namespace

bool Sensors::Read(uint8_t reg, uint8_t* data, size_t size) {
    if (size > 1) {
        reg |= 0x80;
    }
    return i2c_master_transmit_receive(device_, &reg, 1, data, size, 20) == ESP_OK;
}

bool Sensors::Write(uint8_t reg, uint8_t value) {
    const uint8_t bytes[] = {reg, value};
    return i2c_master_transmit(device_, bytes, sizeof(bytes), 20) == ESP_OK;
}

bool Sensors::InitializeAccelerometer(gpio_num_t sda, gpio_num_t scl) {
    i2c_master_bus_config_t bus_config = {};
    // I2C0 belongs to ES8311. These two wires are a separate physical bus.
    bus_config.i2c_port = I2C_NUM_1;
    bus_config.sda_io_num = sda;
    bus_config.scl_io_num = scl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = 1;
    if (i2c_new_master_bus(&bus_config, &bus_) != ESP_OK) {
        return false;
    }
    for (uint8_t address : {0x18, 0x19}) {
        if (i2c_master_probe(bus_, address, 20) != ESP_OK) {
            continue;
        }
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = address;
        config.scl_speed_hz = 100000;
        if (i2c_master_bus_add_device(bus_, &config, &device_) != ESP_OK) {
            continue;
        }
        uint8_t id = 0, version = 0;
        if (Read(0x0f, &id, 1) && Read(0x70, &version, 1) && id == 0x11 && version == 0x28) {
            ESP_LOGI(kTag, "SC7A20H address=0x%02x WHO_AM_I=0x%02x VERSION=0x%02x", address, id,
                     version);
            break;
        }
        ESP_LOGW(kTag,
                 "SC7A20H rejected address=0x%02x id=0x%02x version=0x%02x (expected 0x11/0x28)",
                 address, id, version);
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }
    if (device_ == nullptr) {
        i2c_del_master_bus(bus_);
        bus_ = nullptr;
        return false;
    }
    bool ok = Write(0x68, 0xa5);
    vTaskDelay(pdMS_TO_TICKS(10));
    // Match testBoard_wifiweb: ±4g, 12bit, BDU, 100 Hz, no unwired interrupts.
    ok = ok && Write(0x1f, 0) && Write(0x21, 0) && Write(0x22, 0) && Write(0x23, 0x90) &&
         Write(0x24, 0) && Write(0x25, 0) && Write(0x20, 0x57);
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t ctrl1 = 0, ctrl4 = 0;
    ok = ok && Read(0x20, &ctrl1, 1) && Read(0x23, &ctrl4, 1) && ctrl1 == 0x57 && ctrl4 == 0x90;
    if (!ok) {
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        i2c_del_master_bus(bus_);
        bus_ = nullptr;
    }
    return ok;
}

bool Sensors::Initialize(gpio_num_t human, gpio_num_t sda, gpio_num_t scl, ServoOutput* output,
                         EventHook hook) {
    human_ = human;
    output_ = output;
    hook_ = std::move(hook);
    gpio_config_t gpio = {};
    gpio.pin_bit_mask = 1ULL << human;
    gpio.mode = GPIO_MODE_INPUT;
    gpio.pull_down_en = GPIO_PULLDOWN_ENABLE;
    if (gpio_config(&gpio) != ESP_OK) {
        return false;
    }
    sda_ = sda;
    scl_ = scl;
    ESP_LOGI(
        kTag,
        "WZH feedback-v2; expected SC7A20H accelerometer, I2C1 SDA=%d SCL=%d, addresses=0x18/0x19",
        sda, scl);
    status_.accelerometer = InitializeAccelerometer(sda, scl);
    ESP_LOGI(kTag, "Human IO%d; SC7A20H on I2C1 IO%d/IO%d: %s", human, sda, scl,
             status_.accelerometer ? "ready" : "unavailable");
    return xTaskCreate([](void* arg) { static_cast<Sensors*>(arg)->Task(); }, "wzh_sensors", 4096,
                       this, 1, nullptr) == pdPASS;
}

void Sensors::Task() {
    PresenceGreeting presence;
    PostureInput posture;
    int fall_samples = 0, safe_samples = 0, read_failures = 0;
    bool falling = false;
    uint32_t next_greeting_attempt = 0, next_probe = 0, next_diagnostic = 0, next_event_attempt = 0;
    while (true) {
        const uint32_t now = esp_timer_get_time() / 1000;
        const bool human = gpio_get_level(human_) != 0;
        if (presence.Update(human, now, !output_->blocked() && !output_->powered()) && hook_ &&
            static_cast<int32_t>(now - next_greeting_attempt) >= 0) {
            if (hook_(Event::kHuman)) {
                ESP_LOGI(kTag, "Input=presence rising");
                presence.Accepted(now);
            }
            next_greeting_attempt = now + 1000;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.human = presence.present();
        }
        if (device_ == nullptr && static_cast<int32_t>(now - next_probe) >= 0) {
            const bool ready = InitializeAccelerometer(sda_, scl_);
            if (ready)
                read_failures = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.accelerometer = ready;
            }
            if (!ready)
                ESP_LOGW(kTag,
                         "SC7A20H unavailable: no valid response on I2C1 IO%d/%d; retry in 5s",
                         sda_, scl_);
            next_probe = now + 5000;
            posture.Invalidate();
        }
        if (device_ != nullptr) {
            uint8_t bytes[6] = {};
            if (Read(0x28, bytes, sizeof(bytes))) {
                read_failures = 0;
                auto axis = [&bytes](int index) {
                    return (static_cast<int16_t>(bytes[index] | (bytes[index + 1] << 8)) >> 4) * 2;
                };
                const int x = axis(0), y = axis(2), z = axis(4);
                const float magnitude = std::sqrt(float(x * x + y * y + z * z));
                const bool angles_valid = magnitude >= 700 && magnitude <= 1300;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.accelerometer = true;
                    status_.x_mg = x;
                    status_.y_mg = y;
                    status_.z_mg = z;
                    status_.angles_valid = angles_valid;
                    status_.orientation = Orientation(x, y, z);
                    if (angles_valid) {
                        status_.pitch = std::atan2(-x, std::sqrt(float(y * y + z * z))) * 57.29578f;
                        status_.roll = std::atan2(float(y), float(z)) * 57.29578f;
                    }
                }
                fall_samples = magnitude < 250 ? std::min(fall_samples + 1, 3) : 0;
                safe_samples = angles_valid ? std::min(safe_samples + 1, 25) : 0;
                if (fall_samples >= 3)
                    falling = true;
                else if (safe_samples >= 25)
                    falling = false;
                output_->SetFault(kFalling, falling);
                // Protection recovery is not a new interaction. Do not retain
                // handling observed while output was forbidden and play it later.
                if (output_->blocked())
                    posture.Invalidate();
                const auto interaction = posture.Update(x, y, z, output_->powered(), falling, now);
                if (interaction != PetInteraction::kNone && hook_ && !output_->blocked() &&
                    static_cast<int32_t>(now - next_event_attempt) >= 0) {
                    const auto event = interaction == PetInteraction::kShaken    ? Event::kShaken
                                       : interaction == PetInteraction::kFlipped ? Event::kFlipped
                                       : interaction == PetInteraction::kPutDown ? Event::kPutDown
                                                                                 : Event::kPickedUp;
                    const bool accepted = hook_(event);
                    if (accepted) {
                        ESP_LOGI(kTag, "Input=posture %s xyz=%d/%d/%dmg delta=%d",
                                 InteractionName(interaction), x, y, z, posture.delta());
                        posture.Accepted(now);
                    } else {
                        ESP_LOGD(kTag, "Posture=%s deferred", InteractionName(interaction));
                    }
                    next_event_attempt = now + 150;
                }
                if (static_cast<int32_t>(now - next_diagnostic) >= 0) {
                    ESP_LOGD(kTag,
                             "SC7A20H xyz=%d/%d/%dmg norm=%d delta=%d powered=%d faults=0x%lx", x,
                             y, z, int(magnitude), posture.delta(), output_->powered(),
                             static_cast<unsigned long>(output_->faults()));
                    next_diagnostic = now + 3000;
                }
            } else {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.accelerometer = false;
                    status_.angles_valid = false;
                }
                posture.Invalidate();
                fall_samples = safe_samples = 0;
                if (++read_failures >= 3) {
                    ESP_LOGW(
                        kTag,
                        "SC7A20H read failed repeatedly; preserve fall protection and re-probe");
                    i2c_master_bus_rm_device(device_);
                    device_ = nullptr;
                    i2c_del_master_bus(bus_);
                    bus_ = nullptr;
                    next_probe = now + 1000;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void Sensors::RegisterMcpTools(const char* product) {
    McpServer::GetInstance().AddTool(
        std::string("self.") + product + ".get_sensors",
        "Read human presence and SC7A20H acceleration, pitch/roll and validity. Orientation "
        "0..5 means +X/-X/+Y/-Y/+Z/-Z, -1 means tilted. Angles are valid only near 1g.",
        PropertyList(), [this](const PropertyList&) -> ReturnValue {
            std::lock_guard<std::mutex> lock(mutex_);
            auto* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "human", status_.human);
            cJSON_AddBoolToObject(result, "servo_powered", output_->powered());
            cJSON_AddNumberToObject(result, "motion_faults", output_->faults());
            cJSON_AddBoolToObject(result, "accelerometer_valid", status_.accelerometer);
            cJSON_AddBoolToObject(result, "angles_valid", status_.angles_valid);
            cJSON_AddNumberToObject(result, "x_mg", status_.x_mg);
            cJSON_AddNumberToObject(result, "y_mg", status_.y_mg);
            cJSON_AddNumberToObject(result, "z_mg", status_.z_mg);
            cJSON_AddNumberToObject(result, "pitch", status_.pitch);
            cJSON_AddNumberToObject(result, "roll", status_.roll);
            cJSON_AddNumberToObject(result, "orientation", status_.orientation);
            return result;
        });
}

}  // namespace wzh
