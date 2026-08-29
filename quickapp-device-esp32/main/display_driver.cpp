#include "display_driver.h"

#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char* TAG = "display";

// 从 Kconfig 读取引脚配置
#define LCD_MOSI  CONFIG_QA_LCD_MOSI
#define LCD_MISO  CONFIG_QA_LCD_MISO
#define LCD_SCLK  CONFIG_QA_LCD_SCLK
#define LCD_CS    CONFIG_QA_LCD_CS
#define LCD_DC    CONFIG_QA_LCD_DC
#define LCD_RST   CONFIG_QA_LCD_RST
#define LCD_BL    CONFIG_QA_LCD_BL

#define LCD_WIDTH  CONFIG_QA_DISPLAY_WIDTH
#define LCD_HEIGHT CONFIG_QA_DISPLAY_HEIGHT

// ILI9341 SPI 时钟频率: 写操作最高 40MHz
#define LCD_SPI_FREQ_HZ (40 * 1000 * 1000)

// LVGL 部分刷新缓冲区行数
#define LVGL_BUF_LINES 40

static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_display_t* s_display = nullptr;
static bool s_spi_bus_initialized = false;

// LVGL flush callback: 将渲染好的像素推送到屏幕
static void flush_cb(lv_display_t* disp, const lv_area_t* area,
                     uint8_t* px_map) {
  const int x_start = area->x1;
  const int y_start = area->y1;
  const int x_end = area->x2 + 1;
  const int y_end = area->y2 + 1;

  esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, px_map);

  lv_display_flush_ready(disp);
}

lv_display_t* display_driver_init() {
  ESP_LOGI(TAG, "Initializing ILI9341 display %dx%d", LCD_WIDTH, LCD_HEIGHT);

  // 背光控制
  if (LCD_BL >= 0) {
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = 1ULL << LCD_BL;
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&bl_cfg);
    gpio_set_level(static_cast<gpio_num_t>(LCD_BL), 1);  // 高电平点亮
  }

  // SPI 总线初始化 (LCD + Touch 共享)
  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num = LCD_MOSI;
  bus_cfg.miso_io_num = LCD_MISO;
  bus_cfg.sclk_io_num = LCD_SCLK;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = LCD_WIDTH * LVGL_BUF_LINES * 2;  // 16-bit color
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
  s_spi_bus_initialized = true;

  // LCD panel IO (SPI 设备)
  esp_lcd_panel_io_handle_t io_handle = nullptr;
  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.dc_gpio_num = LCD_DC;
  io_config.cs_gpio_num = LCD_CS;
  io_config.pclk_hz = LCD_SPI_FREQ_HZ;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  io_config.spi_mode = 0;
  io_config.trans_queue_depth = 10;
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io_handle));

  // ILI9341 面板驱动 (esp_lcd 内置支持)
  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = LCD_RST;
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
  panel_config.bits_per_pixel = 16;
  // ILI9341 使用 esp_lcd_new_panel_st7789 兼容接口 (同为 8080/SPI 命令集兼容)
  // 如果 ESP-IDF >= 5.3 有 esp_lcd_new_panel_ili9341 可直接用
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel));

  // 复位并初始化
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

  // ILI9341 需要反色
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
  // 设置显示方向 (竖屏)
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, false));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
  // 开启显示
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

  ESP_LOGI(TAG, "ILI9341 panel initialized");

  // 创建 LVGL display
  s_display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  if (s_display == nullptr) {
    ESP_LOGE(TAG, "lv_display_create failed");
    return nullptr;
  }

  // 分配双缓冲 (DMA capable 内部 SRAM)
  const size_t buf_size = LCD_WIDTH * LVGL_BUF_LINES * sizeof(lv_color16_t);
  auto* buf1 = static_cast<uint8_t*>(heap_caps_malloc(buf_size, MALLOC_CAP_DMA));
  auto* buf2 = static_cast<uint8_t*>(heap_caps_malloc(buf_size, MALLOC_CAP_DMA));
  if (buf1 == nullptr || buf2 == nullptr) {
    ESP_LOGE(TAG, "Display buffer allocation failed (need %u bytes x2)",
             (unsigned)buf_size);
    return nullptr;
  }

  lv_display_set_buffers(s_display, buf1, buf2, buf_size,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(s_display, flush_cb);

  ESP_LOGI(TAG, "LVGL display ready, buf=%u bytes x2", (unsigned)buf_size);
  return s_display;
}

void display_driver_deinit() {
  if (s_panel != nullptr) {
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_del(s_panel);
    s_panel = nullptr;
  }
  if (s_spi_bus_initialized) {
    spi_bus_free(SPI2_HOST);
    s_spi_bus_initialized = false;
  }
  s_display = nullptr;
}

void* display_driver_get_spi_host() {
  // 返回 SPI2_HOST 的指针值 (实际是 int, 用于 touch driver 添加设备)
  // touch driver 通过同一个 SPI_HOST 添加自己的 CS 设备
  return reinterpret_cast<void*>(SPI2_HOST);
}
