/**
 * @file wzh_flash.c
 * @brief WZH 外部音频 Flash 统一入口：总线管理 + JEDEC 自动探测 + 型号分发。
 *
 * 三款产品默认搭载 W25N01GV SPI NAND；个别批次可能换用 W25Q256 级 NOR。
 * 以真机 JEDEC ID 为准：
 *   NAND：EF AA 21（命令后先输出一个哑字节）
 *   NOR ：EF 40 xx / EF 60 xx（命令后立即输出）
 */

#include <sdkconfig.h>

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>

#include "w25n01gv.h"
#include "w25q256.h"
#include "wzh_flash.h"
#include "wzh_flash_internal.h"

static const char* TAG = "wzh_flash";

#define WZH_SPI_CLK_HZ 40000000
#define WZH_SPI_MAX_TRANSFER 4096
#define WZH_JEDEC_CMD 0x9F

static SemaphoreHandle_t s_mutex = NULL;
static spi_device_handle_t s_dev = NULL;
static spi_host_device_t s_host = SPI2_HOST;
static gpio_num_t s_cs_pin = GPIO_NUM_NC;
static wzh_flash_type_t s_type = WZH_FLASH_UNKNOWN;
static uint8_t s_jedec[3] = {0};

bool wzh_spi_lock(void)
{
    return s_mutex && xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY) == pdTRUE;
}

void wzh_spi_unlock(void)
{
    xSemaphoreGiveRecursive(s_mutex);
}

void wzh_cs_low(void)
{
    gpio_set_level(s_cs_pin, 0);
}

void wzh_cs_high(void)
{
    gpio_set_level(s_cs_pin, 1);
}

spi_device_handle_t wzh_spi_device(void)
{
    return s_dev;
}

esp_err_t wzh_spi_transmit(const void* tx, size_t len, void* rx, size_t rx_len)
{
    if (rx == NULL) {
        spi_transaction_t t = {
            .length = len * 8,
            .tx_buffer = tx,
        };
        return spi_device_polling_transmit(s_dev, &t);
    }
    spi_transaction_t t = {
        .length = len * 8,
        .rxlength = rx_len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_dev, &t);
}

esp_err_t wzh_spi_tx(const void* data, size_t len)
{
    return wzh_spi_transmit(data, len, NULL, 0);
}

esp_err_t wzh_spi_rx(void* data, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .rxlength = len * 8,
        .rx_buffer = data,
    };
    return spi_device_polling_transmit(s_dev, &t);
}

esp_err_t wzh_spi_setup(const wzh_flash_pins_t* pins)
{
    s_cs_pin = pins->cs_pin;
    s_host = pins->host;

    gpio_config_t cs_cfg = {
        .pin_bit_mask = 1ULL << pins->cs_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cs_cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    wzh_cs_high();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = pins->mosi_pin,
        .miso_io_num = pins->miso_pin,
        .sclk_io_num = pins->clk_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = WZH_SPI_MAX_TRANSFER + 8,
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
    };
    ret = spi_bus_initialize(pins->host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = WZH_SPI_CLK_HZ,
        .spics_io_num = -1,  // 手动 CS，读/写序列跨越多个事务
        .queue_size = 4,
    };
    ret = spi_bus_add_device(pins->host, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        spi_bus_free(pins->host);
        return ret;
    }
    return ESP_OK;
}

void wzh_spi_teardown(void)
{
    if (s_dev != NULL) {
        spi_bus_remove_device(s_dev);
        s_dev = NULL;
    }
    spi_bus_free(s_host);
}

/** 全双工读 5 字节：NOR 在命令后立即输出（rx[1..3]），NAND 先输出一个哑字节（rx[2..4]）。 */
static esp_err_t probe_jedec(uint8_t* rx5)
{
    if (!wzh_spi_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t tx[5] = {WZH_JEDEC_CMD, 0, 0, 0, 0};
    wzh_cs_low();
    esp_err_t ret = wzh_spi_transmit(tx, sizeof(tx), rx5, sizeof(tx));
    wzh_cs_high();
    wzh_spi_unlock();
    return ret;
}

esp_err_t wzh_flash_init(const wzh_flash_pins_t* pins)
{
    if (s_type != WZH_FLASH_UNKNOWN) {
        return ESP_OK;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = wzh_spi_setup(pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t rx[5] = {0};
    ret = probe_jedec(rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JEDEC ID 读取失败: %s", esp_err_to_name(ret));
        wzh_spi_teardown();
        return ret;
    }

#if CONFIG_WZH_FLASH_FORCE_NAND
    bool nand = true;
    bool nor = false;
#elif CONFIG_WZH_FLASH_FORCE_NOR
    bool nand = false;
    bool nor = true;
#else
    bool nand = (rx[2] == 0xEF && rx[3] == 0xAA && rx[4] == 0x21);
    bool nor = (rx[1] == 0xEF && rx[2] != 0xAA);
#endif
    if (nand) {
        s_jedec[0] = rx[2];
        s_jedec[1] = rx[3];
        s_jedec[2] = rx[4];
    } else if (nor) {
        s_jedec[0] = rx[1];
        s_jedec[1] = rx[2];
        s_jedec[2] = rx[3];
    } else {
        ESP_LOGE(TAG, "未知 Flash 芯片，JEDEC 原始值: %02X %02X %02X %02X %02X",
                 rx[0], rx[1], rx[2], rx[3], rx[4]);
        wzh_spi_teardown();
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "JEDEC ID: %02X %02X %02X", s_jedec[0], s_jedec[1], s_jedec[2]);

    if (nand) {
        ret = w25n01gv_attach();
        if (ret != ESP_OK) {
            wzh_spi_teardown();
            return ret;
        }
        s_type = WZH_FLASH_NAND_W25N01GV;
        ESP_LOGI(TAG, "检测到 W25N01GV SPI NAND，用户区容量 %llu MiB，擦除粒度 128 KiB",
                 (unsigned long long)(w25n01gv_get_capacity() / 1024 / 1024));
    } else {
        ret = w25q256_attach();
        if (ret != ESP_OK) {
            wzh_spi_teardown();
            return ret;
        }
        s_type = WZH_FLASH_NOR_W25Q256;
        ESP_LOGI(TAG, "检测到 W25Q 系 SPI NOR，容量 %llu MiB，擦除粒度 4 KiB",
                 (unsigned long long)(w25q256_get_capacity() / 1024 / 1024));
    }
    return ESP_OK;
}

void wzh_flash_deinit(void)
{
    if (s_type == WZH_FLASH_UNKNOWN) {
        return;
    }
    if (wzh_spi_lock()) {
        wzh_cs_high();
        wzh_spi_teardown();
        s_type = WZH_FLASH_UNKNOWN;
        wzh_spi_unlock();
    }
}

wzh_flash_type_t wzh_flash_type(void)
{
    return s_type;
}

const char* wzh_flash_type_name(void)
{
    switch (s_type) {
        case WZH_FLASH_NOR_W25Q256:
            return "W25Q256 (SPI NOR)";
        case WZH_FLASH_NAND_W25N01GV:
            return "W25N01GV (SPI NAND)";
        default:
            return "unknown";
    }
}

uint64_t wzh_flash_capacity(void)
{
    if (s_type == WZH_FLASH_NAND_W25N01GV) {
        return w25n01gv_get_capacity();
    }
    if (s_type == WZH_FLASH_NOR_W25Q256) {
        return w25q256_get_capacity();
    }
    return 0;
}

uint32_t wzh_flash_erase_size(void)
{
    return (s_type == WZH_FLASH_NAND_W25N01GV) ? W25N01GV_BLOCK_SIZE : W25Q256_SECTOR_SIZE;
}

uint32_t wzh_flash_program_size(void)
{
    return (s_type == WZH_FLASH_NAND_W25N01GV) ? W25N01GV_PAGE_SIZE : W25Q256_PAGE_SIZE;
}

const uint8_t* wzh_flash_jedec_id(void)
{
    return s_jedec;
}

esp_err_t wzh_flash_read(uint32_t addr, void* buf, size_t len)
{
    if (s_type == WZH_FLASH_NAND_W25N01GV) {
        return w25n01gv_read(addr, (uint8_t*)buf, len);
    }
    if (s_type == WZH_FLASH_NOR_W25Q256) {
        return w25q256_read(addr, (uint8_t*)buf, len);
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t wzh_flash_write(uint32_t addr, const uint8_t* buf, size_t len)
{
    if (s_type == WZH_FLASH_NAND_W25N01GV) {
        return w25n01gv_write(addr, buf, len);
    }
    if (s_type == WZH_FLASH_NOR_W25Q256) {
        return w25q256_write(addr, buf, len);
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t wzh_flash_erase_unit(uint32_t addr)
{
    if (s_type == WZH_FLASH_NAND_W25N01GV) {
        return w25n01gv_erase_block(addr);
    }
    if (s_type == WZH_FLASH_NOR_W25Q256) {
        return w25q256_erase_sector(addr);
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t wzh_flash_erase_all(void)
{
    if (s_type == WZH_FLASH_NAND_W25N01GV) {
        return w25n01gv_erase_chip();
    }
    if (s_type == WZH_FLASH_NOR_W25Q256) {
        return w25q256_erase_chip();
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t wzh_flash_unit_ok(uint32_t addr, bool* ok)
{
    if (ok == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_type == WZH_FLASH_NAND_W25N01GV) {
        uint32_t block = addr / W25N01GV_BLOCK_SIZE;
        return w25n01gv_is_bad_block(block, ok);
    }
    *ok = true;
    return ESP_OK;
}
