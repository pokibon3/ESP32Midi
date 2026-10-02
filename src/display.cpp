#include "display.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include <math.h>

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

// ---- layout (320 x 240)
constexpr int HEADER_H = 28;
constexpr int STATUS_Y = 31;
constexpr int ROW_Y = 57;
constexpr int ROW_H = 11;
constexpr int METER_X = 172;
constexpr int SEGMENTS = 24;
constexpr int SEG_W = 6;  // 5px lit + 1px gap

constexpr uint16_t COL_BG = TFT_BLACK;
constexpr uint16_t COL_LABEL = 0x8C71;  // gray
constexpr uint16_t COL_TRACK = 0x2104;  // empty bar

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return lgfx::color565(r, g, b); }

// h: 0..360, s/v: 0..1
uint16_t hsv(float h, float s, float v) {
  float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c;
  float r = 0, g = 0, b = 0;
  if (h < 60) { r = c; g = x; }
  else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; }
  else { r = c; b = x; }
  return rgb((r + m) * 255, (g + m) * 255, (b + m) * 255);
}

// Green -> yellow -> red along 0..1
uint16_t meterColor(float t) { return hsv(120.0f * (1.0f - fminf(t, 1.0f)), 1.0f, 1.0f); }

uint16_t channelColor(int c) { return hsv(c * 360.0f / 16, 0.75f, 1.0f); }
uint16_t channelDim(int c) { return hsv(c * 360.0f / 16, 0.8f, 0.22f); }

// Per-channel meter state
uint8_t level[16], peak[16], peakHold[16];
uint8_t drawnLit[16], drawnPeak[16];
const char* drawnName[16];
int16_t drawnProgram[16];
bool drawnDrum[16];

void drawGradientBar(int x, int y, int w, int h, float value) {
  int filled = (int)(w * fminf(fmaxf(value, 0.0f), 1.0f) + 0.5f);
  for (int i = 0; i < w; i++) {
    lcd.drawFastVLine(x + i, y, h, i < filled ? meterColor((float)i / w) : COL_TRACK);
  }
}

// Volume slider: track + blue fill + round knob, 0..100
constexpr int SLIDER_X = 30, SLIDER_W = 120;
int drawnVolume = -1;

void drawVolumeSlider(int vol) {
  const int y = STATUS_Y + 12, cy = y + 4;
  lcd.fillRect(SLIDER_X - 5, y - 1, SLIDER_W + 11, 11, COL_BG);  // clear old knob
  int kx = SLIDER_X + SLIDER_W * vol / 100;
  for (int x = SLIDER_X; x < kx; x++) {
    float t = (float)(x - SLIDER_X) / SLIDER_W;
    lcd.drawFastVLine(x, cy - 2, 5, rgb(0, 90 + 110 * t, 200 + 55 * t));
  }
  lcd.fillRoundRect(kx, cy - 2, SLIDER_X + SLIDER_W - kx, 5, 2, COL_TRACK);
  lcd.fillCircle(kx, cy, 4, TFT_WHITE);
  lcd.drawCircle(kx, cy, 4, rgb(0, 140, 255));
  lcd.setTextColor(TFT_CYAN, COL_BG);
  lcd.setCursor(SLIDER_X + SLIDER_W + 10, STATUS_Y + 13);
  lcd.printf("%3d", vol);
}

void drawSegment(int c, int k, uint16_t color) {
  lcd.fillRect(METER_X + k * SEG_W, ROW_Y + c * ROW_H + 2, SEG_W - 1, ROW_H - 4, color);
}

uint16_t segmentColor(int k) {
  return k < 14 ? rgb(0, 230, 118) : k < 20 ? rgb(255, 214, 0) : rgb(255, 61, 0);
}

void drawChannelLabel(int c, const synth::Stats& st) {
  int y = ROW_Y + c * ROW_H;
  uint16_t col = channelColor(c);
  lcd.fillRoundRect(2, y + 1, 18, ROW_H - 2, 3, col);
  lcd.setTextColor(TFT_BLACK, col);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.drawNumber(c + 1, 11, y + ROW_H / 2);
  lcd.setTextDatum(textdatum_t::top_left);

  lcd.setTextColor(st.drum[c] ? TFT_ORANGE : col, COL_BG);
  lcd.setCursor(24, y + 2);
  if (st.drum[c]) {
    lcd.print("DR ");
  } else {
    lcd.printf("%03d", st.program[c] + 1);
  }
  lcd.setTextColor(TFT_WHITE, COL_BG);
  lcd.setCursor(46, y + 2);
  lcd.printf("%-20.20s", st.presetName[c] ? st.presetName[c] : "");
}

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
  lcd.fillRect(0, 0, 320, HEADER_H, rgb(160, 0, 32));
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(8, 6);
  lcd.print(title);
  lcd.setFont(&fonts::Font0);
  lcd.setTextColor(TFT_WHITE, COL_BG);
  lcd.setCursor(8, 40);
  lcd.print(msg);
}

void showHeader(const char* sfName, uint32_t presets, uint32_t regions) {
  lcd.fillScreen(COL_BG);

  // Header: navy -> blue gradient
  for (int x = 0; x < 320; x++) {
    float t = x / 319.0f;
    lcd.drawFastVLine(x, 0, HEADER_H, rgb(10 + 20 * t, 30 + 90 * t, 100 + 130 * t));
  }
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(6, 6);
  lcd.print("GM SYNTH");
  lcd.setFont(&fonts::Font0);
  lcd.setTextDatum(textdatum_t::top_right);
  lcd.setTextColor(rgb(150, 230, 255));
  char buf[48];
  snprintf(buf, sizeof(buf), "%.30s", sfName);
  lcd.drawString(buf, 314, 5);
  lcd.setTextColor(rgb(200, 230, 255));
  snprintf(buf, sizeof(buf), "%lu presets  %lu regions", (unsigned long)presets, (unsigned long)regions);
  lcd.drawString(buf, 314, 16);
  lcd.setTextDatum(textdatum_t::top_left);

  // Status labels
  lcd.setTextColor(COL_LABEL, COL_BG);
  lcd.setCursor(4, STATUS_Y + 1);
  lcd.print("VOICE");
  lcd.setCursor(186, STATUS_Y + 1);
  lcd.print("CPU");
  lcd.setCursor(4, STATUS_Y + 13);
  lcd.print("VOL");
  lcd.setCursor(186, STATUS_Y + 13);
  lcd.print("PEAK");
  drawnVolume = -1;

  for (int x = 0; x < 320; x++) {
    lcd.drawPixel(x, ROW_Y - 3, hsv(x * 360.0f / 320, 0.7f, 0.6f));
  }

  for (int c = 0; c < 16; c++) {
    drawnLit[c] = 0;
    drawnPeak[c] = 0xFF;
    drawnName[c] = nullptr;
    drawnProgram[c] = -1;
    for (int k = 0; k < SEGMENTS; k++) drawSegment(c, k, channelDim(c));
  }
}

void update(float masterGain) {
  synth::Stats& st = synth::stats();
  lcd.setFont(&fonts::Font0);

  // ---- status
  drawGradientBar(38, STATUS_Y, 112, 9, (float)st.activeVoices / SYNTH_MAX_VOICES);
  lcd.setTextColor(TFT_WHITE, COL_BG);
  lcd.setCursor(154, STATUS_Y + 1);
  lcd.printf("%3u", st.activeVoices);
  drawGradientBar(208, STATUS_Y, 80, 9, st.cpuLoad);
  lcd.setCursor(292, STATUS_Y + 1);
  lcd.printf("%3d%%", (int)(st.cpuLoad * 100));

  int vol = (int)(masterGain * 100 + 0.5f);
  if (vol != drawnVolume) {
    drawVolumeSlider(vol);
    drawnVolume = vol;
  }
  lcd.setTextColor(TFT_YELLOW, COL_BG);
  lcd.setCursor(216, STATUS_Y + 13);
  lcd.printf("%3u", st.peakVoices);

  // ---- channels
  for (int c = 0; c < 16; c++) {
    if (st.presetName[c] != drawnName[c] || st.program[c] != drawnProgram[c] || st.drum[c] != drawnDrum[c]) {
      drawChannelLabel(c, st);
      drawnName[c] = st.presetName[c];
      drawnProgram[c] = st.program[c];
      drawnDrum[c] = st.drum[c];
    }

    if (st.activity[c] > level[c]) level[c] = st.activity[c];
    st.activity[c] = 0;
    if (level[c] >= peak[c]) {
      peak[c] = level[c];
      peakHold[c] = 15;  // ~0.75 s
    } else if (peakHold[c]) {
      peakHold[c]--;
    } else {
      peak[c] = peak[c] > 4 ? peak[c] - 4 : 0;
    }

    uint8_t lit = (level[c] * SEGMENTS + 126) / 127;
    uint8_t pk = peak[c] ? (peak[c] * SEGMENTS + 126) / 127 - 1 : 0xFF;

    // Redraw only the segments whose state changed
    uint8_t lo = min(lit, drawnLit[c]), hi = max(lit, drawnLit[c]);
    for (int k = lo; k < hi; k++) drawSegment(c, k, k < lit ? segmentColor(k) : channelDim(c));
    if (pk != drawnPeak[c]) {
      if (drawnPeak[c] < SEGMENTS && drawnPeak[c] >= lit) drawSegment(c, drawnPeak[c], channelDim(c));
      if (pk < SEGMENTS && pk >= lit) drawSegment(c, pk, TFT_WHITE);
    } else if (pk < SEGMENTS && pk >= lit && pk < drawnLit[c]) {
      drawSegment(c, pk, TFT_WHITE);  // level fell below the peak marker
    }
    drawnLit[c] = lit;
    drawnPeak[c] = pk;

    level[c] = level[c] > 6 ? level[c] - 6 : 0;
  }
}

}  // namespace display
