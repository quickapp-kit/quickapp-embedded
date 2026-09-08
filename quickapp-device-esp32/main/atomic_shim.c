/*
 * Xtensa 原子内建 libcall 补充 (bring-up 用)
 *
 * 背景: 工具链在 -mdisable-hardware-atomics 下, 对 1 字节 std::atomic
 * (如 std::atomic_flag / atomic<bool>) 会发出 __atomic_test_and_set /
 * __atomic_clear 的 libcall, 但当前工具链未提供该符号 (无 libatomic)。
 * 这里用关中断的临界区实现, 保证多核/中断下的原子性。
 *
 * 注意: 这是 bring-up 阶段的补丁, 后续架构调整时应由 runtime 侧统一处理
 * 嵌入式原子原语, 而非在设备工程里补 libcall。
 */
#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_atomic_mux = portMUX_INITIALIZER_UNLOCKED;

bool __atomic_test_and_set(volatile void *ptr, int memorder) {
    (void)memorder;
    portENTER_CRITICAL(&s_atomic_mux);
    volatile unsigned char *byte = (volatile unsigned char *)ptr;
    unsigned char previous = *byte;
    *byte = 1;
    portEXIT_CRITICAL(&s_atomic_mux);
    return previous != 0;
}

void __atomic_clear(volatile void *ptr, int memorder) {
    (void)memorder;
    portENTER_CRITICAL(&s_atomic_mux);
    volatile unsigned char *byte = (volatile unsigned char *)ptr;
    *byte = 0;
    portEXIT_CRITICAL(&s_atomic_mux);
}
