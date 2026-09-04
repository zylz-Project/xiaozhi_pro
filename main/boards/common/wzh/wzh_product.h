#ifndef WZH_PRODUCT_H_
#define WZH_PRODUCT_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>

namespace wzh {

// 三个 WZH 产品共享基础层的单一配置入口。每个板型在自己的目录里提供
// wzh::Product() 的实现（板级 wzh_product.cc），公共代码只依赖这个接口，
// 不直接包含任何板级 config.h。
struct ProductConfig {
    // MCP 命名空间用：crawl_panda / dinosaur / tail_red_panda
    const char* product_name;
    // 配网热点前缀（"CrawlPanda" / "Dino" / "Panda"）
    const char* ap_prefix;
    // Audio Hub 产品 ID（crawling-panda / dinosaur / tail-wagging-panda）
    const char* hub_product_id;
    // 外部音频 Flash（三款硬件引脚一致，取值来自板级 config.h）
    spi_host_device_t flash_host;
    gpio_num_t flash_cs_pin;
    gpio_num_t flash_clk_pin;
    gpio_num_t flash_mosi_pin;
    gpio_num_t flash_miso_pin;
};

const ProductConfig& Product();

}  // namespace wzh

#endif  // WZH_PRODUCT_H_
