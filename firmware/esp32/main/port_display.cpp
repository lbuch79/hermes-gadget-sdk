// SPI ST7789 / ILI9341 panel via esp_lcd. The framebuffer lives in PSRAM when
// there is some; rows are copied through a small DMA-capable bounce buffer on
// flush.
//
// Boards without PSRAM (the classic ESP32 "Cheap Yellow Display") can't hold a
// full 320x240 frame in internal RAM, so `fb_scale` 2 keeps a quarter-size
// framebuffer and doubles every pixel on its way to the panel. The UI lays
// itself out for the smaller size.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

namespace hgp {
namespace {

const char* TAG = "hg.lcd";
constexpr int kBounceRows = 20;  // panel rows per DMA transfer
constexpr ledc_channel_t kBlChannel = LEDC_CHANNEL_0;

// Reads `n` bytes after command `cmd` with CS and DC driven by hand, before
// esp_lcd owns the bus. Returns false when nothing answers (MISO floats high).
bool read_id(spi_host_device_t host, const LcdConfig& cfg, uint8_t cmd, uint8_t* out, size_t n) {
  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = 4 * 1000 * 1000;
  dev.mode = 0;
  dev.spics_io_num = -1;
  dev.queue_size = 1;
  spi_device_handle_t h = nullptr;
  if (spi_bus_add_device(host, &dev, &h) != ESP_OK) return false;
  gpio_reset_pin(static_cast<gpio_num_t>(cfg.cs));
  gpio_set_direction(static_cast<gpio_num_t>(cfg.cs), GPIO_MODE_OUTPUT);
  gpio_reset_pin(static_cast<gpio_num_t>(cfg.dc));
  gpio_set_direction(static_cast<gpio_num_t>(cfg.dc), GPIO_MODE_OUTPUT);
  gpio_set_level(static_cast<gpio_num_t>(cfg.cs), 0);
  gpio_set_level(static_cast<gpio_num_t>(cfg.dc), 0);
  spi_transaction_t t = {};
  t.length = 8;
  t.flags = SPI_TRANS_USE_TXDATA;
  t.tx_data[0] = cmd;
  spi_device_polling_transmit(h, &t);
  gpio_set_level(static_cast<gpio_num_t>(cfg.dc), 1);
  uint8_t tx[8] = {}, rx[8] = {};
  if (n) {
    spi_transaction_t r = {};
    r.length = 8 * n;
    r.tx_buffer = tx;
    r.rx_buffer = rx;
    spi_device_polling_transmit(h, &r);
  }
  gpio_set_level(static_cast<gpio_num_t>(cfg.cs), 1);
  spi_bus_remove_device(h);
  std::memcpy(out, rx, n);
  bool all_ff = true, all_00 = true;
  for (size_t i = 0; i < n; ++i) {
    all_ff &= rx[i] == 0xFF;
    all_00 &= rx[i] == 0x00;
  }
  return !all_ff && !all_00;
}

// Asks the panel what it is. Software-resets it first so it answers reads.
// ILI9341: RDID4 (0xD3) = dummy, 0x00, 0x93, 0x41. ST7789: RDDID (0x04) =
// 0x85 0x85 0x52 behind a one-clock dummy, so the bytes come shifted.
void detect_controller(spi_host_device_t host, LcdConfig& cfg) {
  if (cfg.miso < 0) return;
  uint8_t none[1];
  read_id(host, cfg, 0x01, none, 0);  // SWRESET
  vTaskDelay(pdMS_TO_TICKS(150));
  uint8_t d3[4] = {}, id[4] = {};
  bool got_d3 = read_id(host, cfg, 0xD3, d3, 4);
  bool got_id = read_id(host, cfg, 0x04, id, 4);
  uint8_t da[2] = {}, db[2] = {}, dc[2] = {}, st[5] = {};
  read_id(host, cfg, 0xDA, da, 2);
  read_id(host, cfg, 0xDB, db, 2);
  read_id(host, cfg, 0xDC, dc, 2);
  read_id(host, cfg, 0x09, st, 5);
  ESP_LOGI(TAG, "panel RDID1-3: %02x %02x | %02x %02x | %02x %02x; RDDST %02x %02x %02x %02x %02x", da[0], da[1],
           db[0], db[1], dc[0], dc[1], st[0], st[1], st[2], st[3], st[4]);
  ESP_LOGI(TAG, "panel ID: D3=%02x %02x %02x %02x, 04=%02x %02x %02x %02x", d3[0], d3[1], d3[2], d3[3], id[0],
           id[1], id[2], id[3]);
  if (got_d3 && d3[2] == 0x93 && d3[3] == 0x41) {
    if (cfg.controller != LcdController::Ili9341) ESP_LOGW(TAG, "panel answers as an ILI9341; using that driver");
    cfg.controller = LcdController::Ili9341;
    return;
  }
  // Undo the dummy clock: shift the 32 bits left by one.
  uint32_t v = (uint32_t(id[0]) << 24) | (uint32_t(id[1]) << 16) | (uint32_t(id[2]) << 8) | id[3];
  uint32_t shifted = (v << 1) >> 8;
  // An ILI9341 always answers RDID4 with 93 41. A panel that answers RDDID but
  // not that is the ST7789 of the two-USB 2432S028 revision (seen answering
  // RDID1-3 with 81 81 B3), which takes no inversion and the same orientation.
  if (got_id || shifted == 0x858552) {
    if (cfg.controller != LcdController::St7789) ESP_LOGW(TAG, "panel isn't an ILI9341; using the ST7789 driver");
    cfg.controller = LcdController::St7789;
    cfg.invert = false;
    cfg.bgr = false;
    // In landscape (swap_xy) the panel's row-address bit flips the image left
    // to right; this panel needs it set to read correctly (confirmed on hardware).
    cfg.mirror_y = !cfg.mirror_y;
    return;
  }
  ESP_LOGW(TAG, "panel ID not recognised; keeping the board's %s setting",
           cfg.controller == LcdController::Ili9341 ? "ILI9341" : "ST7789");
}

}  // namespace

bool SpiDisplay::on_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<SpiDisplay*>(ctx)->done_, &woken);
  return woken == pdTRUE;
}

bool SpiDisplay::begin(const LcdConfig& cfg) {
  cfg_ = cfg;
  scale_ = std::max<int>(1, cfg.fb_scale);
  fb_w_ = static_cast<uint16_t>(cfg.width / scale_);
  fb_h_ = static_cast<uint16_t>(cfg.height / scale_);
  const size_t px = static_cast<size_t>(fb_w_) * fb_h_;
  fb_ = static_cast<uint16_t*>(heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!fb_) fb_ = static_cast<uint16_t*>(heap_caps_malloc(px * 2, MALLOC_CAP_8BIT));
  bounce_rows_ = kBounceRows - kBounceRows % scale_;
  bounce_ = static_cast<uint16_t*>(heap_caps_malloc(static_cast<size_t>(cfg.width) * bounce_rows_ * 2, MALLOC_CAP_DMA));
  if (!fb_ || !bounce_) {
    ESP_LOGE(TAG, "not enough memory for a %ux%u framebuffer", fb_w_, fb_h_);
    return false;
  }
  std::memset(fb_, 0, px * 2);
  done_ = xSemaphoreCreateBinary();

  const auto host = static_cast<spi_host_device_t>(cfg.spi_host);
  spi_bus_config_t bus = {};
  bus.mosi_io_num = cfg.mosi;
  bus.miso_io_num = cfg.miso;
  bus.sclk_io_num = cfg.sclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = cfg.width * bounce_rows_ * 2;
  ESP_ERROR_CHECK(spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO));
  detect_controller(host, cfg_);
  const LcdConfig& c = cfg_;

  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.dc_gpio_num = static_cast<gpio_num_t>(cfg.dc);
  io_cfg.cs_gpio_num = static_cast<gpio_num_t>(cfg.cs);
  io_cfg.pclk_hz = static_cast<uint32_t>(cfg.spi_mhz) * 1000 * 1000;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;
  io_cfg.spi_mode = 0;
  io_cfg.trans_queue_depth = 4;
  io_cfg.on_color_trans_done = &SpiDisplay::on_trans_done;
  io_cfg.user_ctx = this;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(host), &io_cfg, &io_));

  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = static_cast<gpio_num_t>(cfg.rst);
  panel_cfg.rgb_ele_order = c.bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB;
  panel_cfg.bits_per_pixel = 16;
  if (c.controller == LcdController::Ili9341) {
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_, &panel_cfg, &panel_));
  } else {
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_, &panel_cfg, &panel_));
  }
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, c.invert));
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, c.swap_xy));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, c.mirror_x, c.mirror_y));
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_, cfg.gap_x, cfg.gap_y));
  flush(0, fb_h_);  // clear whatever the panel powered up with
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
  if (c.miso >= 0) {
    // RDDPM (0x0A): bit 7 booster on, bit 4 sleep out, bit 2 display on. Dummy byte first.
    uint8_t pm[2] = {}, mad[2] = {}, pix[2] = {};
    esp_lcd_panel_io_rx_param(io_, 0x0A, pm, 2);
    esp_lcd_panel_io_rx_param(io_, 0x0B, mad, 2);
    esp_lcd_panel_io_rx_param(io_, 0x0C, pix, 2);
    ESP_LOGI(TAG, "after init: RDDPM %02x %02x, MADCTL %02x %02x, COLMOD %02x %02x", pm[0], pm[1], mad[0], mad[1],
             pix[0], pix[1]);
  }

  if (cfg.backlight >= 0) {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch = {};
    ch.gpio_num = cfg.backlight;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = kBlChannel;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    set_backlight(100);
  }
  ESP_LOGI(TAG, "%s %ux%u ready (framebuffer %ux%u)",
           c.controller == LcdController::Ili9341 ? "ILI9341" : "ST7789", cfg.width, cfg.height, fb_w_, fb_h_);
  return true;
}

hg::DisplayInfo SpiDisplay::info() const {
  hg::DisplayInfo di;
  di.width = fb_w_;
  di.height = fb_h_;
  di.swap_bytes = true;  // the panel wants big-endian RGB565
  di.has_backlight = cfg_.backlight >= 0;
  return di;
}

void SpiDisplay::flush(uint16_t y0, uint16_t y1) {
  const int w = cfg_.width;
  if (scale_ == 1) {
    for (int y = y0; y < y1; y += bounce_rows_) {
      int rows = std::min<int>(bounce_rows_, y1 - y);
      std::memcpy(bounce_, fb_ + static_cast<size_t>(y) * w, static_cast<size_t>(rows) * w * 2);
      esp_lcd_panel_draw_bitmap(panel_, 0, y, w, y + rows, bounce_);
      // The bounce buffer is reused: wait until the DMA transfer has finished.
      xSemaphoreTake(done_, pdMS_TO_TICKS(100));
    }
    return;
  }
  // Scaled: each framebuffer row becomes `scale_` panel rows of repeated pixels.
  const int s = scale_;
  const int src_rows = bounce_rows_ / s;
  for (int y = y0; y < y1; y += src_rows) {
    int rows = std::min<int>(src_rows, y1 - y);
    uint16_t* out = bounce_;
    for (int r = 0; r < rows; ++r) {
      const uint16_t* src = fb_ + static_cast<size_t>(y + r) * fb_w_;
      uint16_t* line = out;
      for (int x = 0; x < fb_w_; ++x) {
        uint16_t p = src[x];
        for (int k = 0; k < s; ++k) *out++ = p;
      }
      for (int k = 1; k < s; ++k) {
        std::memcpy(out, line, static_cast<size_t>(w) * 2);
        out += w;
      }
    }
    esp_lcd_panel_draw_bitmap(panel_, 0, y * s, w, (y + rows) * s, bounce_);
    xSemaphoreTake(done_, pdMS_TO_TICKS(100));
  }
}

void SpiDisplay::set_backlight(uint8_t percent) {
  if (cfg_.backlight < 0) return;
  uint32_t duty = (1023u * std::min<uint8_t>(percent, 100)) / 100u;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, kBlChannel, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, kBlChannel);
}

}  // namespace hgp
