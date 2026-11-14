# Define source files
set(SOURCES "audio/audio_codec.cc"
            "audio/audio_debugger.cc"
            "audio/audio_service.cc"
            "audio/demuxer/ogg_demuxer.cc"
            "audio/codecs/no_audio_codec.cc"
            "audio/codecs/box_audio_codec.cc"
            "audio/codecs/es8311_audio_codec.cc"
            "audio/codecs/es8374_audio_codec.cc"
            "audio/codecs/es8388_audio_codec.cc"
            "audio/codecs/es8389_audio_codec.cc"
            "audio/codecs/dummy_audio_codec.cc"
            "led/single_led.cc"
            "led/circular_strip.cc"
            "led/gpio_led.cc"
            "display/display.cc"
            "display/text_glyph.cc"
            "display/lcd_display.cc"
            "display/oled_display.cc"
            "display/lvgl_display/lvgl_display.cc"
            "display/emote_display.cc"
            "display/lvgl_display/emoji_collection.cc"
            "display/lvgl_display/lvgl_theme.cc"
            "display/lvgl_display/lvgl_font.cc"
            "display/lvgl_display/dynamic_glyph_cache.cc"
            "display/lvgl_display/lvgl_image.cc"
            "display/lvgl_display/gif/lvgl_gif.cc"
            "display/lvgl_display/gif/gifdec.c"
            "display/lvgl_display/jpg/image_to_jpeg.cpp"
            "display/lvgl_display/jpg/jpeg_to_image.c"
            "protocols/protocol.cc"
            "protocols/text_glyph_payload.cc"
            "protocols/mqtt_protocol.cc"
            "protocols/websocket_protocol.cc"
            "mcp_server.cc"
            "system_info.cc"
            "application.cc"
            "ota.cc"
            "settings.cc"
            "device_state_machine.cc"
            "assets.cc"
            "main.cc"
            )

set(INCLUDE_DIRS "." "display" "display/lvgl_display" "display/lvgl_display/jpg" "audio" "audio/demuxer" "protocols")

# Add board common files
list(APPEND SOURCES
    "boards/common/board.cc"
    "boards/common/wifi_board.cc"
    "boards/common/ml307_board.cc"
    "boards/common/nt26_board.cc"
    "boards/common/dual_network_board.cc"
    "boards/common/adc_battery_monitor.cc"
    "boards/common/axp2101.cc"
    "boards/common/backlight.cc"
    "boards/common/button.cc"
    "boards/common/i2c_device.cc"
    "boards/common/knob.cc"
    "boards/common/power_save_timer.cc"
    "boards/common/press_to_talk_mcp_tool.cc"
    "boards/common/sleep_timer.cc"
    "boards/common/sy6970.cc"
    "boards/common/system_reset.cc"
)
if(CONFIG_XIAOZHI_NETWORK_ETHERNET)
    list(APPEND SOURCES "boards/common/ethernet_board.cc")
endif()
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common)


idf_build_get_property(build_components BUILD_COMPONENTS)
# Function to find component dynamically by pattern
function(find_component_by_pattern PATTERN COMPONENT_VAR PATH_VAR)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${PATTERN}")
            set(${COMPONENT_VAR} ${COMPONENT} PARENT_SCOPE)
            idf_component_get_property(COMPONENT_PATH ${COMPONENT} COMPONENT_DIR)
            set(${PATH_VAR} "${COMPONENT_PATH}" PARENT_SCOPE)
            break()
        endif()
    endforeach()
endfunction()

# Set default BUILTIN_TEXT_FONT and BUILTIN_ICON_FONT
set(BUILTIN_TEXT_FONT font_noto_sans_basic_14_1)
set(BUILTIN_ICON_FONT font_material_symbols_14_1)

set(EMOTE_RESOLUTION "320_240")

# Select the board source directory from Kconfig
# Set default assets if the board uses partition table V2
# WZH product migration: only the three WZH toys remain in this fork.
if(CONFIG_BOARD_TYPE_WZH_CRAWLPANDA)
    set(BOARD_DIR "wzh_crawlPanda")
    set(BUILTIN_TEXT_FONT font_noto_sans_basic_16_4)
    set(BUILTIN_ICON_FONT font_material_symbols_16_4)
    set(DEFAULT_EMOJI_COLLECTION noto-color-emoji_32)
elseif(CONFIG_BOARD_TYPE_WZH_DINOSAUR)
    set(BOARD_DIR "wzh_dinosaur")
    set(BUILTIN_TEXT_FONT font_noto_sans_basic_16_4)
    set(BUILTIN_ICON_FONT font_material_symbols_16_4)
    set(DEFAULT_EMOJI_COLLECTION noto-color-emoji_32)
elseif(CONFIG_BOARD_TYPE_WZH_TAILREDPANDA)
    set(BOARD_DIR "wzh_tailRedPanda")
    set(BUILTIN_TEXT_FONT font_noto_sans_basic_16_4)
    set(BUILTIN_ICON_FONT font_material_symbols_16_4)
    set(DEFAULT_EMOJI_COLLECTION noto-color-emoji_32)
endif()

if(NOT BOARD_DIR AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    message(FATAL_ERROR "The selected board does not define BOARD_DIR")
endif()

set(BOARD_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/config.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BOARD_CONFIG_FILE}")

# BOARD_TYPE and BOARD_MANUFACTURER are reported by the firmware.
# Prefer config.json and fall back to values derived from the full board directory.
set(BOARD_TYPE "")
set(BOARD_MANUFACTURER "")
if(EXISTS "${BOARD_CONFIG_FILE}")
    file(READ "${BOARD_CONFIG_FILE}" BOARD_CONFIG_JSON)
    string(REGEX MATCH "\"type\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" BOARD_TYPE_MATCH "${BOARD_CONFIG_JSON}")
    if(BOARD_TYPE_MATCH)
        set(BOARD_TYPE "${CMAKE_MATCH_1}")
    endif()
    string(REGEX MATCH "\"manufacturer\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" BOARD_MANUFACTURER_MATCH "${BOARD_CONFIG_JSON}")
    if(BOARD_MANUFACTURER_MATCH)
        set(BOARD_MANUFACTURER "${CMAKE_MATCH_1}")
    endif()
endif()
if(NOT BOARD_TYPE)
    string(REPLACE "/" "-" BOARD_TYPE "${BOARD_DIR}")
endif()
if(NOT BOARD_MANUFACTURER AND BOARD_DIR MATCHES "^([^/]+)/")
    set(BOARD_MANUFACTURER "${CMAKE_MATCH_1}")
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT BOARD_TYPE MATCHES "^[a-z0-9.-]+$")
    message(FATAL_ERROR
        "Invalid BOARD_TYPE '${BOARD_TYPE}': only lowercase letters, digits, '.' and '-' are allowed"
    )
endif()

include(${CMAKE_CURRENT_LIST_DIR}/boards/common/wzh/wake_model_check.cmake)

file(GLOB BOARD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/*.c
)
list(APPEND SOURCES ${BOARD_SOURCES})

# WZH 三产品共享基础层（Flash 抽象/PNDA 音频仓库/播放引擎等），
# 只在三块 WZH 板型下参与编译，不影响其他板型的固件体积。
if(CONFIG_BOARD_TYPE_WZH_CRAWLPANDA OR CONFIG_BOARD_TYPE_WZH_DINOSAUR OR CONFIG_BOARD_TYPE_WZH_TAILREDPANDA)
    file(GLOB WZH_COMMON_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/wzh/*.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/wzh/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/wzh/voice_tts/*.cc
    )
    list(APPEND SOURCES ${WZH_COMMON_SOURCES})
    list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/wzh)
    list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/wzh/voice_tts)
    file(GLOB_RECURSE WZH_UI_INPUTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/wzh_ui/*")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WZH_UI_INPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/pack_wzh_ui.py")
    execute_process(COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/pack_wzh_ui.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/wzh_ui" "${CMAKE_CURRENT_BINARY_DIR}/wzh_ui_pack.bin"
        "${CMAKE_CURRENT_BINARY_DIR}/wzh_ui_manifest.inc" COMMAND_ERROR_IS_FATAL ANY)
    list(APPEND INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}")
    set(WZH_CONSOLE_FILES "${CMAKE_CURRENT_BINARY_DIR}/wzh_ui_pack.bin")
    set(WZH_CONVERSATION_SOUNDS "assets/wzh_sounds/conversation_exit.ogg")
endif()

# Select one audio engine for each chip family
if(CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32P4 OR CONFIG_IDF_TARGET_ESP32S31)
    list(APPEND SOURCES "audio/engines/afe_audio_engine.cc")
    if(CONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND)
        list(APPEND SOURCES "audio/engines/neural_audio_frontend.cc")
    endif()
    list(APPEND SOURCES "audio/wake_words/custom_wake_word.cc")
    list(APPEND SOURCES "audio/wake_words/wake_word_audio_cache.cc")
else()
    list(APPEND SOURCES "audio/engines/lite_audio_engine.cc")
    list(APPEND SOURCES "audio/wake_words/esp_wake_word.cc")
endif()

# Auto Select Additional Sources
if (CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING)
    list(APPEND SOURCES "boards/common/blufi.cpp")
endif ()
# Select language directory according to Kconfig
if(CONFIG_LANGUAGE_ZH_CN)
    set(LANG_DIR "zh-CN")
elseif(CONFIG_LANGUAGE_ZH_TW)
    set(LANG_DIR "zh-TW")
elseif(CONFIG_LANGUAGE_EN_US)
    set(LANG_DIR "en-US")
elseif(CONFIG_LANGUAGE_JA_JP)
    set(LANG_DIR "ja-JP")
elseif(CONFIG_LANGUAGE_KO_KR)
    set(LANG_DIR "ko-KR")
elseif(CONFIG_LANGUAGE_VI_VN)
    set(LANG_DIR "vi-VN")
elseif(CONFIG_LANGUAGE_TH_TH)
    set(LANG_DIR "th-TH")
elseif(CONFIG_LANGUAGE_DE_DE)
    set(LANG_DIR "de-DE")
elseif(CONFIG_LANGUAGE_FR_FR)
    set(LANG_DIR "fr-FR")
elseif(CONFIG_LANGUAGE_ES_ES)
    set(LANG_DIR "es-ES")
elseif(CONFIG_LANGUAGE_IT_IT)
    set(LANG_DIR "it-IT")
elseif(CONFIG_LANGUAGE_RU_RU)
    set(LANG_DIR "ru-RU")
elseif(CONFIG_LANGUAGE_AR_SA)
    set(LANG_DIR "ar-SA")
elseif(CONFIG_LANGUAGE_HI_IN)
    set(LANG_DIR "hi-IN")
elseif(CONFIG_LANGUAGE_PT_PT)
    set(LANG_DIR "pt-PT")
elseif(CONFIG_LANGUAGE_PL_PL)
    set(LANG_DIR "pl-PL")
elseif(CONFIG_LANGUAGE_CS_CZ)
    set(LANG_DIR "cs-CZ")
elseif(CONFIG_LANGUAGE_FI_FI)
    set(LANG_DIR "fi-FI")
elseif(CONFIG_LANGUAGE_TR_TR)
    set(LANG_DIR "tr-TR")
elseif(CONFIG_LANGUAGE_ID_ID)
    set(LANG_DIR "id-ID")
elseif(CONFIG_LANGUAGE_UK_UA)
    set(LANG_DIR "uk-UA")
elseif(CONFIG_LANGUAGE_RO_RO)
    set(LANG_DIR "ro-RO")
elseif(CONFIG_LANGUAGE_BG_BG)
    set(LANG_DIR "bg-BG")
elseif(CONFIG_LANGUAGE_CA_ES)
    set(LANG_DIR "ca-ES")
elseif(CONFIG_LANGUAGE_DA_DK)
    set(LANG_DIR "da-DK")
elseif(CONFIG_LANGUAGE_EL_GR)
    set(LANG_DIR "el-GR")
elseif(CONFIG_LANGUAGE_FA_IR)
    set(LANG_DIR "fa-IR")
elseif(CONFIG_LANGUAGE_FIL_PH)
    set(LANG_DIR "fil-PH")
elseif(CONFIG_LANGUAGE_HE_IL)
    set(LANG_DIR "he-IL")
elseif(CONFIG_LANGUAGE_HR_HR)
    set(LANG_DIR "hr-HR")
elseif(CONFIG_LANGUAGE_HU_HU)
    set(LANG_DIR "hu-HU")
elseif(CONFIG_LANGUAGE_MS_MY)
    set(LANG_DIR "ms-MY")
elseif(CONFIG_LANGUAGE_NB_NO)
    set(LANG_DIR "nb-NO")
elseif(CONFIG_LANGUAGE_NL_NL)
    set(LANG_DIR "nl-NL")
elseif(CONFIG_LANGUAGE_SK_SK)
    set(LANG_DIR "sk-SK")
elseif(CONFIG_LANGUAGE_SL_SI)
    set(LANG_DIR "sl-SI")
elseif(CONFIG_LANGUAGE_SV_SE)
    set(LANG_DIR "sv-SE")
elseif(CONFIG_LANGUAGE_SR_RS)
    set(LANG_DIR "sr-RS")
endif()

# Define generation path
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")

# Collect current language audio files
file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/*.ogg)

# If not en-US, collect en-US audio files as fallback for missing files
if(NOT LANG_DIR STREQUAL "en-US")
    file(GLOB EN_US_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/en-US/*.ogg)

    # Extract filenames (without path) from current language
    set(EXISTING_NAMES "")
    foreach(SOUND_FILE ${LANG_SOUNDS})
        get_filename_component(FILENAME ${SOUND_FILE} NAME)
        list(APPEND EXISTING_NAMES ${FILENAME})
    endforeach()

    # Only add en-US audio files that are missing in current language
    foreach(EN_SOUND ${EN_US_SOUNDS})
        get_filename_component(FILENAME ${EN_SOUND} NAME)
        if(NOT ${FILENAME} IN_LIST EXISTING_NAMES)
            list(APPEND LANG_SOUNDS ${EN_SOUND})
            message(STATUS "Using en-US fallback for missing audio: ${FILENAME}")
        endif()
    endforeach()
endif()

# 注意：不能用 CONFIGURE_DEPENDS —— IDF 的 component 脚本模式会 include 本文件，
# 脚本模式下该选项报错。新增音效文件后重新构建（重新 configure）即可生效。
file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.ogg)
set(WZH_EXCLUDED_COMMON_ARGS "")

if(CONFIG_WZH_SYSTEM_PROMPTS AND (CONFIG_BOARD_TYPE_WZH_CRAWLPANDA OR CONFIG_BOARD_TYPE_WZH_DINOSAUR OR CONFIG_BOARD_TYPE_WZH_TAILREDPANDA))
    include("${PROJECT_DIR}/scripts/cmake/wzh_system_prompts.cmake")
    if(CONFIG_BOARD_TYPE_WZH_CRAWLPANDA)
        set(WZH_ONBOARDING_PROFILE an)
    elseif(CONFIG_BOARD_TYPE_WZH_TAILREDPANDA)
        set(WZH_ONBOARDING_PROFILE xing)
    else()
        set(WZH_ONBOARDING_PROFILE long)
    endif()
    set(WZH_ONBOARDING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/wzh_onboarding_voices/${WZH_ONBOARDING_PROFILE}")
    file(GLOB WZH_ONBOARDING_SOUNDS "${WZH_ONBOARDING_DIR}/*.ogg")
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        execute_process(COMMAND python3 "${PROJECT_DIR}/scripts/audio/validate_wzh_onboarding.py"
                        "${WZH_ONBOARDING_PROFILE}" RESULT_VARIABLE WZH_ONBOARDING_RESULT)
        if(NOT WZH_ONBOARDING_RESULT EQUAL 0)
            message(FATAL_ERROR "Invalid WZH onboarding audio pack")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/wzh_onboarding_voices/manifest.json"
            "${PROJECT_DIR}/scripts/audio/validate_wzh_onboarding.py" ${WZH_ONBOARDING_SOUNDS})
    endif()
    foreach(ONBOARDING_SOUND IN LISTS WZH_ONBOARDING_SOUNDS)
        get_filename_component(ONBOARDING_NAME "${ONBOARDING_SOUND}" NAME)
        list(REMOVE_ITEM COMMON_SOUNDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/common/${ONBOARDING_NAME}")
        list(APPEND COMMON_SOUNDS "${ONBOARDING_SOUND}")
    endforeach()
else()
    # These product-specific fallbacks exist so gen_lang.py can declare their
    # symbols, but must not consume flash on unrelated builds.
    foreach(ONBOARDING_NAME intro touch motion cmd1 net_invite wake ap page connected
                            ability interrupt clone farewell idle_miss adv_idle_seen adv_wifi_waiting)
        list(REMOVE_ITEM COMMON_SOUNDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/common/${ONBOARDING_NAME}.ogg")
        list(APPEND WZH_EXCLUDED_COMMON_ARGS --exclude-common "${ONBOARDING_NAME}")
    endforeach()
endif()

# If target chip is ESP32, exclude specific files to avoid build errors
if(CONFIG_IDF_TARGET_ESP32)
    list(REMOVE_ITEM SOURCES "audio/codecs/box_audio_codec.cc"
                             "audio/codecs/es8388_audio_codec.cc"
                             "audio/codecs/es8389_audio_codec.cc"
                             "led/gpio_led.cc"
                             "display/lvgl_display/jpg/image_to_jpeg.cpp"
                             "display/lvgl_display/jpg/jpeg_to_image.c"
                             "boards/common/nt26_board.cc"
                             "boards/common/ml307_board.cc"
                             "boards/common/dual_network_board.cc"
                             )
endif()

# Include EspVideo if target is ESP32S3 or ESP32P4
if(CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32P4)
    list(APPEND SOURCES "boards/common/esp_video.cc"
                        "boards/common/rndis_board.cc"
                        )
endif()

# Include EspVideo if target is ESP32S31
if(CONFIG_IDF_TARGET_ESP32S31)
    list(APPEND SOURCES "boards/common/esp_video.cc")
endif()

# Include Esp32Camera if target is ESP32S3
if(CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND SOURCES "boards/common/esp32_camera.cc")
endif()

set(MAIN_PRIV_REQUIRES_EXTRA "")
idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS} ${WZH_CONSOLE_FILES} ${WZH_CONVERSATION_SOUNDS}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    PRIV_REQUIRES
                        esp_pm
                        esp_psram
                        esp_netif
                        esp_eth
                        esp_driver_gpio
                        esp_driver_uart
                        esp_driver_spi
                        esp_driver_i2c
                        esp_driver_i2s
                        esp_driver_ledc
                        esp_driver_jpeg
                        esp_driver_ppa
                        esp_driver_tsens
                        esp_app_format
                        app_update
                        esp_http_client
                        spi_flash
                        mbedtls
                        console
                        efuse
                        bt
                        fatfs
                        xiaozhi-fonts
                        ${MAIN_PRIV_REQUIRES_EXTRA}
                    )

# Use target_compile_definitions to define the reported board identity
# If BOARD_NAME is empty, use BOARD_TYPE
if(NOT BOARD_NAME)
    set(BOARD_NAME ${BOARD_TYPE})
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT BOARD_NAME MATCHES "^[a-z0-9.-]+$")
    message(FATAL_ERROR
        "Invalid BOARD_NAME '${BOARD_NAME}': only lowercase letters, digits, '.' and '-' are allowed"
    )
endif()
string(REGEX MATCH "^font_noto_sans_basic_([0-9]+)_([0-9]+)$" _font_match "${BUILTIN_TEXT_FONT}")
if(NOT _font_match)
    message(FATAL_ERROR "Unsupported built-in text font name: ${BUILTIN_TEXT_FONT}")
endif()
set(TEXT_FONT_SIZE "${CMAKE_MATCH_1}")
set(TEXT_FONT_BPP "${CMAKE_MATCH_2}")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB}
                        PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                        PRIVATE BOARD_MANUFACTURER=\"${BOARD_MANUFACTURER}\"
                        PRIVATE BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT} BUILTIN_ICON_FONT=${BUILTIN_ICON_FONT}
                        PRIVATE TEXT_FONT_SIZE=${TEXT_FONT_SIZE} TEXT_FONT_BPP=${TEXT_FONT_BPP}
                        )
endif()

# Add generation rules
add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${PROJECT_DIR}/scripts/gen_lang.py
            --language "${LANG_DIR}"
            --output "${LANG_HEADER}"
            ${WZH_EXCLUDED_COMMON_ARGS}
    DEPENDS
        ${LANG_JSON}
        ${PROJECT_DIR}/scripts/gen_lang.py
        ${COMMON_SOUNDS}
    COMMENT "Generating ${LANG_DIR} language config"
)

# Force build generation dependencies
add_custom_target(lang_header ALL
    DEPENDS ${LANG_HEADER}
)

# Find ESP-SR component dynamically
find_component_by_pattern("espressif__esp-sr" ESP_SR_COMPONENT ESP_SR_COMPONENT_PATH)
if(ESP_SR_COMPONENT_PATH)
    set(ESP_SR_MODEL_PATH "${ESP_SR_COMPONENT_PATH}/model")
endif()

# Find xiaozhi-fonts component dynamically
find_component_by_pattern("xiaozhi-fonts" NOTO_FONTS_COMPONENT NOTO_FONTS_COMPONENT_PATH)
if(NOTO_FONTS_COMPONENT_PATH)
    set(NOTO_FONTS_PATH "${NOTO_FONTS_COMPONENT_PATH}")
endif()




# Function to build default assets based on configuration
function(build_default_assets_bin)
    # Set output path for generated assets.bin
    set(GENERATED_ASSETS_BIN "${CMAKE_BINARY_DIR}/generated_assets.bin")
    set(DEFAULT_ASSETS_DEPENDENCIES
        "${SDKCONFIG}"
        "${PROJECT_DIR}/scripts/build_default_assets.py"
    )

    # Prepare arguments for build script
    set(BUILD_ARGS
        "--sdkconfig" "${SDKCONFIG}"
        "--output" "${GENERATED_ASSETS_BIN}"
    )

    # Add builtin text font if defined
    if(BUILTIN_TEXT_FONT)
        list(APPEND BUILD_ARGS "--builtin_text_font" "${BUILTIN_TEXT_FONT}")
        string(REPLACE "_basic_" "_common_" COMMON_TEXT_FONT "${BUILTIN_TEXT_FONT}")
        set(COMMON_TEXT_FONT_FILE "${NOTO_FONTS_PATH}/cbin/${COMMON_TEXT_FONT}.bin")
        if(EXISTS "${COMMON_TEXT_FONT_FILE}")
            list(APPEND DEFAULT_ASSETS_DEPENDENCIES "${COMMON_TEXT_FONT_FILE}")
        endif()
        if(EXISTS "${NOTO_FONTS_PATH}/manifest.json")
            list(APPEND DEFAULT_ASSETS_DEPENDENCIES "${NOTO_FONTS_PATH}/manifest.json")
        endif()
    endif()

    # Add default emoji collection if defined
    if(DEFAULT_EMOJI_COLLECTION)
        list(APPEND BUILD_ARGS "--emoji_collection" "${DEFAULT_EMOJI_COLLECTION}")
        file(GLOB DEFAULT_EMOJI_FILES CONFIGURE_DEPENDS
            "${NOTO_FONTS_PATH}/png/${DEFAULT_EMOJI_COLLECTION}/*"
        )
        list(APPEND DEFAULT_ASSETS_DEPENDENCIES ${DEFAULT_EMOJI_FILES})
    endif()

    # Add default assets extra files if defined
    if(DEFAULT_ASSETS_EXTRA_FILES)
        list(APPEND BUILD_ARGS "--extra_files" "${DEFAULT_ASSETS_EXTRA_FILES}")
    endif()

    list(APPEND BUILD_ARGS "--esp_sr_model_path" "${ESP_SR_MODEL_PATH}")
    list(APPEND BUILD_ARGS "--noto_fonts_path" "${NOTO_FONTS_PATH}")

    set(NEURAL_MODEL_DIRS "")
    if(CONFIG_SR_NSN_NSNET2)
        list(APPEND NEURAL_MODEL_DIRS "${ESP_SR_MODEL_PATH}/nsnet_model/nsnet2")
    endif()
    if(CONFIG_SR_VADN_VADNET1_MEDIUM)
        list(APPEND NEURAL_MODEL_DIRS "${ESP_SR_MODEL_PATH}/vadnet_model/vadnet1_medium")
    endif()
    foreach(MODEL_DIR IN LISTS NEURAL_MODEL_DIRS)
        file(GLOB NEURAL_MODEL_FILES CONFIGURE_DEPENDS "${MODEL_DIR}/*")
        list(APPEND DEFAULT_ASSETS_DEPENDENCIES ${NEURAL_MODEL_FILES})
    endforeach()

    # Create custom command to build assets
    add_custom_command(
        OUTPUT ${GENERATED_ASSETS_BIN}
        COMMAND python ${PROJECT_DIR}/scripts/build_default_assets.py ${BUILD_ARGS}
        DEPENDS ${DEFAULT_ASSETS_DEPENDENCIES}
        COMMENT "Building default assets.bin based on configuration"
        VERBATIM
    )

    # Create target for generated assets
    add_custom_target(generated_default_assets ALL
        DEPENDS ${GENERATED_ASSETS_BIN}
    )

    # Set the generated file path in parent scope
    set(GENERATED_ASSETS_LOCAL_FILE ${GENERATED_ASSETS_BIN} PARENT_SCOPE)

    message(STATUS "Default assets build configured: ${GENERATED_ASSETS_BIN}")
endfunction()


# Function to get local assets file path (handles both URL and local file)
function(get_assets_local_file assets_source assets_local_file_var)
    # Check if it's a URL (starts with http:// or https://)
    if(assets_source MATCHES "^https?://")
        # It's a URL, download it
        get_filename_component(ASSETS_FILENAME "${assets_source}" NAME)
        set(ASSETS_LOCAL_FILE "${CMAKE_BINARY_DIR}/${ASSETS_FILENAME}")
        set(ASSETS_TEMP_FILE "${CMAKE_BINARY_DIR}/${ASSETS_FILENAME}.tmp")

        # Check if local file exists
        if(EXISTS ${ASSETS_LOCAL_FILE})
            message(STATUS "Assets file ${ASSETS_FILENAME} already exists, skipping download")
        else()
            message(STATUS "Downloading ${ASSETS_FILENAME}")

            # Clean up any existing temp file
            if(EXISTS ${ASSETS_TEMP_FILE})
                file(REMOVE ${ASSETS_TEMP_FILE})
            endif()

            # Download to temporary file first
            file(DOWNLOAD ${assets_source} ${ASSETS_TEMP_FILE}
                 STATUS DOWNLOAD_STATUS)
            list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
            if(NOT STATUS_CODE EQUAL 0)
                # Clean up temp file on failure
                if(EXISTS ${ASSETS_TEMP_FILE})
                    file(REMOVE ${ASSETS_TEMP_FILE})
                endif()
                message(FATAL_ERROR "Failed to download ${ASSETS_FILENAME} from ${assets_source}")
            endif()

            # Move temp file to final location (atomic operation)
            file(RENAME ${ASSETS_TEMP_FILE} ${ASSETS_LOCAL_FILE})
            message(STATUS "Successfully downloaded ${ASSETS_FILENAME}")
        endif()
    else()
        # It's a local file path
        if(IS_ABSOLUTE "${assets_source}")
            set(ASSETS_LOCAL_FILE "${assets_source}")
        else()
            set(ASSETS_LOCAL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${assets_source}")
        endif()

        # Check if local file exists
        if(NOT EXISTS ${ASSETS_LOCAL_FILE})
            message(FATAL_ERROR "Assets file not found: ${ASSETS_LOCAL_FILE}")
        endif()

        message(STATUS "Using assets file: ${ASSETS_LOCAL_FILE}")
    endif()

    set(${assets_local_file_var} ${ASSETS_LOCAL_FILE} PARENT_SCOPE)
endfunction()


partition_table_get_partition_info(size "--partition-name assets" "size")
partition_table_get_partition_info(offset "--partition-name assets" "offset")
if ("${size}" AND "${offset}")
    # Flash assets based on configuration
    if(CONFIG_FLASH_DEFAULT_ASSETS)
        # Build default assets based on configuration
        build_default_assets_bin()
        esptool_py_flash_to_partition(flash "assets" "${GENERATED_ASSETS_LOCAL_FILE}")
        message(STATUS "Generated default assets flash configured: ${GENERATED_ASSETS_LOCAL_FILE} -> assets partition")
    elseif(CONFIG_FLASH_CUSTOM_ASSETS)
        # Flash custom assets
        get_assets_local_file("${CONFIG_CUSTOM_ASSETS_FILE}" ASSETS_LOCAL_FILE)
        esptool_py_flash_to_partition(flash "assets" "${ASSETS_LOCAL_FILE}")
        message(STATUS "Custom assets flash configured: ${ASSETS_LOCAL_FILE} -> assets partition")
    elseif(CONFIG_FLASH_EXPRESSION_ASSETS)
        set(ASSETS_NAME "expression_assets")
        set(ASSETS_PARTITION "assets")
        set(ASSETS_FILE "${CMAKE_BINARY_DIR}/${ASSETS_NAME}.bin")

        if(EMOTE_EXTERNAL_PATH)
            build_speaker_assets_bin("${ASSETS_PARTITION}" ${EMOTE_RESOLUTION} ${ASSETS_FILE} ${CONFIG_MMAP_FILE_NAME_LENGTH} "${EMOTE_EXTERNAL_PATH}")
        else()
            build_speaker_assets_bin("${ASSETS_PARTITION}" ${EMOTE_RESOLUTION} ${ASSETS_FILE} ${CONFIG_MMAP_FILE_NAME_LENGTH})
        endif()
        message(STATUS "Generated emote assets: ${ASSETS_FILE} -> ${ASSETS_PARTITION} partition")
        esptool_py_flash_to_partition(flash "${ASSETS_PARTITION}" "${ASSETS_FILE}")
    elseif(CONFIG_FLASH_NONE_ASSETS)
        message(STATUS "Assets flashing disabled (FLASH_NONE_ASSETS)")
    endif()
else()
    message(STATUS "Assets partition not found, using v1 partition table")
endif()
