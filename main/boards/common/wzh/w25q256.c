/**
 * @file w25q256.c
 * @brief Winbond W25Q 系列 SPI NOR driver (wzh shared layer).
 *
 * Manual-CS full-duplex transactions, same style as the NAND driver.
 * The reference project's driver was reduced to the operations the audio
 * store needs: read, page program, 4KB sector erase, chip erase.
 */

#include <string.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "w25q256.h"
#include "wzh_flash_internal.h"

static const char *TAG = "w25q256";

#define W25Q_PROGRAM_TIMEOUT_MS  500U
#define W25Q_SECTOR_TIMEOUT_MS   500U
#define W25Q_CHIP_TIMEOUT_MS (400U * 1000U)

/* JEDEC 第三字节 → 容量（Mbit）。 */
static const struct { uint8_t id; uint32_t mbit; } kCapTable[] = {
    {0x15, 16},   /* W25Q16 */
    {0x16, 32},   /* W25Q32 */
    {0x17, 64},   /* W25Q64 */
    {0x18, 128},  /* W25Q128 */
    {0x19, 256},  /* W25Q256 */
    {0x20, 512},  /* 512Mbit 级 */
};

static uint64_t g_capacity_bytes = 0;

static esp_err_t read_sr1(uint8_t *sr1)
{
    uint8_t tx[2] = {W25Q_CMD_READ_SR1, 0x00};
    uint8_t rx[2] = {0};
    wzh_cs_low();
    esp_err_t ret = wzh_spi_transmit(tx, sizeof(tx), rx, sizeof(rx));
    wzh_cs_high();
    if (ret == ESP_OK) *sr1 = rx[1];
    return ret;
}

static esp_err_t write_enable(void)
{
    uint8_t cmd = W25Q_CMD_WRITE_ENABLE;
    wzh_cs_low();
    esp_err_t ret = wzh_spi_tx(&cmd, 1);
    wzh_cs_high();
    return ret;
}

esp_err_t w25q256_attach(void)
{
    uint8_t sr1 = 0;
    if (read_sr1(&sr1) != ESP_OK) {
        return ESP_FAIL;
    }
    if (sr1 & W25Q_SR1_BUSY) {
        ESP_LOGW(TAG, "上电后芯片仍忙，等待就绪");
        if (!w25q256_wait_busy(10000)) {
            return ESP_ERR_TIMEOUT;
        }
    }

    uint8_t mid = 0, mtype = 0, cap = 0;
    esp_err_t ret = w25q256_read_jedec_id(&mid, &mtype, &cap);
    if (ret != ESP_OK) {
        return ret;
    }
    if (mid != 0xEF) {
        ESP_LOGE(TAG, "非 Winbond 芯片: JEDEC %02X %02X %02X", mid, mtype, cap);
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t mbit = 0;
    for (size_t i = 0; i < sizeof(kCapTable) / sizeof(kCapTable[0]); ++i) {
        if (kCapTable[i].id == cap) {
            mbit = kCapTable[i].mbit;
            break;
        }
    }
    if (mbit == 0) {
        ESP_LOGE(TAG, "未知的容量码 0x%02X (JEDEC %02X %02X)", cap, mid, mtype);
        return ESP_ERR_NOT_SUPPORTED;
    }
    g_capacity_bytes = (uint64_t)mbit * 1024 * 1024 / 8;
    ESP_LOGI(TAG, "W25Q 系 NOR 就绪: %llu MiB, 4KB 扇区 / 256B 页",
             (unsigned long long)(g_capacity_bytes / 1024 / 1024));
    return ESP_OK;
}

esp_err_t w25q256_read_jedec_id(uint8_t *manufacturer_id,
                                uint8_t *memory_type,
                                uint8_t *capacity_id)
{
    if (!manufacturer_id || !memory_type || !capacity_id) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;
    /* 命令后立即输出 MF/Type/Cap（rx[0] 是命令期间的浮空字节，丢弃）。 */
    uint8_t tx[4] = {W25Q_CMD_READ_JEDEC_ID, 0, 0, 0};
    uint8_t rx[4] = {0};
    wzh_cs_low();
    esp_err_t ret = wzh_spi_transmit(tx, sizeof(tx), rx, sizeof(rx));
    wzh_cs_high();
    wzh_spi_unlock();
    if (ret == ESP_OK) {
        *manufacturer_id = rx[1];
        *memory_type = rx[2];
        *capacity_id = rx[3];
    }
    return ret;
}

bool w25q256_wait_busy(uint32_t timeout_ms)
{
    if (!wzh_spi_lock()) return false;
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    bool idle = false;
    do {
        uint8_t sr1 = 0;
        if (read_sr1(&sr1) == ESP_OK && !(sr1 & W25Q_SR1_BUSY)) {
            idle = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    } while ((xTaskGetTickCount() - start) <= timeout);
    wzh_spi_unlock();
    return idle;
}

esp_err_t w25q256_read(uint32_t addr, uint8_t *buf, size_t len)
{
    if (!buf && len) return ESP_ERR_INVALID_ARG;
    if (g_capacity_bytes == 0 || addr >= g_capacity_bytes ||
        len > g_capacity_bytes - addr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) return ESP_OK;
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;

    /* 0x03 读命令 + 24 位地址 + 连续数据必须保持在同一次 CS 拉低期间。 */
    uint8_t header[4] = {
        W25Q_CMD_READ_DATA,
        (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)((addr >> 8) & 0xFF),
        (uint8_t)(addr & 0xFF),
    };
    wzh_cs_low();
    esp_err_t ret = wzh_spi_tx(header, sizeof(header));
    size_t done = 0;
    while (ret == ESP_OK && done < len) {
        size_t chunk = len - done;
        if (chunk > 2048) chunk = 2048;
        ret = wzh_spi_rx(buf + done, chunk);
        done += chunk;
    }
    wzh_cs_high();
    wzh_spi_unlock();
    return ret;
}

esp_err_t w25q256_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    if (!buf && len) return ESP_ERR_INVALID_ARG;
    if (g_capacity_bytes == 0 || addr >= g_capacity_bytes ||
        len > g_capacity_bytes - addr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) return ESP_OK;
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_OK;
    size_t done = 0;
    while (done < len) {
        uint32_t current = addr + done;
        uint32_t page_end = (current & ~(W25Q256_PAGE_SIZE - 1)) + W25Q256_PAGE_SIZE;
        size_t chunk = page_end - current;
        if (chunk > len - done) chunk = len - done;

        ret = write_enable();
        if (ret != ESP_OK) break;

        uint8_t header[4] = {
            W25Q_CMD_PAGE_PROGRAM,
            (uint8_t)((current >> 16) & 0xFF),
            (uint8_t)((current >> 8) & 0xFF),
            (uint8_t)(current & 0xFF),
        };
        wzh_cs_low();
        ret = wzh_spi_tx(header, sizeof(header));
        if (ret == ESP_OK) ret = wzh_spi_tx(buf + done, chunk);
        wzh_cs_high();
        if (ret != ESP_OK) break;

        if (!w25q256_wait_busy(W25Q_PROGRAM_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "页编程超时 @ 0x%08lX", (unsigned long)current);
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        done += chunk;
    }
    wzh_spi_unlock();
    return ret;
}

static esp_err_t erase_op(uint8_t cmd, uint32_t addr, uint32_t timeout_ms)
{
    if (g_capacity_bytes == 0 || addr >= g_capacity_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = write_enable();
    if (ret == ESP_OK) {
        uint8_t tx[4] = {
            cmd,
            (uint8_t)((addr >> 16) & 0xFF),
            (uint8_t)((addr >> 8) & 0xFF),
            (uint8_t)(addr & 0xFF),
        };
        wzh_cs_low();
        ret = wzh_spi_tx(tx, sizeof(tx));
        wzh_cs_high();
    }
    if (ret == ESP_OK && !w25q256_wait_busy(timeout_ms)) {
        ret = ESP_ERR_TIMEOUT;
    }
    wzh_spi_unlock();
    return ret;
}

esp_err_t w25q256_erase_sector(uint32_t addr)
{
    addr &= ~(W25Q256_SECTOR_SIZE - 1);
    return erase_op(W25Q_CMD_SECTOR_ERASE, addr, W25Q_SECTOR_TIMEOUT_MS);
}

esp_err_t w25q256_erase_chip(void)
{
    ESP_LOGW(TAG, "开始整片擦除（可能需要 1~3 分钟）");
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = write_enable();
    if (ret == ESP_OK) {
        uint8_t cmd = W25Q_CMD_CHIP_ERASE;
        wzh_cs_low();
        ret = wzh_spi_tx(&cmd, 1);
        wzh_cs_high();
    }
    wzh_spi_unlock();
    if (ret != ESP_OK) {
        return ret;
    }
    return w25q256_wait_busy(W25Q_CHIP_TIMEOUT_MS) ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint64_t w25q256_get_capacity(void)
{
    return g_capacity_bytes;
}
