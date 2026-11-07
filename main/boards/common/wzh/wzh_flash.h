#ifndef WZH_FLASH_H_
#define WZH_FLASH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WZH_FLASH_UNKNOWN = 0,
    WZH_FLASH_NOR_W25Q256,    // SPI NOR，4KB 扇区 / 256B 页
    WZH_FLASH_NAND_W25N01GV,  // SPI NAND，128KB 块 / 2KB 页（用户区 1000 块）
} wzh_flash_type_t;

typedef struct {
    spi_host_device_t host;
    gpio_num_t cs_pin;
    gpio_num_t clk_pin;
    gpio_num_t mosi_pin;
    gpio_num_t miso_pin;
} wzh_flash_pins_t;

/**
 * @brief 初始化 SPI 总线并通过 JEDEC ID 自动探测外部音频 Flash，
 *        随后完成型号相关的配置（NAND 保护位/ECC/BBM，NOR 解除深掉电）。
 *
 * Kconfig 可强制指定型号（CONFIG_WZH_FLASH_FORCE_NAND/_NOR）；强制型号与
 * 实际芯片不符时初始化失败并打印 JEDEC 日志。
 */
esp_err_t wzh_flash_init(const wzh_flash_pins_t* pins);
void wzh_flash_deinit(void);

wzh_flash_type_t wzh_flash_type(void);
const char* wzh_flash_type_name(void);
/** 芯片可正常使用的容量（NAND 为扣除 BBM 保留块后的用户区大小）。 */
uint64_t wzh_flash_capacity(void);
uint32_t wzh_flash_erase_size(void);
uint32_t wzh_flash_program_size(void);
const uint8_t* wzh_flash_jedec_id(void);  // 3 字节，init 后有效

esp_err_t wzh_flash_read(uint32_t addr, void* buf, size_t len);
esp_err_t wzh_flash_write(uint32_t addr, const uint8_t* buf, size_t len);
/** 擦除 addr 所在的一个擦除单元（NOR 4KB 扇区 / NAND 128KB 块）。 */
esp_err_t wzh_flash_erase_unit(uint32_t addr);
/** 擦除全部用户区（NOR 整片擦除；NAND 逐块并跳过坏块）。耗时可达数十秒。 */
esp_err_t wzh_flash_erase_all(void);
/** NAND 坏块查询；NOR 恒为好块。ok==false 时该单元不可读写。 */
esp_err_t wzh_flash_unit_ok(uint32_t addr, bool* ok);

#ifdef __cplusplus
}
#endif

#endif  // WZH_FLASH_H_
