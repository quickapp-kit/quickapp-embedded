# QuickApp Device — ESP32-S3 真机部署
# 顶层 ESP-IDF 项目 CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# quickapp-device-esp32 lives at quickapp-kit-ai/quickapp-embedded/quickapp-device-esp32.
# Component wrappers repeat this boundary because ESP-IDF discovers dependencies
# in separate CMake scopes before configuring the project component.
set(QUICKAPP_KIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "QuickApp Kit monorepo root")

# ESP-IDF 项目组件搜索路径
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components"
)

# QA_MINIMAL 模式开关: 存在 main/minimal.mode 文件即启用
# (用文件标记而非 -D 参数, 保证 flash/monitor 等命令 re-run CMake 时开关不丢失)
file(GLOB QA_MINIMAL_MODE_MARKER CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/main/minimal.mode")
if(QA_MINIMAL_MODE_MARKER)
    set(QA_MINIMAL ON CACHE BOOL "Build only the ESP32 hardware validation app" FORCE)
    set(EXCLUDE_COMPONENTS
        quickapp_runtime_core
        quickapp_runtime_js
        quickapp_runtime_lvgl
        quickapp_third_party
    )
else()
    set(QA_MINIMAL OFF CACHE BOOL "Build only the ESP32 hardware validation app" FORCE)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(quickapp_device)
