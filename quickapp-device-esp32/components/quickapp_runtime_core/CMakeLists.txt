# QuickApp Runtime Core — ESP-IDF component wrapper
# Bridges the CMake library targets from quickapp-runtime-core into ESP-IDF build

# QA_MINIMAL 模式: 注册空组件, 不编译 (硬件验证阶段)
if(QA_MINIMAL)
    idf_component_register()
    return()
endif()

get_filename_component(QUICKAPP_KIT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(CORE_ROOT "${QUICKAPP_KIT_ROOT}/quickapp-runtime-core")

# 收集所有 core 源文件
file(GLOB_RECURSE CORE_SRCS
    "${CORE_ROOT}/src/*.cpp"
    "${CORE_ROOT}/src/*.c"
)

idf_component_register(
    SRCS ${CORE_SRCS}
    INCLUDE_DIRS
        "${CORE_ROOT}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIV_INCLUDE_DIRS
        "${CORE_ROOT}/src"
        "${CORE_ROOT}/third_party"
    REQUIRES
        esp_rom
        quickapp_third_party
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions)

# Core 库编译定义
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    QUICKAPP_EMBEDDED=1
)
