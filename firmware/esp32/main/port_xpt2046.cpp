// XPT2046 resistive touch controller on its own SPI bus (the Cheap Yellow
// Display wires it to pins separate from the LCD). A task polls it every 20 ms
// and posts Touch events in panel pixels; the app task feeds them to
// hg::TouchGestures (hold to talk, tap for yes, swipe down to cancel).
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {
namespace {

const char* TAG = "hg.xpt";
constexpr uint32_t kPollMs = 20;
constexpr int kSamples = 4;  // readings averaged per report (resistive panels are noisy)
// 12-bit conversions, differential reference, PD=00: the pen interrupt stays enabled.
constexpr uint8_t kReadX = 0xD0;
constexpr uint8_t kReadY = 0x90;
constexpr uint8_t kReadZ1 = 0xB0;
constexpr uint8_t kReadZ2 = 0xC0;

int map_axis(int raw, int lo, int hi, int size) {
  if (hi == lo) return 0;
  int v = (raw - lo) * (size - 1) / (hi - lo);
  return std::clamp(v, 0, size - 1);
}

}  // namespace

bool XptTouch::begin(const XptTouchConfig& cfg) {
  cfg_ = cfg;
  const auto host = static_cast<spi_host_device_t>(cfg.spi_host);
  spi_bus_config_t bus = {};
  bus.mosi_io_num = cfg.mosi;
  bus.miso_io_num = cfg.miso;
  bus.sclk_io_num = cfg.sclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 16;
  if (spi_bus_initialize(host, &bus, SPI_DMA_DISABLED) != ESP_OK) {
    ESP_LOGE(TAG, "SPI bus for touch failed");
    return false;
  }
  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = 2 * 1000 * 1000;
  dev.mode = 0;
  dev.spics_io_num = cfg.cs;
  dev.queue_size = 1;
  spi_device_handle_t handle = nullptr;
  if (spi_bus_add_device(host, &dev, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "touch controller not added");
    return false;
  }
  dev_ = handle;
  if (cfg.irq >= 0) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << cfg.irq;
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);
  }
  read12(kReadX);  // the first conversion after power-up enables the pen interrupt
  xTaskCreate(&XptTouch::task, "hg-xpt", 3072, this, 5, nullptr);
  ESP_LOGI(TAG, "XPT2046 touch ready");
  return true;
}

uint16_t XptTouch::read12(uint8_t command) {
  spi_transaction_t t = {};
  t.length = 24;
  t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
  t.tx_data[0] = command;
  if (spi_device_polling_transmit(static_cast<spi_device_handle_t>(dev_), &t) != ESP_OK) return 0;
  return static_cast<uint16_t>(((t.rx_data[1] << 8) | t.rx_data[2]) >> 3) & 0x0FFF;
}

bool XptTouch::sample(TouchSample& out) {
  // The pen interrupt (active low) says whether anything presses the panel.
  if (cfg_.irq >= 0 && gpio_get_level(static_cast<gpio_num_t>(cfg_.irq)) != 0) {
    out = {false, 0, 0};
    return true;
  }
  int z = 4095 + read12(kReadZ1) - read12(kReadZ2);
  if (z < cfg_.pressure_min) {
    out = {false, 0, 0};
    return true;
  }
  int sx = 0, sy = 0;
  for (int i = 0; i < kSamples; ++i) {
    sx += read12(kReadX);
    sy += read12(kReadY);
  }
  int rx = sx / kSamples, ry = sy / kSamples;
  if (cfg_.swap_xy) std::swap(rx, ry);
  int x = map_axis(rx, cfg_.x_min, cfg_.x_max, cfg_.width);
  int y = map_axis(ry, cfg_.y_min, cfg_.y_max, cfg_.height);
  if (cfg_.mirror_x) x = cfg_.width - 1 - x;
  if (cfg_.mirror_y) y = cfg_.height - 1 - y;
  out = {true, static_cast<int16_t>(x), static_cast<int16_t>(y)};
  return true;
}

void XptTouch::task(void* arg) {
  auto* self = static_cast<XptTouch*>(arg);
  bool was_touching = false;
  for (;;) {
    TouchSample s{};
    if (self->sample(s)) {
      // Every sample while the finger is down (gestures need the motion), plus the lift.
      if (s.touching || was_touching) events::post(EventType::Touch, &s, sizeof(s));
      if (s.touching && !was_touching) ESP_LOGD(TAG, "touch at %d,%d", s.x, s.y);
      was_touching = s.touching;
    }
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
  }
}

}  // namespace hgp
