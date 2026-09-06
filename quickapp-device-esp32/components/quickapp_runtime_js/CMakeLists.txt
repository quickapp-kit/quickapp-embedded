# QuickApp Runtime JS — ESP-IDF component wrapper
# Bridges quickapp-runtime-js into ESP-IDF build (uses vendored QuickJS)

# QA_MINIMAL 模式: 注册空组件, 不编译 (硬件验证阶段)
if(QA_MINIMAL)
    idf_component_register()
    return()
endif()

get_filename_component(QUICKAPP_KIT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(JS_ROOT "${QUICKAPP_KIT_ROOT}/quickapp-runtime-core/runtime/js")
set(QUICKJS_ROOT "${QUICKAPP_KIT_ROOT}/source/third_party/quickjs")

file(GLOB_RECURSE JS_SRCS
    "${JS_ROOT}/src/*.cpp"
    "${JS_ROOT}/providers/quickjs/src/*.cpp"
)
list(FILTER JS_SRCS EXCLUDE REGEX "libuv_event_loop_backend\\.cpp$")

file(STRINGS "${QUICKJS_ROOT}/VERSION" QUICKAPP_QUICKJS_VERSION LIMIT_COUNT 1)
set(QUICKAPP_JS_QUICKJS_SOURCE_DIR "${QUICKJS_ROOT}")
configure_file(
    "${JS_ROOT}/cmake/quickjs_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/quickapp_quickjs_config.h"
    @ONLY
)

idf_component_register(
    SRCS ${JS_SRCS}
    INCLUDE_DIRS
        "${JS_ROOT}/include"
        "${JS_ROOT}/providers/quickjs/include"
    PRIV_INCLUDE_DIRS
        "${JS_ROOT}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    REQUIRES
        quickapp_runtime_core
        quickapp_third_party
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -fexceptions
    -Wno-error=maybe-uninitialized
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    QUICKAPP_EMBEDDED=1
    QUICKAPP_QUICKJS_VERSION="${QUICKAPP_QUICKJS_VERSION}"
)
