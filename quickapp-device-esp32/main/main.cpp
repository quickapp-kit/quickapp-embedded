/**
 * QuickApp Device — ESP32-S3 嵌入式运行时入口
 *
 * 启动流程:
 *   1. LVGL 初始化
 *   2. ILI9341 显示驱动 + XPT2046 触摸驱动
 *   3. SPIFFS 挂载 → 读取 RPK 包
 *   4. QuickApp 运行时初始化 (Core + JS + LVGL Host)
 *   5. 主循环: LVGL tick + runtime service
 */

#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include <lvgl.h>

#include "display_driver.h"
#include "touch_driver.h"
#include "spiffs_package_source.h"

// QuickApp Runtime headers
#include "quickapp/core/package/package_loader.h"
#include "quickapp/core/foundation/app_runtime_factory.h"
#include "quickapp/core/feature/module_registry.h"
#include "quickapp/core/event/event_router.h"
#include "quickapp/core/render/initial_render_pipeline.h"
#include "quickapp/core/surface/surface_controller.h"
#include "quickapp/core/timer/timer_registry.h"
#include "quickapp/js/abi/runtime_abi_service.h"
#include "quickapp/js/engine/js_engine_service.h"
#include "quickapp/js/engine/quickjs_engine_provider.h"
#include "quickapp/js/framework/static_facade_catalog.h"
#include "quickapp/js/module/module_loader.h"
#include "quickapp/js/vm/vm_lifecycle_service.h"
#include "quickapp/lvgl/font/system_default_font_asset.h"
#include "quickapp/lvgl/foundation/owner_task_queue.h"
#include "quickapp/lvgl/integration/core_mount_bridge.h"
#include "quickapp/lvgl/measure/font_measure.h"
#include "quickapp/lvgl/mount/lvgl_mount_backend.h"
#include "quickapp/lvgl/mount/mount_host.h"
#include "quickapp/lvgl/surface/lvgl_page_root_backend.h"
#include "quickapp/lvgl/surface/surface_host.h"
#include "quickapp/lvgl/backends/embedded_backends.h"

static const char* TAG = "quickapp_main";

namespace qc = quickapp::core;
namespace qp = quickapp::core::package;
namespace qs = quickapp::core::surface;
namespace qj = quickapp::js;
namespace qlf = quickapp::lvgl::foundation;
namespace qli = quickapp::lvgl::integration;
namespace qlm = quickapp::lvgl::mount;
namespace qls = quickapp::lvgl::surface;
namespace qm = quickapp::lvgl::measure;
namespace qlb = quickapp::lvgl::backends;

// --- Runtime Composition (与 runtime_composition.h 对齐) ---

static qp::RuntimeComposition makeRuntimeComposition() {
  std::set<std::string, std::less<>> components{
      "View", "Text", "Button", "Image", "Input",
      "Switch", "Slider", "Picker", "List", "Scroll", "Tabs"};
  std::set<std::string, std::less<>> capabilities{
      "system.prompt", "system.router", "system.device"};
  return {"quickapp-kit-runtime-v1", "quickapp-kit-js-engine-v1",
          std::move(components), std::move(capabilities)};
}

// --- LVGL Tick Timer ---

static void lvgl_tick_cb(void* /*arg*/) {
  lv_tick_inc(1);  // 1ms per tick
}

static esp_timer_handle_t s_lvgl_tick_timer = nullptr;

static void start_lvgl_tick_timer() {
  esp_timer_create_args_t timer_args = {};
  timer_args.callback = lvgl_tick_cb;
  timer_args.name = "lvgl_tick";
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(s_lvgl_tick_timer, 1000));  // 1ms
}

// --- Embedded Loop Callbacks (FreeRTOS-based) ---

static std::uint64_t loop_now_ns(void* /*ctx*/) noexcept {
  return static_cast<std::uint64_t>(esp_timer_get_time()) * 1000ULL;  // us → ns
}

static std::uint64_t loop_resolution_ns(void* /*ctx*/) noexcept {
  return 1000000ULL;  // 1ms
}

static qlf::WakeResult loop_notify(void* /*ctx*/) noexcept {
  return qlf::WakeResult::kNotified;
}

static qlf::WakeResult loop_wait_until(void* /*ctx*/,
                                        std::uint64_t deadline_ns) noexcept {
  const std::uint64_t now = loop_now_ns(nullptr);
  if (deadline_ns > now) {
    const std::uint64_t delay_us = (deadline_ns - now) / 1000ULL;
    if (delay_us > 0) {
      // 让出 CPU 给 FreeRTOS 调度器
      vTaskDelay(pdMS_TO_TICKS(delay_us / 1000));
    }
  }
  return qlf::WakeResult::kDeadline;
}

static std::size_t loop_service(void* /*ctx*/,
                                std::size_t /*max_callbacks*/) noexcept {
  // LVGL handler: 处理渲染、动画、输入
  uint32_t time_till_next = lv_timer_handler();
  (void)time_till_next;
  return 1;
}

static qlf::LocalResult loop_close(void* /*ctx*/) noexcept {
  return qlf::LocalResult::success();
}

// --- Main Entry ---

extern "C" void app_main() {
  ESP_LOGI(TAG, "=== QuickApp Device Starting ===");
  ESP_LOGI(TAG, "Free heap: %lu bytes, free PSRAM: %lu bytes",
           (unsigned long)esp_get_free_heap_size(),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  // 1. 初始化 LVGL
  lv_init();
  start_lvgl_tick_timer();
  ESP_LOGI(TAG, "LVGL initialized");

  // 2. 初始化显示驱动
  lv_display_t* display = display_driver_init();
  if (display == nullptr) {
    ESP_LOGE(TAG, "Display init failed!");
    return;
  }

  // 3. 初始化触摸驱动
  lv_indev_t* indev = touch_driver_init(display);
  if (indev == nullptr) {
    ESP_LOGW(TAG, "Touch init failed, continuing without touch");
  }

  // 4. 加载 RPK 包
  ESP_LOGI(TAG, "Loading RPK from SPIFFS...");
  auto rpk_source = quickapp::device::SpiffsPackageSource::create(
      "rpk_store", "app.rpk");

  if (rpk_source == nullptr) {
    ESP_LOGE(TAG, "Cannot load RPK! Showing fallback screen...");

    // 显示一个简单的提示界面
    lv_obj_t* label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "No RPK found.\n\n"
                             "Please flash app.rpk to\n"
                             "the rpk_store partition.");
    lv_obj_center(label);

    // 进入简单的 LVGL 循环
    while (true) {
      lv_timer_handler();
      vTaskDelay(pdMS_TO_TICKS(16));
    }
    return;  // unreachable
  }

  // 5. 创建 PackageLoader
  auto composition = makeRuntimeComposition();
  qc::AppRuntimeFactory runtime_factory;
  auto identity_result = runtime_factory.create();
  if (!identity_result) {
    const auto message = identity_result.error().message;
    ESP_LOGE(TAG, "App runtime identity creation failed: %.*s",
             static_cast<int>(message.size()), message.data());
    return;
  }
  auto runtime_identity = std::move(identity_result).value();

  auto loader_result = qp::PackageLoader::create(
      rpk_source, runtime_identity.request_ids(), composition);
  if (!loader_result) {
    ESP_LOGE(TAG, "PackageLoader creation failed");
    return;
  }
  auto loader = std::move(loader_result).value();
  ESP_LOGI(TAG, "PackageLoader created");

  // 6. 验证并打开包
  // 同步加载: 在嵌入式环境中 completion 是同步调用的
  std::shared_ptr<const qp::VerifiedPackage> package;
  auto open_result = loader->open([&](auto result) {
    if (result) {
      package = std::move(result).value();
    } else {
      const auto message = result.error().message;
      ESP_LOGE(TAG, "RPK verification failed: %.*s",
               static_cast<int>(message.size()), message.data());
    }
  });

  if (!open_result) {
    const auto message = open_result.error().message;
    ESP_LOGE(TAG, "RPK open request failed: %.*s",
             static_cast<int>(message.size()), message.data());
    return;
  }
  if (!package) {
    ESP_LOGE(TAG, "RPK verification failed or no package produced");
    return;
  }

  ESP_LOGI(TAG, "RPK verified: package=%s, entry=%s",
           package->package_id().c_str(), package->entry_route().c_str());

  // 7. 设置 embedded loop backend
  qlb::BuiltinLoopCallbacks loop_callbacks{};
  loop_callbacks.context = nullptr;
  loop_callbacks.now_ns = loop_now_ns;
  loop_callbacks.resolution_ns = loop_resolution_ns;
  loop_callbacks.notify = loop_notify;
  loop_callbacks.wait_until = loop_wait_until;
  loop_callbacks.service = loop_service;
  loop_callbacks.close = loop_close;

  ESP_LOGI(TAG, "Runtime backends configured");

  // 8. 主循环
  // 在嵌入式平台，主循环就是不断地:
  //   - 调用 lv_timer_handler() 处理 LVGL 渲染和动画
  //   - 处理运行时事件队列
  //   - 让出 CPU 等待下一帧
  ESP_LOGI(TAG, "=== Entering main loop ===");
  ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

  while (true) {
    // LVGL 主循环处理
    uint32_t time_till_next = lv_timer_handler();

    // 限制最小延时，避免忙等
    uint32_t delay_ms = time_till_next;
    if (delay_ms < 5) delay_ms = 5;
    if (delay_ms > 50) delay_ms = 50;

    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }

  // Cleanup (unreachable in normal operation)
  rpk_source->close();
  touch_driver_deinit();
  display_driver_deinit();
  lv_deinit();
}
