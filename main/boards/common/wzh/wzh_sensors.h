#ifndef WZH_SENSORS_H_
#define WZH_SENSORS_H_

#include <functional>
#include <mutex>

#include <driver/i2c_master.h>

#include "wzh_servo_output.h"

namespace wzh {

class Sensors {
public:
    enum class Event { kHuman, kPickedUp, kShaken, kFlipped, kPutDown };
    using EventHook = std::function<bool(Event)>;
    bool Initialize(gpio_num_t human, gpio_num_t sda, gpio_num_t scl, ServoOutput* output,
                    EventHook hook);
    void RegisterMcpTools(const char* product);

private:
    struct Status {
        bool human = false;
        bool accelerometer = false;
        bool angles_valid = false;
        int x_mg = 0;
        int y_mg = 0;
        int z_mg = 0;
        float pitch = 0;
        float roll = 0;
        int orientation = -1;
    };
    std::mutex mutex_;
    Status status_;
    gpio_num_t human_ = GPIO_NUM_NC;
    gpio_num_t sda_ = GPIO_NUM_NC, scl_ = GPIO_NUM_NC;
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
    ServoOutput* output_ = nullptr;
    EventHook hook_;
    bool InitializeAccelerometer(gpio_num_t sda, gpio_num_t scl);
    bool Read(uint8_t reg, uint8_t* data, size_t size);
    bool Write(uint8_t reg, uint8_t value);
    void Task();
};

}  // namespace wzh
#endif  // WZH_SENSORS_H_
