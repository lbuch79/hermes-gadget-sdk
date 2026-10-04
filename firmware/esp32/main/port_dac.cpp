// Speaker on the ESP32's built-in 8-bit DAC (continuous/DMA mode) feeding an
// analog amplifier, as on the ESP32-2432S028R "Cheap Yellow Display" (DAC2 on
// GPIO26 -> SC8002B amp -> the SPEAK connector).
//
// write() converts PCM16 to unsigned 8-bit as it queues, so 1.5 s at 16 kHz
// fits in 24 KB of internal RAM on a board without PSRAM. The DAC's DMA clock
// can't run much below 20 kHz on the ESP32, so the writer task upsamples by
// kUpsample with linear interpolation.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include "soc/soc_caps.h"

#if SOC_DAC_SUPPORTED

#include <algorithm>
#include <cstring>

#include "driver/dac_continuous.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {
namespace {

const char* TAG = "hg.dac";
constexpr size_t kBufferBytes = 24 * 1024;  // ~1.5 s at 16 kHz, 8-bit
constexpr int kUpsample = 2;                 // 16 kHz stream -> 32 kHz DAC
constexpr size_t kChunk = 256;               // stream bytes per DAC write

dac_continuous_handle_t handle(void* p) { return static_cast<dac_continuous_handle_t>(p); }

}  // namespace

bool DacSpeaker::begin(const DacSpeakerConfig& cfg) {
  cfg_ = cfg;
  if (cfg.gpio != 25 && cfg.gpio != 26) {
    ESP_LOGE(TAG, "the DAC is on GPIO25 or GPIO26, not %d", cfg.gpio);
    return false;
  }
  buffer_ = xStreamBufferCreate(kBufferBytes, 1);
  if (!buffer_) return false;
  xTaskCreate(&DacSpeaker::task, "hg-dac", 3072, this, 7, nullptr);
  ESP_LOGI(TAG, "DAC speaker on GPIO%d ready", cfg.gpio);
  return true;
}

bool DacSpeaker::begin(uint32_t sample_rate) {
  if (!buffer_) return false;
  abort();
  rate_ = sample_rate;  // applied by the writer task when it next opens the DAC
  open_ = true;
  draining_ = false;
  return true;
}

void DacSpeaker::write(const int16_t* samples, size_t count) {
  if (!open_) return;
  const int vol = volume_.load();
  uint8_t chunk[128];
  size_t dropped = 0;
  while (count) {
    size_t n = std::min(count, sizeof(chunk));
    for (size_t i = 0; i < n; ++i) {
      int s = samples[i] * vol / 100;
      chunk[i] = static_cast<uint8_t>(std::clamp((s >> 8) + 128, 0, 255));
    }
    dropped += n - xStreamBufferSend(buffer_, chunk, n, 0);
    samples += n;
    count -= n;
  }
  if (dropped) ESP_LOGW(TAG, "playback buffer full, dropped %u samples", static_cast<unsigned>(dropped));
}

void DacSpeaker::end() {
  open_ = false;
  draining_ = true;
}

void DacSpeaker::abort() {
  open_ = false;
  draining_ = false;
  flush_ = true;
}

bool DacSpeaker::busy() const {
  return open_ || draining_ || (buffer_ && xStreamBufferBytesAvailable(buffer_) > 0);
}

void DacSpeaker::task(void* arg) {
  // Owns the DAC channel: creates it on the first audio of a stream and frees
  // it when the stream has played out, so the amplifier is quiet in between.
  auto* self = static_cast<DacSpeaker*>(arg);
  uint8_t in[kChunk];
  uint8_t out[kChunk * kUpsample];
  uint8_t last = 128;
  for (;;) {
    if (self->flush_.exchange(false)) {
      xStreamBufferReset(self->buffer_);
      if (self->dac_) {
        dac_continuous_disable(handle(self->dac_));
        dac_continuous_del_channels(handle(self->dac_));
        self->dac_ = nullptr;
      }
    }
    size_t got = xStreamBufferReceive(self->buffer_, in, sizeof(in), pdMS_TO_TICKS(20));
    if (got == 0) {
      if (self->draining_ && !self->open_) {
        self->draining_ = false;
        if (self->dac_) {
          dac_continuous_disable(handle(self->dac_));
          dac_continuous_del_channels(handle(self->dac_));
          self->dac_ = nullptr;
        }
      }
      continue;
    }
    if (!self->dac_) {
      dac_continuous_config_t cfg = {};
      cfg.chan_mask = self->cfg_.gpio == 25 ? DAC_CHANNEL_MASK_CH0 : DAC_CHANNEL_MASK_CH1;
      cfg.desc_num = 4;
      cfg.buf_size = 1024;
      cfg.freq_hz = self->rate_.load() * kUpsample;
      cfg.offset = 0;
      cfg.clk_src = DAC_DIGI_CLK_SRC_APLL;  // APLL reaches low rates accurately
      cfg.chan_mode = DAC_CHANNEL_MODE_SIMUL;
      dac_continuous_handle_t h = nullptr;
      if (dac_continuous_new_channels(&cfg, &h) != ESP_OK || dac_continuous_enable(h) != ESP_OK) {
        ESP_LOGE(TAG, "DAC did not start");
        if (h) dac_continuous_del_channels(h);
        continue;
      }
      self->dac_ = h;
      last = 128;
    }
    size_t n = 0;
    for (size_t i = 0; i < got; ++i) {
      uint8_t s = in[i];
      for (int k = 1; k <= kUpsample; ++k) out[n++] = static_cast<uint8_t>(last + (s - last) * k / kUpsample);
      last = s;
    }
    size_t loaded = 0;
    dac_continuous_write(handle(self->dac_), out, n, &loaded, 200);
  }
}

}  // namespace hgp

#else  // no DAC on this chip: the board config never enables it

namespace hgp {
bool DacSpeaker::begin(const DacSpeakerConfig&) { return false; }
bool DacSpeaker::begin(uint32_t) { return false; }
void DacSpeaker::write(const int16_t*, size_t) {}
void DacSpeaker::end() {}
void DacSpeaker::abort() {}
bool DacSpeaker::busy() const { return false; }
void DacSpeaker::task(void*) {}
}  // namespace hgp

#endif
