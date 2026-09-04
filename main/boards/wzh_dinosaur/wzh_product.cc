#include "config.h"

#include "wzh_product.h"

namespace wzh {

const ProductConfig& Product() {
    static const ProductConfig config = {
        .product_name = "dinosaur",
        .ap_prefix = PANDA_AP_PREFIX,
        .hub_product_id = "dinosaur",
        .flash_host = DINO_FLASH_HOST,
        .flash_cs_pin = DINO_FLASH_CS_PIN,
        .flash_clk_pin = DINO_FLASH_CLK_PIN,
        .flash_mosi_pin = DINO_FLASH_MOSI_PIN,
        .flash_miso_pin = DINO_FLASH_MISO_PIN,
    };
    return config;
}

}  // namespace wzh
