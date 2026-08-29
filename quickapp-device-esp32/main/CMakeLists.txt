# QA_MINIMAL 模式: 只验证硬件 (LVGL + ILI9341 + XPT2046), 不集成 runtime
# 通过 idf.py -DQA_MINIMAL=1 build 启用
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/minimal.mode")
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
