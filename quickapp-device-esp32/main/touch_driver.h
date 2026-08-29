#pragma once

#include <lvgl.h>

/// 初始化 XPT2046 SPI 电阻触摸驱动 + LVGL indev
/// 必须在 display_driver_init() 之后调用 (共享 SPI 总线)
/// 返回创建的 lv_indev_t*，失败返回 nullptr
lv_indev_t* touch_driver_init(lv_display_t* display);

/// 关闭触摸驱动
void touch_driver_deinit();
