# LVGL component wrapper for ESP-IDF
# Pulls in the vendored LVGL source from third_party

set(LVGL_ROOT "${QUICKAPP_KIT_ROOT}/source/third_party/lvgl")

file(GLOB_RECURSE LVGL_SRCS
    "${LVGL_ROOT}/src/*.c"
)

idf_component_register(
    SRCS ${LVGL_SRCS}
    INCLUDE_DIRS
        "${LVGL_ROOT}"
        "${LVGL_ROOT}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}"  # lv_conf.h lives here
    REQUIRES
        esp_timer
        driver
)

# LVGL 需要 lv_conf.h, 通过 LV_CONF_INCLUDE_SIMPLE 宏启用
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_CONF_INCLUDE_SIMPLE=1)
