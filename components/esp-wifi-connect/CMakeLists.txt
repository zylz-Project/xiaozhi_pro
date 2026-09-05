message("esp-wifi-connect (ZYLZ fork): IDF_VERSION_MAJOR is ${IDF_VERSION_MAJOR}")

# ---------------------------------------------------------------------------
# ZYLZ 品牌化：仅在三块 WZH 产品板下用品牌模板生成配网页面（玩具暖色风，
# 每款产品一个专属主色），其余板型嵌入上游原版页面，行为与官方组件一致。
# 品牌文案与配色在编译期注入（configure_file），页面交互逻辑与原版完全相同。
# ---------------------------------------------------------------------------
set(WZH_BRAND_NAME "")
if(CONFIG_BOARD_TYPE_WZH_CRAWLPANDA)
    set(WZH_BRAND_NAME "CrawlPanda")
    set(WZH_EMOJI "🐼")
    set(WZH_ACCENT "#16a34a")
    set(WZH_ACCENT_HOVER "#15803d")
    set(WZH_ACCENT_SOFT "rgba(22, 163, 74, 0.12)")
    set(WZH_BG_1 "#f0fdf4")
    set(WZH_BG_2 "#dcfce7")
    set(WZH_ACCENT_DARK "#4ade80")
    set(WZH_ACCENT_HOVER_DARK "#86efac")
    set(WZH_ACCENT_SOFT_DARK "rgba(74, 222, 128, 0.16)")
elseif(CONFIG_BOARD_TYPE_WZH_DINOSAUR)
    set(WZH_BRAND_NAME "Dino")
    set(WZH_EMOJI "🦕")
    set(WZH_ACCENT "#ea580c")
    set(WZH_ACCENT_HOVER "#c2410c")
    set(WZH_ACCENT_SOFT "rgba(234, 88, 12, 0.12)")
    set(WZH_BG_1 "#fff7ed")
    set(WZH_BG_2 "#ffedd5")
    set(WZH_ACCENT_DARK "#fb923c")
    set(WZH_ACCENT_HOVER_DARK "#fdba74")
    set(WZH_ACCENT_SOFT_DARK "rgba(251, 146, 60, 0.16)")
elseif(CONFIG_BOARD_TYPE_WZH_TAILREDPANDA)
    set(WZH_BRAND_NAME "RedPanda")
    set(WZH_EMOJI "🐾")
    set(WZH_ACCENT "#e11d48")
    set(WZH_ACCENT_HOVER "#be123c")
    set(WZH_ACCENT_SOFT "rgba(225, 29, 72, 0.12)")
    set(WZH_BG_1 "#fff1f2")
    set(WZH_BG_2 "#ffe4e6")
    set(WZH_ACCENT_DARK "#fb7185")
    set(WZH_ACCENT_HOVER_DARK "#fda4af")
    set(WZH_ACCENT_SOFT_DARK "rgba(251, 113, 133, 0.16)")
endif()

if(NOT WZH_BRAND_NAME STREQUAL "")
    set(WZH_PAGE_TITLE "ZYLZ · ${WZH_BRAND_NAME} 设备配网")
    configure_file("assets/wifi_configuration_branded.html.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/wifi_configuration.html" @ONLY)
    configure_file("assets/wifi_configuration_done_branded.html.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/wifi_configuration_done.html" @ONLY)
    set(WIFI_CONFIG_HTML "${CMAKE_CURRENT_BINARY_DIR}/wifi_configuration.html")
    set(WIFI_CONFIG_DONE_HTML "${CMAKE_CURRENT_BINARY_DIR}/wifi_configuration_done.html")
    message("esp-wifi-connect (ZYLZ fork): branding for ${WZH_BRAND_NAME}")
else()
    set(WIFI_CONFIG_HTML "assets/wifi_configuration.html")
    set(WIFI_CONFIG_DONE_HTML "assets/wifi_configuration_done.html")
endif()

if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    # v6
    message("esp-wifi-connect (ZYLZ fork): idf_component_register for v6 used")

    idf_component_register(
        SRCS
            "wifi_manager.cc"
            "wifi_configuration_ap.cc"
            "wifi_station.cc"
            "ssid_manager.cc"
            "dns_server.cc"
        INCLUDE_DIRS
            "include"
        EMBED_TXTFILES
            "${WIFI_CONFIG_HTML}"
            "${WIFI_CONFIG_DONE_HTML}"
        REQUIRES "esp_timer" "esp_http_server" "esp_wifi" "nvs_flash"
    )
else()
    # v5
    message("esp-wifi-connect (ZYLZ fork): idf_component_register for v5 used")

    idf_component_register(
        SRCS
            "wifi_manager.cc"
            "wifi_configuration_ap.cc"
            "wifi_station.cc"
            "ssid_manager.cc"
            "dns_server.cc"
        INCLUDE_DIRS
            "include"
        EMBED_TXTFILES
            "${WIFI_CONFIG_HTML}"
            "${WIFI_CONFIG_DONE_HTML}"
        REQUIRES "esp_timer" "esp_http_server" "esp_wifi" "nvs_flash" "json"
    )
endif()
