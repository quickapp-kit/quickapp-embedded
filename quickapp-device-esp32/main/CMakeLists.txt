# QA_MINIMAL 模式: 只验证硬件 (LVGL + ILI9341 + XPT2046), 不集成 runtime
# 由项目根目录的 main/minimal.mode 文件统一控制。
if(QA_MINIMAL)
    message(STATUS "Building in MINIMAL mode (hardware test, no runtime)")
    idf_component_register(
        SRCS
            "main_minimal.cpp"
            "display_driver.cpp"
            "touch_driver.cpp"
        INCLUDE_DIRS "."
        REQUIRES
            driver
            esp_lcd
            esp_timer
            lvgl
    )
else()
    message(STATUS "Building in FULL mode (with QuickApp runtime)")
    idf_component_register(
        SRCS
            "main.cpp"
            "display_driver.cpp"
            "touch_driver.cpp"
            "spiffs_package_source.cpp"
        INCLUDE_DIRS "."
        REQUIRES
            driver
            esp_lcd
            esp_timer
            spiffs
            esp_partition
            spi_flash
            lvgl
        PRIV_REQUIRES
            quickapp_runtime_core
            quickapp_runtime_js
            quickapp_runtime_lvgl
    )
endif()

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
if(NOT QA_MINIMAL)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions)
endif()
