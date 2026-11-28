#pragma once
#include "config.h"

inline bool DinosaurReservedPin(int pin) {
    // Internal flash/PSRAM, USB, strapping, and the existing dinosaur devices.
    if ((pin >= 26 && pin <= 37) || pin == 19 || pin == 20 || pin == 45 || pin == 46)
        return true;
    const int occupied[] = {
        BOOT_BUTTON_GPIO,
        POWER_CTRL_GPIO,
        POWER_BUTTON_GPIO,
        3,
        TOUCH_GPIO,
        AUDIO_I2S_GPIO_WS,
        AUDIO_I2S_GPIO_BCLK,
        AUDIO_I2S_GPIO_DOUT,
        AUDIO_I2S_GPIO_DIN,
        AUDIO_CODEC_I2C_SDA_PIN,
        AUDIO_CODEC_I2C_SCL_PIN,
        HUMAN_SENSOR_GPIO,
        POSTURE_I2C_SDA_GPIO,
        POSTURE_I2C_SCL_GPIO,
        SERVO_POWER_GPIO,
        SERVO_HEAD_TURN_GPIO,
        SERVO_NECK_LEAN_GPIO,
        SERVO_NECK_TILT_GPIO,
        SERVO_TAIL_LR_GPIO,
        SERVO_TAIL_UD_GPIO,
        DINO_FLASH_CS_PIN,
        DINO_FLASH_CLK_PIN,
        DINO_FLASH_MOSI_PIN,
        DINO_FLASH_MISO_PIN,
    };
    for (int used : occupied) {
        if (pin == used)
            return true;
    }
    return false;
}

inline bool DinosaurValidModemPins(int kTx, int kRx, int kDtr) {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(kTx) || !GPIO_IS_VALID_GPIO(kRx) || kTx == kRx ||
        DinosaurReservedPin(kTx) || DinosaurReservedPin(kRx))
        return false;
    return kDtr == -1 || (GPIO_IS_VALID_OUTPUT_GPIO(kDtr) && !DinosaurReservedPin(kDtr) &&
                          kDtr != kTx && kDtr != kRx);
}
