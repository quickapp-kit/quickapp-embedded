#include "touch_driver.h"

#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char* TAG = "touch";

// 从 Kconfig 读取引脚
#define TP_CS   CONFIG_QA_TP_CS
#define TP_IRQ  CONFIG_QA_TP_IRQ

// XPT2046 命令字节
// 通道选择 + 12-bit + 差分模式
#define XPT2046_CMD_X  0xD0  // X 坐标 (差分, 12-bit)
#define XPT2046_CMD_Y  0x90  // Y 坐标 (差分, 12-bit)
#define XPT2046_CMD_Z1 0xB0  // Z1 压力
#define XPT2046_CMD_Z2 0xC0  // Z2 压力

// 触摸压力阈值 (低于此值视为无触摸)
#define TOUCH_PRESSURE_THRESHOLD 50

// XPT2046 SPI 时钟: 最高 2.5MHz
#define TOUCH_SPI_FREQ_HZ (2 * 1000 * 1000)

// 屏幕尺寸 (用于坐标映射)
#define LCD_WIDTH  CONFIG_QA_DISPLAY_WIDTH
#define LCD_HEIGHT CONFIG_QA_DISPLAY_HEIGHT

// 触摸校准参数 (默认值, 实际使用时需通过校准程序确定)
// XPT2046 原始值范围大约 200~3900 (12-bit ADC, 有效区间)
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3900
#define TOUCH_Y_MIN 200
#define TOUCH_Y_MAX 3900

static spi_device_handle_t s_touch_spi = nullptr;
static lv_indev_t* s_indev = nullptr;
static volatile bool s_touch_pressed = false;
// 诊断: 触摸按下边沿锁存, 用于每次按下只打印一次坐标
static bool s_press_edge_latched = false;

// T_IRQ 中断处理: 低电平表示有触摸
static void IRAM_ATTR touch_irq_handler(void* arg) {
  s_touch_pressed =
      (gpio_get_level(static_cast<gpio_num_t>(TP_IRQ)) == 0);
}

// 读取 XPT2046 单通道 12-bit 值
static uint16_t xpt2046_read_channel(uint8_t cmd) {
  uint8_t tx_buf[3] = {cmd, 0x00, 0x00};
  uint8_t rx_buf[3] = {};

  spi_transaction_t trans = {};
  trans.length = 24;       // 3 bytes = 24 bits
  trans.tx_buffer = tx_buf;
  trans.rx_buffer = rx_buf;

  esp_err_t err = spi_device_transmit(s_touch_spi, &trans);
  if (err != ESP_OK) {
    return 0;
  }

  // 响应在第 2-3 字节, 高 12 位有效
  uint16_t raw = (static_cast<uint16_t>(rx_buf[1]) << 8) | rx_buf[2];
  raw >>= 3;  // 右移 3 位得到 12-bit 值
  return raw & 0x0FFF;
}

// 读取触摸坐标 (带简单滤波: 多次采样取中值)
static bool xpt2046_read_position(int16_t* out_x, int16_t* out_y) {
  // 先读压力判断是否有触摸
  uint16_t z1 = xpt2046_read_channel(XPT2046_CMD_Z1);
  uint16_t z2 = xpt2046_read_channel(XPT2046_CMD_Z2);

  // 压力计算: pressure = z1 (z2 越小压力越大)
  int pressure = z1;
  if (pressure < TOUCH_PRESSURE_THRESHOLD) {
    return false;  // 无触摸
  }

  // 多次采样取平均 (简单去抖)
  constexpr int SAMPLES = 4;
  uint32_t sum_x = 0, sum_y = 0;
  int valid = 0;

  for (int i = 0; i < SAMPLES; i++) {
    uint16_t raw_x = xpt2046_read_channel(XPT2046_CMD_X);
    uint16_t raw_y = xpt2046_read_channel(XPT2046_CMD_Y);

    if (raw_x > TOUCH_X_MIN && raw_x < TOUCH_X_MAX &&
        raw_y > TOUCH_Y_MIN && raw_y < TOUCH_Y_MAX) {
      sum_x += raw_x;
      sum_y += raw_y;
      valid++;
    }
  }

  if (valid == 0) {
    return false;
  }

  uint16_t avg_x = sum_x / valid;
  uint16_t avg_y = sum_y / valid;

  // 映射到屏幕坐标
  // 注意: 电阻触摸方向可能与屏幕相反, 需根据实际安装调整
  *out_x = static_cast<int16_t>(
      (long)(avg_x - TOUCH_X_MIN) * LCD_WIDTH / (TOUCH_X_MAX - TOUCH_X_MIN));
  *out_y = static_cast<int16_t>(
      (long)(avg_y - TOUCH_Y_MIN) * LCD_HEIGHT / (TOUCH_Y_MAX - TOUCH_Y_MIN));

  // 边界钳制
  if (*out_x < 0) *out_x = 0;
  if (*out_x >= LCD_WIDTH) *out_x = LCD_WIDTH - 1;
  if (*out_y < 0) *out_y = 0;
  if (*out_y >= LCD_HEIGHT) *out_y = LCD_HEIGHT - 1;

  return true;
}

// LVGL 触摸读取回调
static void touch_read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
  int16_t x = 0, y = 0;

  // 先检查 IRQ 引脚状态 (快速判断)
  if (TP_IRQ >= 0 && gpio_get_level(static_cast<gpio_num_t>(TP_IRQ)) != 0) {
    // IRQ 高电平 = 无触摸
    data->state = LV_INDEV_STATE_RELEASED;
    s_press_edge_latched = false;
    return;
  }

  if (xpt2046_read_position(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
    // 诊断: 仅在 released->pressed 边沿打印一次映射后坐标, 避免刷屏
    if (!s_press_edge_latched) {
      s_press_edge_latched = true;
      ESP_LOGI(TAG, "[TOUCH] press x=%d y=%d", (int)x, (int)y);
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
    s_press_edge_latched = false;
  }
}

// 诊断: 打印原始读数 (判断 SPI 触摸是否通信)
void touch_driver_debug_dump() {
  if (s_touch_spi == nullptr) {
    ESP_LOGW(TAG, "[DBG] touch spi not initialized");
    return;
  }
  int irq_level = (TP_IRQ >= 0)
                      ? gpio_get_level(static_cast<gpio_num_t>(TP_IRQ))
                      : -1;
  uint16_t z1 = xpt2046_read_channel(XPT2046_CMD_Z1);
  uint16_t z2 = xpt2046_read_channel(XPT2046_CMD_Z2);
  uint16_t x = xpt2046_read_channel(XPT2046_CMD_X);
  uint16_t y = xpt2046_read_channel(XPT2046_CMD_Y);
  ESP_LOGI(TAG, "[DBG] IRQ=%d Z1=%u Z2=%u X=%u Y=%u",
           irq_level, z1, z2, x, y);
}

lv_indev_t* touch_driver_init(lv_display_t* display) {
  ESP_LOGI(TAG, "Initializing XPT2046 touch (CS=%d, IRQ=%d)", TP_CS, TP_IRQ);

  // 配置 T_IRQ 引脚为输入 + 中断 (下降沿)
  if (TP_IRQ >= 0) {
    gpio_config_t irq_cfg = {};
    irq_cfg.pin_bit_mask = 1ULL << TP_IRQ;
    irq_cfg.mode = GPIO_MODE_INPUT;
    irq_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    irq_cfg.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&irq_cfg);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(static_cast<gpio_num_t>(TP_IRQ),
                         touch_irq_handler, nullptr);
    ESP_LOGI(TAG, "Touch IRQ configured on GPIO %d", TP_IRQ);
  }

  // 在已有的 SPI2_HOST 总线上添加 XPT2046 设备
  spi_device_interface_config_t dev_cfg = {};
  dev_cfg.clock_speed_hz = TOUCH_SPI_FREQ_HZ;
  dev_cfg.mode = 0;
  dev_cfg.spics_io_num = TP_CS;
  dev_cfg.queue_size = 3;
  // XPT2046 在 CS 拉高后需要一定时间, command_bits/address_bits 保持 0
  dev_cfg.command_bits = 0;
  dev_cfg.address_bits = 0;

  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_touch_spi));
  ESP_LOGI(TAG, "XPT2046 SPI device attached to SPI2_HOST");

  // 创建 LVGL indev
  s_indev = lv_indev_create();
  if (s_indev == nullptr) {
    ESP_LOGE(TAG, "lv_indev_create failed");
    return nullptr;
  }
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_indev, touch_read_cb);
  lv_indev_set_display(s_indev, display);

  ESP_LOGI(TAG, "LVGL touch indev ready (XPT2046 resistive)");
  return s_indev;
}

void touch_driver_deinit() {
  if (TP_IRQ >= 0) {
    gpio_isr_handler_remove(static_cast<gpio_num_t>(TP_IRQ));
  }
  if (s_touch_spi != nullptr) {
    spi_bus_remove_device(s_touch_spi);
    s_touch_spi = nullptr;
  }
  s_indev = nullptr;
}
