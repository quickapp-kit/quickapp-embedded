#pragma once

#include <lvgl.h>

/// 初始化 ILI9341 SPI 显示驱动 + LVGL display
/// 返回创建的 lv_display_t*，失败返回 nullptr
lv_display_t* display_driver_init();

/// 关闭显示驱动
void display_driver_deinit();

/// 获取 SPI 总线句柄 (触摸驱动共享同一 SPI 总线)
/// 必须在 display_driver_init() 之后调用
void* display_driver_get_spi_host();
