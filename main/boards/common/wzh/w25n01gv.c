/**
 * @file w25n01gv.c
 * @brief W25N01GVZEIG SPI NAND driver for ESP32-S3 (wzh shared layer).
 *
 * Ported from the board-level driver (proven on real hardware); bus setup,
 * manual CS and the recursive lock now live in wzh_flash.c.
 */

#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "w25n01gv.h"
#include "wzh_flash_internal.h"

static const char *TAG = "w25n01gv";

#define W25N_READ_TIMEOUT_MS       10U
#define W25N_PROGRAM_TIMEOUT_MS    20U
#define W25N_ERASE_TIMEOUT_MS     100U
#define W25N_RESET_TIMEOUT_MS    1000U

/* 0 = unknown, 1 = good, -1 = bad. */
static int8_t g_block_state[W25N01GV_PHYSICAL_BLOCK_COUNT];

static esp_err_t send_cmd(uint8_t cmd)
{
    wzh_cs_low();
    esp_err_t ret = wzh_spi_tx(&cmd, 1);
    wzh_cs_high();
    return ret;
}

static esp_err_t read_register(uint8_t reg, uint8_t *value)
{
    if (!value) return ESP_ERR_INVALID_ARG;
    uint8_t tx[3] = {W25N01GV_CMD_READ_STATUS, reg, 0x00};
    uint8_t rx[3] = {0};
    wzh_cs_low();
    esp_err_t ret = wzh_spi_transmit(tx, sizeof(tx), rx, sizeof(rx));
    wzh_cs_high();
    if (ret == ESP_OK) *value = rx[2];
    return ret;
}

static esp_err_t write_enable(void)
{
    esp_err_t ret = send_cmd(W25N01GV_CMD_WRITE_ENABLE);
    if (ret != ESP_OK) return ret;
    uint8_t status = 0;
    ret = read_register(W25N01GV_REG_STATUS, &status);
    return (ret == ESP_OK && (status & W25N01GV_STATUS_WEL)) ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    esp_err_t ret = write_enable();
    if (ret != ESP_OK) return ret;
    uint8_t tx[3] = {W25N01GV_CMD_WRITE_STATUS, reg, value};
    wzh_cs_low();
    ret = wzh_spi_tx(tx, sizeof(tx));
    wzh_cs_high();
    if (ret != ESP_OK) return ret;
    return w25n01gv_wait_busy(10) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t send_row_command(uint8_t cmd, uint16_t page)
{
    uint8_t tx[4] = {cmd, 0x00, (uint8_t)(page >> 8), (uint8_t)page};
    wzh_cs_low();
    esp_err_t ret = wzh_spi_tx(tx, sizeof(tx));
    wzh_cs_high();
    return ret;
}

static esp_err_t page_to_cache(uint16_t page, uint8_t *status)
{
    esp_err_t ret = send_row_command(W25N01GV_CMD_PAGE_DATA_READ, page);
    if (ret != ESP_OK) return ret;
    if (!w25n01gv_wait_busy(W25N_READ_TIMEOUT_MS)) return ESP_ERR_TIMEOUT;
    return read_register(W25N01GV_REG_STATUS, status);
}

static esp_err_t cache_read(uint16_t column, void *buf, size_t len)
{
    uint8_t header[4] = {
        W25N01GV_CMD_READ_DATA, (uint8_t)(column >> 8), (uint8_t)column, 0x00
    };
    wzh_cs_low();
    esp_err_t ret = wzh_spi_tx(header, sizeof(header));
    if (ret == ESP_OK) ret = wzh_spi_rx(buf, len);
    wzh_cs_high();
    return ret;
}

static esp_err_t log_bbm_lut(void)
{
    uint8_t raw[20 * 4] = {0};
    uint8_t header[2] = {W25N01GV_CMD_READ_BBM_LUT, 0x00};

    wzh_cs_low();
    esp_err_t ret = wzh_spi_tx(header, sizeof(header));
    if (ret == ESP_OK) ret = wzh_spi_rx(raw, sizeof(raw));
    wzh_cs_high();
    if (ret != ESP_OK) return ret;

    unsigned active = 0;
    for (unsigned i = 0; i < 20; ++i) {
        const uint8_t *entry = &raw[i * 4];
        bool enabled = (entry[0] & 0x80U) != 0;
        bool invalid = (entry[0] & 0x40U) != 0;
        if (!enabled) continue;

        uint16_t lba = (uint16_t)(((entry[0] & 0x03U) << 8) | entry[1]);
        uint16_t pba = (uint16_t)((entry[2] << 8) | entry[3]);
        ESP_LOGI(TAG, "BBM LUT[%u]: LBA=%u -> PBA=%u (%s)",
                 i, lba, pba, invalid ? "已失效" : "有效");
        if (!invalid) ++active;
    }
    ESP_LOGI(TAG, "BBM LUT: %u/20 个有效映射", active);
    return ESP_OK;
}

esp_err_t w25n01gv_attach(void)
{
    memset(g_block_state, 0, sizeof(g_block_state));

    esp_err_t ret = send_cmd(W25N01GV_CMD_RESET);
    if (ret != ESP_OK || !w25n01gv_wait_busy(W25N_RESET_TIMEOUT_MS)) {
        return (ret == ESP_OK) ? ESP_ERR_TIMEOUT : ret;
    }

    uint8_t mid = 0, did_hi = 0, did_lo = 0;
    ret = w25n01gv_read_jedec_id(&mid, &did_hi, &did_lo);
    if (ret != ESP_OK || mid != 0xEF || did_hi != 0xAA || did_lo != 0x21) {
        ESP_LOGE(TAG, "JEDEC ID 不匹配: %02X %02X %02X (期望 EF AA 21)",
                 mid, did_hi, did_lo);
        return ESP_ERR_NOT_FOUND;
    }

    /* Power-up defaults protect the whole array. Clear BP/TB and verify it. */
    uint8_t protection = 0, configuration = 0;
    ret = read_register(W25N01GV_REG_PROTECTION, &protection);
    if (ret != ESP_OK) return ret;
    uint8_t unprotected = protection & (uint8_t)~0x7C;
    if (unprotected != protection) {
        ret = write_register(W25N01GV_REG_PROTECTION, unprotected);
        if (ret != ESP_OK) return ret;
    }

    /* ZEIG powers up in buffer mode; force BUF=1 and internal ECC on. */
    ret = read_register(W25N01GV_REG_CONFIGURATION, &configuration);
    if (ret != ESP_OK) return ret;
    uint8_t desired_cfg = (configuration | W25N01GV_CFG_ECC_E | W25N01GV_CFG_BUF)
                          & (uint8_t)~W25N01GV_CFG_OTP_E;
    if (desired_cfg != configuration) {
        ret = write_register(W25N01GV_REG_CONFIGURATION, desired_cfg);
        if (ret != ESP_OK) return ret;
    }

    ret = read_register(W25N01GV_REG_PROTECTION, &protection);
    if (ret != ESP_OK || (protection & 0x7C) != 0) {
        ESP_LOGE(TAG, "无法解除阵列保护, Protection=0x%02X", protection);
        return ESP_ERR_INVALID_STATE;
    }

    ret = log_bbm_lut();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取 BBM LUT 失败: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "W25N01GVZEIG 初始化成功, ECC 已启用");
    ESP_LOGI(TAG, "块布局: 用户块 0~999, BBM 保留块 1000~1023 (每块 128 KiB)");
    return ESP_OK;
}

esp_err_t w25n01gv_read_jedec_id(uint8_t *manufacturer_id,
                                  uint8_t *device_id_high,
                                  uint8_t *device_id_low)
{
    if (!manufacturer_id || !device_id_high || !device_id_low) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;
    /* 9Fh, one dummy byte, then EFh AAh 21h. */
    uint8_t tx[5] = {W25N01GV_CMD_JEDEC_ID, 0, 0, 0, 0};
    uint8_t rx[5] = {0};
    wzh_cs_low();
    esp_err_t ret = wzh_spi_transmit(tx, sizeof(tx), rx, sizeof(rx));
    wzh_cs_high();
    if (ret == ESP_OK) {
        *manufacturer_id = rx[2];
        *device_id_high = rx[3];
        *device_id_low = rx[4];
    }
    wzh_spi_unlock();
    return ret;
}

bool w25n01gv_wait_busy(uint32_t timeout_ms)
{
    if (!wzh_spi_device()) return false;
    bool locked_here = false;
    if (!wzh_spi_lock()) return false;
    locked_here = true;
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    bool idle = false;
    do {
        uint8_t status = 0;
        if (read_register(W25N01GV_REG_STATUS, &status) == ESP_OK &&
            !(status & W25N01GV_STATUS_BUSY)) {
            idle = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while ((xTaskGetTickCount() - start) <= timeout);
    if (locked_here) wzh_spi_unlock();
    return idle;
}

esp_err_t w25n01gv_read_status(uint8_t *protection,
                                uint8_t *configuration,
                                uint8_t *status)
{
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;
    esp_err_t ret;
    if (protection && (ret = read_register(W25N01GV_REG_PROTECTION, protection)) != ESP_OK) goto out;
    if (configuration && (ret = read_register(W25N01GV_REG_CONFIGURATION, configuration)) != ESP_OK) goto out;
    if (status && (ret = read_register(W25N01GV_REG_STATUS, status)) != ESP_OK) goto out;
    ret = ESP_OK;
out:
    wzh_spi_unlock();
    return ret;
}

esp_err_t w25n01gv_is_bad_block(uint32_t block_index, bool *is_bad)
{
    if (!is_bad || block_index >= W25N01GV_PHYSICAL_BLOCK_COUNT) return ESP_ERR_INVALID_ARG;
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;
    if (g_block_state[block_index] != 0) {
        *is_bad = g_block_state[block_index] < 0;
        wzh_spi_unlock();
        return ESP_OK;
    }

    uint8_t status = 0;
    uint8_t spare_marker = 0;
    uint16_t first_page = (uint16_t)(block_index * W25N01GV_PAGES_PER_BLOCK);
    esp_err_t ret = page_to_cache(first_page, &status);
    if (ret != ESP_OK) goto out;

    /*
     * The factory bad-block marker lives in byte 0 of the first page's spare
     * area. Main-array byte 0 is user data and must not be checked here.
     */
    ret = cache_read(W25N01GV_PAGE_SIZE, &spare_marker, 1);
    if (ret != ESP_OK) goto out;
    *is_bad = spare_marker != 0xFF;

    if (*is_bad) {
        ESP_LOGW(TAG, "坏块 %lu: Spare[0]=0x%02X",
                 (unsigned long)block_index, spare_marker);
    }

    g_block_state[block_index] = *is_bad ? -1 : 1;
out:
    wzh_spi_unlock();
    return ret;
}

esp_err_t w25n01gv_read(uint32_t addr, uint8_t *buf, size_t len)
{
    if ((!buf && len)) return ESP_ERR_INVALID_ARG;
    if (addr >= W25N01GV_TOTAL_SIZE || len > W25N01GV_TOTAL_SIZE - addr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) return ESP_OK;
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;

    size_t done = 0;
    esp_err_t ret = ESP_OK;
    while (done < len) {
        uint32_t current = addr + done;
        uint16_t page = (uint16_t)(current / W25N01GV_PAGE_SIZE);
        uint16_t column = (uint16_t)(current % W25N01GV_PAGE_SIZE);
        size_t chunk = W25N01GV_PAGE_SIZE - column;
        if (chunk > len - done) chunk = len - done;

        bool bad = false;
        ret = w25n01gv_is_bad_block(page / W25N01GV_PAGES_PER_BLOCK, &bad);
        if (ret != ESP_OK) goto out;
        if (bad) {
            ESP_LOGE(TAG, "拒绝读取坏块 %u", page / W25N01GV_PAGES_PER_BLOCK);
            ret = ESP_ERR_INVALID_STATE;
            goto out;
        }

        uint8_t status = 0;
        ret = page_to_cache(page, &status);
        if (ret != ESP_OK) goto out;
        uint8_t ecc = status & W25N01GV_STATUS_ECC_MASK;
        if (ecc == W25N01GV_STATUS_ECC_UNCORRECTABLE || ecc == W25N01GV_STATUS_ECC_MASK) {
            ESP_LOGE(TAG, "页 %u 存在不可纠正 ECC 错误, Status=0x%02X", page, status);
            ret = ESP_ERR_INVALID_CRC;
            goto out;
        }
        if (ecc == W25N01GV_STATUS_ECC_CORRECTED) {
            ESP_LOGW(TAG, "页 %u 已由片内 ECC 修正", page);
        }
        ret = cache_read(column, buf + done, chunk);
        if (ret != ESP_OK) goto out;
        done += chunk;
    }
out:
    wzh_spi_unlock();
    return ret;
}

esp_err_t w25n01gv_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    if ((!buf && len)) return ESP_ERR_INVALID_ARG;
    if (addr >= W25N01GV_TOTAL_SIZE || len > W25N01GV_TOTAL_SIZE - addr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) return ESP_OK;
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;

    /*
     * Always program a complete 2KB image. With ECC enabled, programming only
     * a later fragment would calculate parity against FF in the rest of the
     * cache instead of against bytes already present in the NAND page.
     */
    uint8_t *page_buf = heap_caps_malloc(W25N01GV_PAGE_SIZE, MALLOC_CAP_8BIT);
    if (!page_buf) {
        wzh_spi_unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t done = 0;
    esp_err_t ret = ESP_OK;
    while (done < len) {
        uint32_t current = addr + done;
        uint16_t page = (uint16_t)(current / W25N01GV_PAGE_SIZE);
        uint16_t column = (uint16_t)(current % W25N01GV_PAGE_SIZE);
        size_t chunk = W25N01GV_PAGE_SIZE - column;
        if (chunk > len - done) chunk = len - done;

        bool bad = false;
        ret = w25n01gv_is_bad_block(page / W25N01GV_PAGES_PER_BLOCK, &bad);
        if (ret != ESP_OK) goto out;
        if (bad) {
            ret = ESP_ERR_INVALID_STATE;
            goto out;
        }

        /*
         * Audio/TOC writers erase the containing block before starting a new
         * page.  Do not read such a page first: an erased NAND page has no
         * programmed ECC parity and some chips legitimately report ECC=10b
         * even though the data area is all 0xFF.  A continuation that starts
         * in the middle of a page must still preserve bytes programmed by the
         * preceding call, so only that case uses read-modify-write.
         */
        if (column == 0) {
            memset(page_buf, 0xFF, W25N01GV_PAGE_SIZE);
        } else {
            ret = w25n01gv_read((uint32_t)page * W25N01GV_PAGE_SIZE,
                                page_buf, W25N01GV_PAGE_SIZE);
            if (ret != ESP_OK) goto out;
        }
        for (size_t i = 0; i < chunk; ++i) {
            uint8_t old_value = page_buf[column + i];
            uint8_t new_value = buf[done + i];
            if (((uint8_t)~old_value & new_value) != 0) {
                ESP_LOGE(TAG, "地址 0x%08lX 需要 0->1，必须先擦除 128KB 块",
                         (unsigned long)(current + i));
                ret = ESP_ERR_INVALID_STATE;
                goto out;
            }
            page_buf[column + i] = new_value;
        }

        ret = write_enable();
        if (ret != ESP_OK) goto out;
        uint8_t header[3] = {W25N01GV_CMD_PROGRAM_LOAD, 0x00, 0x00};
        wzh_cs_low();
        ret = wzh_spi_tx(header, sizeof(header));
        if (ret == ESP_OK) ret = wzh_spi_tx(page_buf, W25N01GV_PAGE_SIZE);
        wzh_cs_high();
        if (ret != ESP_OK) goto out;

        ret = send_row_command(W25N01GV_CMD_PROGRAM_EXECUTE, page);
        if (ret != ESP_OK) goto out;
        if (!w25n01gv_wait_busy(W25N_PROGRAM_TIMEOUT_MS)) {
            ret = ESP_ERR_TIMEOUT;
            goto out;
        }
        uint8_t status = 0;
        ret = read_register(W25N01GV_REG_STATUS, &status);
        if (ret != ESP_OK) goto out;
        if (status & W25N01GV_STATUS_PFAIL) {
            ESP_LOGE(TAG, "页 %u 编程失败, Status=0x%02X", page, status);
            g_block_state[page / W25N01GV_PAGES_PER_BLOCK] = -1;
            ret = ESP_FAIL;
            goto out;
        }
        done += chunk;
    }

out:
    free(page_buf);
    wzh_spi_unlock();
    return ret;
}

esp_err_t w25n01gv_erase_block(uint32_t addr)
{
    if (addr >= W25N01GV_TOTAL_SIZE) return ESP_ERR_INVALID_ARG;
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;
    uint32_t block = addr / W25N01GV_BLOCK_SIZE;
    bool bad = false;
    esp_err_t ret = w25n01gv_is_bad_block(block, &bad);
    if (ret != ESP_OK) goto out;
    if (bad) {
        ESP_LOGW(TAG, "跳过坏块 %lu", (unsigned long)block);
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }

    ret = write_enable();
    if (ret != ESP_OK) goto out;
    uint16_t page = (uint16_t)(block * W25N01GV_PAGES_PER_BLOCK);
    ret = send_row_command(W25N01GV_CMD_BLOCK_ERASE, page);
    if (ret != ESP_OK) goto out;
    if (!w25n01gv_wait_busy(W25N_ERASE_TIMEOUT_MS)) { ret = ESP_ERR_TIMEOUT; goto out; }

    uint8_t status = 0;
    ret = read_register(W25N01GV_REG_STATUS, &status);
    if (ret != ESP_OK) goto out;
    if (status & W25N01GV_STATUS_EFAIL) {
        ESP_LOGE(TAG, "块 %lu 擦除失败, Status=0x%02X", (unsigned long)block, status);
        g_block_state[block] = -1;
        ret = ESP_FAIL;
        goto out;
    }
    ret = ESP_OK;
out:
    wzh_spi_unlock();
    return ret;
}

esp_err_t w25n01gv_erase_chip(void)
{
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "开始擦除 1000 个用户块（保留物理块 1000~1023）");
    unsigned skipped = 0;
    for (uint32_t block = 0; block < W25N01GV_USER_BLOCK_COUNT; ++block) {
        esp_err_t ret = w25n01gv_erase_block(block * W25N01GV_BLOCK_SIZE);
        if (ret == ESP_ERR_INVALID_STATE) {
            ++skipped;
            continue;
        }
        if (ret != ESP_OK) { wzh_spi_unlock(); return ret; }
        if ((block + 1) % 64 == 0) {
            ESP_LOGI(TAG, "用户区擦除进度: %lu/%u", (unsigned long)(block + 1), W25N01GV_USER_BLOCK_COUNT);
        }
    }
    ESP_LOGI(TAG, "用户区擦除完成, 跳过 %u 个坏块, 24 个 BBM 保留块未触碰", skipped);
    wzh_spi_unlock();
    return ESP_OK;
}

uint64_t w25n01gv_get_capacity(void)
{
    return W25N01GV_TOTAL_SIZE;
}

esp_err_t w25n01gv_read_unique_id(uint8_t uid[8])
{
    if (!uid) return ESP_ERR_INVALID_ARG;
    if (!wzh_spi_lock()) return ESP_ERR_INVALID_STATE;
    uint8_t cfg = 0;
    esp_err_t ret = read_register(W25N01GV_REG_CONFIGURATION, &cfg);
    if (ret != ESP_OK) { wzh_spi_unlock(); return ret; }
    uint8_t otp_cfg = cfg | W25N01GV_CFG_OTP_E | W25N01GV_CFG_BUF | W25N01GV_CFG_ECC_E;
    ret = write_register(W25N01GV_REG_CONFIGURATION, otp_cfg);
    if (ret == ESP_OK) {
        uint8_t status = 0;
        ret = page_to_cache(0, &status);
        if (ret == ESP_OK) ret = cache_read(0, uid, 8);
    }
    esp_err_t restore_ret = write_register(W25N01GV_REG_CONFIGURATION, cfg);
    wzh_spi_unlock();
    return ret != ESP_OK ? ret : restore_ret;
}
