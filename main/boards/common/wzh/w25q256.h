#pragma once

/**
 * @file w25q256.h
 * @brief Winbond W25Q 系列 SPI NOR driver (wzh shared layer).
 *
 * 容量以 JEDEC 第三字节查表为准（W25Q256 = 0x19 = 256 Mbit = 32 MiB）。
 * 参考工程头文件里 "64MB/512Mbit" 的注释是错的，本层不沿用。
 * Bus setup is owned by wzh_flash.c — call w25q256_attach() after
 * wzh_spi_setup() has run.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define W25Q256_PAGE_SIZE       256U      // 页编程粒度
#define W25Q256_SECTOR_SIZE     4096U     // 最小擦除单元
#define W25Q256_BLOCK_64K_SIZE  65536U

#define W25Q_CMD_WRITE_ENABLE       0x06
#define W25Q_CMD_READ_SR1           0x05
#define W25Q_CMD_READ_DATA          0x03
#define W25Q_CMD_PAGE_PROGRAM       0x02
#define W25Q_CMD_SECTOR_ERASE       0x20    // 4KB
#define W25Q_CMD_CHIP_ERASE         0xC7
#define W25Q_CMD_READ_JEDEC_ID      0x9F

#define W25Q_SR1_BUSY    (1 << 0)
#define W25Q_SR1_WEL     (1 << 1)

/** 校验 JEDEC 并解析容量。总线必须已由 wzh_flash 初始化。 */
esp_err_t w25q256_attach(void);

esp_err_t w25q256_read_jedec_id(uint8_t *manufacturer_id,
                                uint8_t *memory_type,
                                uint8_t *capacity_id);
esp_err_t w25q256_read(uint32_t addr, uint8_t *buf, size_t len);
esp_err_t w25q256_write(uint32_t addr, const uint8_t *buf, size_t len);
esp_err_t w25q256_erase_sector(uint32_t addr);
esp_err_t w25q256_erase_chip(void);
bool w25q256_wait_busy(uint32_t timeout_ms);
uint64_t w25q256_get_capacity(void);

#ifdef __cplusplus
}
#endif
