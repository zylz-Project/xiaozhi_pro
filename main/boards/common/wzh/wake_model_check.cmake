# Prevent a stale menuconfig model from silently pairing another WZH product's
# wake phrase with the selected board. Other custom wake models remain valid.
if(CONFIG_USE_AFE_WAKE_WORD AND NOT CONFIG_WZH_ALLOW_CROSS_PRODUCT_WAKE_MODEL)
    if(CONFIG_BOARD_TYPE_WZH_CRAWLPANDA AND
       (CONFIG_SR_WN_WN9L_NI3HAO3XING1BAO3_TTS3 OR CONFIG_SR_WN_WN9_XIAOLONGXIAOLONG_TTS))
        message(FATAL_ERROR "WZH wake model mismatch: crawl panda expects 你好小安; remove the other WZH wake model or explicitly allow a cross-product phrase.")
    elseif(CONFIG_BOARD_TYPE_WZH_TAILREDPANDA AND
           (CONFIG_SR_WN_WN9_NIHAOXIAOAN_TTS2 OR CONFIG_SR_WN_WN9_XIAOLONGXIAOLONG_TTS))
        message(FATAL_ERROR "WZH wake model mismatch: red panda expects 你好星宝; remove the other WZH wake model or explicitly allow a cross-product phrase.")
    elseif(CONFIG_BOARD_TYPE_WZH_DINOSAUR AND
           (CONFIG_SR_WN_WN9_NIHAOXIAOAN_TTS2 OR CONFIG_SR_WN_WN9L_NI3HAO3XING1BAO3_TTS3))
        message(FATAL_ERROR "WZH wake model mismatch: dinosaur expects 小龙小龙; remove the other WZH wake model or explicitly allow a cross-product phrase.")
    endif()
endif()
