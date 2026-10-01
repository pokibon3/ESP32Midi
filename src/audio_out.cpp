#include "audio_out.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>

#include "board.h"

namespace audio_out {

static i2s_chan_handle_t s_tx;

static bool es8156Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8156_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// Same sequence as Espressif esp_codec_dev (ES8156, slave mode, I2S 16bit)
static bool es8156Init() {
  static const uint8_t seq[][2] = {
      {0x02, 0x04}, {0x20, 0x2A}, {0x21, 0x3C}, {0x22, 0x00}, {0x24, 0x07},
      {0x23, 0x00}, {0x0A, 0x01}, {0x0B, 0x01}, {0x11, 0x00}, {0x14, 0xBF},
      {0x0D, 0x14}, {0x18, 0x00}, {0x08, 0x3F}, {0x00, 0x02}, {0x00, 0x03},
      {0x25, 0x20},
  };
  for (auto& s : seq) {
    if (!es8156Write(s[0], s[1])) return false;
  }
  return true;
}

void setVolume(uint8_t v) { es8156Write(0x14, v); }

bool begin(uint32_t sampleRate) {
  pinMode(PA_EN_PIN, OUTPUT);
  digitalWrite(PA_EN_PIN, LOW);

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = 4;
  chanCfg.dma_frame_num = 256;
  chanCfg.auto_clear = true;
  if (i2s_new_channel(&chanCfg, &s_tx, nullptr) != ESP_OK) return false;

  i2s_std_config_t stdCfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = (gpio_num_t)I2S_MCLK_PIN,
              .bclk = (gpio_num_t)I2S_BCLK_PIN,
              .ws = (gpio_num_t)I2S_WS_PIN,
              .dout = (gpio_num_t)I2S_DOUT_PIN,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  stdCfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_channel_init_std_mode(s_tx, &stdCfg) != ESP_OK) return false;
  if (i2s_channel_enable(s_tx) != ESP_OK) return false;

  // MCLK must be running before the codec is configured
  Wire.begin(CODEC_I2C_SDA, CODEC_I2C_SCL, 400000);
  if (!es8156Init()) return false;

  digitalWrite(PA_EN_PIN, HIGH);
  return true;
}

void write(const int16_t* frames, size_t frameCount) {
  size_t written = 0;
  i2s_channel_write(s_tx, frames, frameCount * 2 * sizeof(int16_t), &written, portMAX_DELAY);
}

}  // namespace audio_out
