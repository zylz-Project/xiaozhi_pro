#pragma once

/**
 * @file wzh_flash_internal.h
 * @brief wzh_flash.c 与两款芯片驱动共用的 SPI 底层设施。
 *
 * 总线、CS 引脚和递归锁由 wzh_flash.c 持有；w25q256/w25n01gv 驱动只通过
 * 这里的接口访问 SPI。所有公开的驱动函数必须持有锁后再操作总线。
 */

#include <stdbool.h>
#include <stddef.h>

#include <driver/spi_master.h>
#include <esp_err.h>

#include "wzh_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wzh_spi_setup(const wzh_flash_pins_t* pins);
void wzh_spi_teardown(void);
spi_device_handle_t wzh_spi_device(void);

bool wzh_spi_lock(void);
void wzh_spi_unlock(void);

void wzh_cs_low(void);
void wzh_cs_high(void);

/** 全双工同时收发（长度必须相等）。 */
esp_err_t wzh_spi_transmit(const void* tx, size_t len, void* rx, size_t rx_len);
esp_err_t wzh_spi_tx(const void* data, size_t len);
esp_err_t wzh_spi_rx(void* data, size_t len);

#ifdef __cplusplus
}
#endif
