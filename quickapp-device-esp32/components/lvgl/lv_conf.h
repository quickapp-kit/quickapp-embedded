/**
 * LVGL 配置文件 (ESP32-S3 嵌入式环境)
 * 基于 LVGL v9.x 配置
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Color depth: 16-bit (RGB565) for ILI9341 */
#define LV_COLOR_DEPTH 16

/* Memory: 使用 ESP32 heap 管理 */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC malloc
#define LV_MEM_CUSTOM_FREE free
#define LV_MEM_CUSTOM_REALLOC realloc

/* 显示刷新周期 (ms) — 约 60fps */
#define LV_DISPLAY_DEF_REFR_PERIOD 16

/* Input device read period (ms) */
#define LV_DEF_REFR_PERIOD 16
#define LV_INDEV_DEF_READ_PERIOD 30

/* 启用必要的 widgets */
#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_IMG 1
#define LV_USE_LINE 1
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TEXTAREA 1
#define LV_USE_TABLE 1
#define LV_USE_ROLLER 1
#define LV_USE_DROPDOWN 1
#define LV_USE_CHECKBOX 1

/* Layout */
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* 内置字体 */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* 禁用不需要的功能 (节省 Flash) */
#define LV_USE_ANIMIMG 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_METER 0
#define LV_USE_MSGBOX 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_TABVIEW 1
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

/* 禁用文件系统 (我们自己管理包加载) */
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0

/* 日志 */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN

/* 性能: 关闭断言 (release 模式) */
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

/* Tick: 外部提供 (通过 esp_timer) */
#define LV_TICK_CUSTOM 0

/* 渲染: 使用软件渲染 */
#define LV_USE_DRAW_SW 1

/* PNG 解码 (sport-watch 有 PNG 图片资源; runtime 的 mount_host 直接调 lodepng_decode32) */
#define LV_USE_LODEPNG 1

#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 0
#define LV_TINY_TTF_CACHE_GLYPH_CNT 64

#endif /* LV_CONF_H */
