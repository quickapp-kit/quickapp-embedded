# QuickApp Runtime LVGL — ESP-IDF component wrapper
# Bridges quickapp-runtime-lvgl into ESP-IDF build (embedded minimal config)

# QA_MINIMAL 模式: 注册空组件, 不编译 (硬件验证阶段)
if(EXISTS "${CMAKE_SOURCE_DIR}/main/minimal.mode")
    idf_component_register()
    return()
endif()

set(LVGL_RT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../quickapp-runtime-lvgl")

# 收集 LVGL runtime 源文件
file(GLOB_RECURSE LVGL_RT_SRCS
    "${LVGL_RT_ROOT}/src/*.cpp"
    "${LVGL_RT_ROOT}/src/*.c"
)

idf_component_register(
    SRCS ${LVGL_RT_SRCS}
    INCLUDE_DIRS
        "${LVGL_RT_ROOT}/include"
    REQUIRES
        quickapp_runtime_core
        quickapp_runtime_js
        lvgl
    PRIV_REQUIRES
        quickapp_third_party
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    QUICKAPP_EMBEDDED=1
    QUICKAPP_LVGL_EMBEDDED_MIN=1
)
