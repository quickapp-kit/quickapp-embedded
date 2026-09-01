# Third-party libraries — ESP-IDF component wrapper
# Provides: QuickJS, Yoga, zlib (vendored sources)

# QA_MINIMAL 模式: 注册空组件, 不编译任何源码 (硬件验证阶段)
if(QA_MINIMAL)
    idf_component_register()
    return()
endif()

set(THIRD_PARTY_ROOT "${QUICKAPP_KIT_ROOT}/source/third_party")

# --- QuickJS ---
file(GLOB QUICKJS_SRCS
    "${THIRD_PARTY_ROOT}/quickjs/*.c"
)
# 排除 quickjs 的 standalone 可执行文件源
list(FILTER QUICKJS_SRCS EXCLUDE REGEX "qjs\\.c$|qjsc\\.c$|run-test262\\.c$")

# --- Yoga ---
file(GLOB YOGA_SRCS
    "${THIRD_PARTY_ROOT}/yoga/yoga/*.cpp"
    "${THIRD_PARTY_ROOT}/yoga/yoga/**/*.cpp"
)

idf_component_register(
    SRCS
        ${QUICKJS_SRCS}
        ${YOGA_SRCS}
    INCLUDE_DIRS
        "${THIRD_PARTY_ROOT}/quickjs"
        "${THIRD_PARTY_ROOT}/yoga"
    REQUIRES
        ""
)

# QuickJS 需要的编译定义
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_BIGNUM=1
    CONFIG_VERSION="2024-01-13"
    _GNU_SOURCE=1
)

# Yoga C++17
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

# 抑制第三方代码的警告
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
