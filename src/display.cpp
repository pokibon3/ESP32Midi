#include "display.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "board.h"
#include "synth.h"

namespace display {

namespace {

class LGFX_BoxLite : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;

 public:
  LGFX_BoxLite() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = LCD_SCLK;
      cfg.pin_mosi = LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = LCD_DC;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = LCD_CS;
      cfg.pin_rst = LCD_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_rotation = 2;
      cfg.invert = true;
      cfg.readable = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = LCD_BL;
      cfg.invert = true;
      cfg.freq = 12000;
      cfg.pwm_channel = 0;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    setPanel(&panel_);
  }
};

LGFX_BoxLite lcd;

constexpr int ROW_Y = 44;
constexpr int ROW_H = 12;
constexpr uint16_t COL_BG = TFT_BLACK;
constexpr uint16_t COL_DIM = 0x7BEF;
uint8_t level[16];

}  // namespace

void begin() {
  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(160);
  lcd.fillScreen(COL_BG);
  lcd.setFont(&fonts::Font0);
  lcd.setTextColor(TFT_WHITE, COL_BG);
}

void showError(const char* title, const char* msg) {
  lcd.fillScreen(COL_BG);
  lcd.setTextColor(TFT_RED, COL_BG);
  lcd.setTextSize(2);
  lcd.setCursor(8, 8);
  lcd.print(title);
  lcd.setTextColor(TFT_WHITE, COL_BG);
  lcd.setTextSize(1);
  lcd.setCursor(8, 40);
  lcd.print(msg);
}

void showHeader(const char* sfName, uint32_t presets, uint32_t regions) {
  lcd.fillScreen(COL_BG);
  lcd.setTextSize(1);
  lcd.setTextColor(TFT_CYAN, COL_BG);
  lcd.setCursor(4, 2);
  lcd.printf("%.52s", sfName);
  lcd.setTextColor(COL_DIM, COL_BG);
  lcd.setCursor(4, 14);
  lcd.printf("%lu presets / %lu regions", (unsigned long)presets, (unsigned long)regions);
  lcd.drawFastHLine(0, ROW_Y - 4, 320, COL_DIM);
}

void update(float masterGain) {
  synth::Stats& st = synth::stats();
  lcd.setTextSize(1);
  lcd.setTextColor(TFT_WHITE, COL_BG);
  lcd.setCursor(4, 26);
  lcd.printf("VOICE %3u/%d PEAK %3u  CPU %3d%%  XRUN %lu  VOL %3d%%  ", st.activeVoices,
             SYNTH_MAX_VOICES, st.peakVoices, (int)(st.cpuLoad * 100), (unsigned long)st.underruns,
             (int)(masterGain * 100 / 0.35f + 0.5f));

  for (int c = 0; c < 16; c++) {
    int y = ROW_Y + c * ROW_H;
    if (st.activity[c] > level[c]) level[c] = st.activity[c];
    st.activity[c] = 0;

    lcd.setCursor(4, y + 2);
    lcd.setTextColor(st.drum[c] ? TFT_ORANGE : TFT_GREEN, COL_BG);
    lcd.printf("%2d", c + 1);
    lcd.setTextColor(TFT_WHITE, COL_BG);
    lcd.printf(" %3d %-20.20s", st.program[c] + 1, st.presetName[c] ? st.presetName[c] : "");

    int w = level[c] * 100 / 127;
    lcd.fillRect(212, y + 2, w, ROW_H - 4, TFT_GREENYELLOW);
    lcd.fillRect(212 + w, y + 2, 100 - w, ROW_H - 4, 0x18E3);
    level[c] = level[c] > 6 ? level[c] - 6 : 0;
  }
}

}  // namespace display
