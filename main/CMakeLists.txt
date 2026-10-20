set(_m5ui_srcs
    "main.cpp"
    "plugin.cpp"
    "ttf_impl.cpp"
    "systems/epd_lut_tables.cpp"
)
set(_m5ui_requires
    m5gfx
    m5unified
    fatfs
    sdmmc
    esp_partition
    esp_driver_sdspi
    esp_driver_spi
    esp_wifi
    esp_netif
    esp_http_server
    nvs_flash
    elf_loader
)

idf_component_register(
    SRCS ${_m5ui_srcs}
    INCLUDE_DIRS
        "."
        "ecs"
        "systems"
        "ui"
        "text"
        "pages"
        "platform"
        "../apps/common"      # so_meta.h：.so 元信息结构，固件和 app 共用一份定义
    REQUIRES ${_m5ui_requires}
)
