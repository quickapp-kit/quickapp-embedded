# QuickApp Device — ESP32-S3 真机部署
# 顶层 ESP-IDF 项目 CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# ESP-IDF 项目组件搜索路径
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components"
)

# QA_MINIMAL 模式开关: 存在 main/minimal.mode 文件即启用
# (用文件标记而非 -D 参数, 保证 flash/monitor 等命令 re-run CMake 时开关不丢失)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main/minimal.mode")
    set(QA_MINIMAL 1)
    set(EXCLUDE_COMPONENTS
        quickapp_runtime_core
        quickapp_runtime_js
        quickapp_runtime_lvgl
        quickapp_third_party
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(quickapp_device)
