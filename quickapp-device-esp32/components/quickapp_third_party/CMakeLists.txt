# Third-party libraries — ESP-IDF component wrapper
# Provides: QuickJS, Yoga, zlib (vendored sources)

# QA_MINIMAL 模式: 注册空组件, 不编译任何源码 (硬件验证阶段)
if(QA_MINIMAL)
    idf_component_register()
    return()
endif()

get_filename_component(QUICKAPP_KIT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(THIRD_PARTY_ROOT "${QUICKAPP_KIT_ROOT}/source/third_party")

# --- QuickJS ---
set(QUICKJS_SOURCE "${THIRD_PARTY_ROOT}/quickjs/quickjs.c")
file(READ "${QUICKJS_SOURCE}" QUICKJS_ESP32_CONTENT)
string(FIND "${QUICKJS_ESP32_CONTENT}"
    "return malloc_usable_size((void *)ptr);" QUICKJS_MALLOC_PORT_OFFSET)
string(FIND "${QUICKJS_ESP32_CONTENT}"
    "res = -tm.tm_gmtoff / 60;" QUICKJS_TIMEZONE_PORT_OFFSET)
if(QUICKJS_MALLOC_PORT_OFFSET EQUAL -1 OR QUICKJS_TIMEZONE_PORT_OFFSET EQUAL -1)
    message(FATAL_ERROR "QuickJS ESP32 port assumptions no longer match the vendored source")
endif()
string(REPLACE
    "return malloc_usable_size((void *)ptr);"
    "return 0;"
    QUICKJS_ESP32_CONTENT "${QUICKJS_ESP32_CONTENT}")
string(REPLACE
    "res = -tm.tm_gmtoff / 60;"
    "struct tm gm_tm; gmtime_r(&ti, &gm_tm); tm.tm_isdst = -1; gm_tm.tm_isdst = -1; res = (int)(difftime(mktime(&gm_tm), mktime(&tm)) / 60);"
    QUICKJS_ESP32_CONTENT "${QUICKJS_ESP32_CONTENT}")
set(QUICKJS_ESP32_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/quickjs_esp32.c")
file(WRITE "${QUICKJS_ESP32_SOURCE}" "${QUICKJS_ESP32_CONTENT}")

set(QUICKJS_SRCS
    "${QUICKJS_ESP32_SOURCE}"
    "${THIRD_PARTY_ROOT}/quickjs/dtoa.c"
    "${THIRD_PARTY_ROOT}/quickjs/libregexp.c"
    "${THIRD_PARTY_ROOT}/quickjs/libunicode.c"
    "${THIRD_PARTY_ROOT}/quickjs/cutils.c"
)

# --- Yoga ---
file(GLOB_RECURSE YOGA_SRCS
    "${THIRD_PARTY_ROOT}/yoga/yoga/*.cpp"
)
set(YOGA_NODE_STYLE_SOURCE
    "${THIRD_PARTY_ROOT}/yoga/yoga/YGNodeStyle.cpp")
file(READ "${YOGA_NODE_STYLE_SOURCE}" YOGA_ESP32_CONTENT)
string(FIND "${YOGA_ESP32_CONTENT}"
    "int32_t YGNodeStyleGetGridColumnStart" YOGA_GRID_PORT_OFFSET)
if(YOGA_GRID_PORT_OFFSET EQUAL -1)
    message(FATAL_ERROR "Yoga ESP32 port assumptions no longer match the vendored source")
endif()
string(REPLACE
    "int32_t YGNodeStyleGetGrid"
    "int YGNodeStyleGetGrid"
    YOGA_ESP32_CONTENT "${YOGA_ESP32_CONTENT}")
set(YOGA_ESP32_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/YGNodeStyle_esp32.cpp")
file(WRITE "${YOGA_ESP32_SOURCE}" "${YOGA_ESP32_CONTENT}")
list(REMOVE_ITEM YOGA_SRCS "${YOGA_NODE_STYLE_SOURCE}")
list(APPEND YOGA_SRCS "${YOGA_ESP32_SOURCE}")

file(STRINGS "${THIRD_PARTY_ROOT}/quickjs/VERSION" QUICKAPP_QUICKJS_VERSION LIMIT_COUNT 1)

idf_component_register(
    SRCS
        ${QUICKJS_SRCS}
        ${YOGA_SRCS}
    INCLUDE_DIRS
        "${THIRD_PARTY_ROOT}/yoga"
    PRIV_INCLUDE_DIRS
        "${THIRD_PARTY_ROOT}/quickjs"
    REQUIRES
        ""
)

# QuickJS 需要的编译定义
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_VERSION="${QUICKAPP_QUICKJS_VERSION}"
    _GNU_SOURCE=1
)

# Yoga C++17
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

# 抑制第三方代码的警告
target_compile_options(${COMPONENT_LIB} PRIVATE
    -w
    -Wno-error=incompatible-pointer-types
)
