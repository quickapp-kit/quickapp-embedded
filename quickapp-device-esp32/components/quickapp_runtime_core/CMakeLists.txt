# QuickApp Runtime Core — ESP-IDF component wrapper
# Bridges the CMake library targets from quickapp-runtime-core into ESP-IDF build

# QA_MINIMAL 模式: 注册空组件, 不编译 (硬件验证阶段)
if(EXISTS "${CMAKE_SOURCE_DIR}/main/minimal.mode")
    idf_component_register()
    return()
endif()

set(CORE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../quickapp-runtime-core")

# 收集所有 core 源文件
file(GLOB_RECURSE CORE_SRCS
    "${CORE_ROOT}/src/*.cpp"
    "${CORE_ROOT}/src/*.c"
)

idf_component_register(
    SRCS ${CORE_SRCS}
    INCLUDE_DIRS
        "${CORE_ROOT}/include"
    REQUIRES
        lvgl
    PRIV_REQUIRES
        quickapp_third_party
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

# Core 库编译定义
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    QUICKAPP_EMBEDDED=1
)
