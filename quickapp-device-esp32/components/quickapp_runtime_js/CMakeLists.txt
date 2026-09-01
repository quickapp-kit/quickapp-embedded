# QuickApp Runtime JS — ESP-IDF component wrapper
# Bridges quickapp-runtime-js into ESP-IDF build (uses vendored QuickJS)

# QA_MINIMAL 模式: 注册空组件, 不编译 (硬件验证阶段)
if(QA_MINIMAL)
    idf_component_register()
    return()
endif()

set(JS_ROOT "${QUICKAPP_KIT_ROOT}/quickapp-runtime-js")

# 收集所有 JS runtime 源文件
file(GLOB_RECURSE JS_SRCS
    "${JS_ROOT}/src/*.cpp"
    "${JS_ROOT}/src/*.c"
)

idf_component_register(
    SRCS ${JS_SRCS}
    INCLUDE_DIRS
        "${JS_ROOT}/include"
    REQUIRES
        quickapp_runtime_core
        quickapp_third_party
        lvgl
    PRIV_REQUIRES
        ""
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    QUICKAPP_EMBEDDED=1
)
