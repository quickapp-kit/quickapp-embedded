/**
 * QuickApp Device — 最小硬件验证程序 (Milestone B)
 *
 * 目的: 不集成 QuickJS/runtime, 只验证硬件通路:
 *   ESP-IDF + LVGL + ILI9341 显示 + XPT2046 触摸
 *
 * 成功标准:
 *   1. 屏幕显示 UI (标题 + 按钮 + 计数器)
 *   2. 触摸按钮有响应 (计数器 +1)
 *   3. 触摸坐标实时显示
 */

#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <lvgl.h>

#include "display_driver.h"
#include "touch_driver.h"

static const char* TAG = "qa_minimal";

// LVGL tick 定时器
static void lvgl_tick_cb(void* /*arg*/) { lv_tick_inc(1); }

static void start_lvgl_tick_timer() {
  esp_timer_create_args_t timer_args = {};
  timer_args.callback = lvgl_tick_cb;
  timer_args.name = "lvgl_tick";
  esp_timer_handle_t timer;
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 1000));  // 1ms
}

// UI 元素 (全局, 供事件回调访问)
static lv_obj_t* s_counter_label = nullptr;
static lv_obj_t* s_touch_label = nullptr;
static int s_counter = 0;

// 按钮点击事件
static void btn_event_cb(lv_event_t* e) {
  (void)e;
  s_counter++;
  lv_label_set_text_fmt(s_counter_label, "Clicks: %d", s_counter);
  ESP_LOGI(TAG, "Button clicked, count=%d", s_counter);
}

// 触摸坐标实时更新 (LVGL 定时器)
static void touch_monitor_cb(lv_timer_t* /*t*/) {
  lv_indev_t* indev = lv_indev_get_next(nullptr);
  if (indev == nullptr) return;

  lv_point_t p;
  lv_indev_get_point(indev, &p);
  lv_indev_state_t state = lv_indev_get_state(indev);

  if (state == LV_INDEV_STATE_PRESSED) {
    lv_label_set_text_fmt(s_touch_label, "Touch: %d, %d", (int)p.x, (int)p.y);
  }
}

// 构建测试 UI
static void build_ui() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x102030), 0);

  // 标题
  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "QuickApp Device\nHardware Test");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  // 计数器标签
  s_counter_label = lv_label_create(scr);
  lv_label_set_text(s_counter_label, "Clicks: 0");
  lv_obj_set_style_text_color(s_counter_label, lv_color_hex(0x00FF88), 0);
  lv_obj_align(s_counter_label, LV_ALIGN_CENTER, 0, -40);

  // 按钮
  lv_obj_t* btn = lv_button_create(scr);
  lv_obj_set_size(btn, 160, 60);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 20);
  lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* btn_label = lv_label_create(btn);
  lv_label_set_text(btn_label, "TAP ME");
  lv_obj_center(btn_label);

  // 触摸坐标标签
  s_touch_label = lv_label_create(scr);
  lv_label_set_text(s_touch_label, "Touch: --, --");
  lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0xFFAA00), 0);
  lv_obj_align(s_touch_label, LV_ALIGN_BOTTOM_MID, 0, -20);

  // 触摸监控定时器
  lv_timer_create(touch_monitor_cb, 100, nullptr);
}

extern "C" void app_main() {
  ESP_LOGI(TAG, "=== QuickApp Device Hardware Test (Milestone B) ===");
  ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

  // 1. LVGL
  lv_init();
  start_lvgl_tick_timer();
  ESP_LOGI(TAG, "LVGL initialized");

  // 2. 显示
  lv_display_t* display = display_driver_init();
  if (display == nullptr) {
    ESP_LOGE(TAG, "Display init FAILED");
    return;
  }
  ESP_LOGI(TAG, "Display OK");

  // 3. 触摸
  lv_indev_t* indev = touch_driver_init(display);
  if (indev == nullptr) {
    ESP_LOGW(TAG, "Touch init failed (display still works)");
  } else {
    ESP_LOGI(TAG, "Touch OK");
  }

  // 4. 构建 UI
  build_ui();
  ESP_LOGI(TAG, "UI built, entering main loop");

  // 5. 主循环
  while (true) {
    uint32_t next = lv_timer_handler();
    if (next < 5) next = 5;
    if (next > 50) next = 50;
    vTaskDelay(pdMS_TO_TICKS(next));
  }
}
