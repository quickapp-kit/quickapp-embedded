# QuickApp Runtime LVGL — ESP-IDF component wrapper
# Bridges quickapp-runtime-lvgl into ESP-IDF build (embedded minimal config)

# QA_MINIMAL 模式: 注册空组件, 不编译 (硬件验证阶段)
if(QA_MINIMAL)
    idf_component_register()
    return()
endif()

get_filename_component(QUICKAPP_KIT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(LVGL_RT_ROOT "${QUICKAPP_KIT_ROOT}/quickapp-runtime-lvgl")

# 收集 LVGL runtime 源文件
file(GLOB_RECURSE LVGL_RT_SRCS
    "${LVGL_RT_ROOT}/src/*.cpp"
    "${LVGL_RT_ROOT}/src/*.c"
)
list(FILTER LVGL_RT_SRCS EXCLUDE REGEX
    "/backends/(libuv_file_package_source|libuv_loop_backend|sdl_backends)\\.cpp$"
)
list(FILTER LVGL_RT_SRCS EXCLUDE REGEX "/event/input_adapter\\.cpp$")

idf_component_register(
    SRCS ${LVGL_RT_SRCS}
    INCLUDE_DIRS
        "${LVGL_RT_ROOT}/include"
    PRIV_INCLUDE_DIRS
        "${LVGL_RT_ROOT}/src/font"
    REQUIRES
        quickapp_runtime_core
        quickapp_runtime_js
        lvgl
    PRIV_REQUIRES
        quickapp_third_party
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    QUICKAPP_EMBEDDED=1
    QUICKAPP_LVGL_EMBEDDED_MIN=1
)
