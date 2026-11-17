# A stale CONFIG_SR_WN_* left in sdkconfig by another product is reported, not
# fatal. The packaged wake model is pinned by the board in main/CMakeLists.txt,
# so the stale value cannot reach the firmware; failing here would instead lock
# the SDK configuration editor out of the very settings needed to fix it.
if(CONFIG_USE_AFE_WAKE_WORD AND NOT CONFIG_WZH_ALLOW_CROSS_PRODUCT_WAKE_MODEL)
    if(CONFIG_BOARD_TYPE_WZH_CRAWLPANDA AND
       (CONFIG_SR_WN_WN9L_NI3HAO3XING1BAO3_TTS3 OR CONFIG_SR_WN_WN9_XIAOLONGXIAOLONG_TTS))
        set(WZH_WAKE_MODEL_MISMATCH "crawl panda expects 你好小安")
    elseif(CONFIG_BOARD_TYPE_WZH_TAILREDPANDA AND
           (CONFIG_SR_WN_WN9_NIHAOXIAOAN_TTS2 OR CONFIG_SR_WN_WN9_XIAOLONGXIAOLONG_TTS))
        set(WZH_WAKE_MODEL_MISMATCH "red panda expects 你好星宝")
    elseif(CONFIG_BOARD_TYPE_WZH_DINOSAUR AND
           (CONFIG_SR_WN_WN9_NIHAOXIAOAN_TTS2 OR CONFIG_SR_WN_WN9L_NI3HAO3XING1BAO3_TTS3))
        set(WZH_WAKE_MODEL_MISMATCH "dinosaur expects 小龙小龙")
    endif()
    if(WZH_WAKE_MODEL_MISMATCH)
        message(WARNING
            "WZH wake model mismatch: ${WZH_WAKE_MODEL_MISMATCH}; another WZH "
            "product's model is still selected in sdkconfig. The build packages "
            "this board's own model regardless, so the firmware is correct.")
    endif()
endif()
