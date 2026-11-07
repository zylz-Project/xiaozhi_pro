#include "config.h"

#include "wzh_product.h"

namespace wzh {

const ProductConfig& Product() {
    static const ProductConfig config = {
        .product_name = "tail_red_panda",
        .ap_prefix = PANDA_AP_PREFIX,
        .hub_product_id = "tail-wagging-panda",
        .flash_host = SPI2_HOST,
        .flash_cs_pin = PANDA_FLASH_CS_PIN,
        .flash_clk_pin = PANDA_FLASH_CLK_PIN,
        .flash_mosi_pin = PANDA_FLASH_MOSI_PIN,
        .flash_miso_pin = PANDA_FLASH_MISO_PIN,
    };
    return config;
}

}  // namespace wzh
