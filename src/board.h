// Board definitions. Select one with -DBOARD_xxx (platformio.ini).
#pragma once

#if defined(BOARD_BOX_LITE)
// ---------------------------------------------------------------------------
// ESP32-S3-BOX-Lite: ES8156 DAC + speaker amp, ST7789 LCD, ADC buttons

#define BOARD_NAME   "ESP32-S3-BOX-Lite"
#define HAS_DISPLAY  1
#define HAS_ES8156   1
#define HAS_ADC_BUTTONS 1

// LCD (ST7789V 320x240, SPI)
#define LCD_MOSI 6
#define LCD_SCLK 7
#define LCD_DC   4
#define LCD_CS   5
#define LCD_RST  48
#define LCD_BL   45  // active low

// Audio codec (ES8156 DAC) control
#define CODEC_I2C_SDA  8
#define CODEC_I2C_SCL  18
#define ES8156_ADDR    0x08

// I2S
#define I2S_MCLK_PIN 2
#define I2S_BCLK_PIN 17
#define I2S_WS_PIN   47
#define I2S_DOUT_PIN 15

// Speaker amplifier enable
#define PA_EN_PIN 46

// Buttons: BOOT = GPIO0, front 3 buttons = resistor ladder on GPIO1 (ADC)
#define BTN_BOOT_PIN 0
#define BTN_ADC_PIN  1

#elif defined(BOARD_XIAO_PCM5102)
// ---------------------------------------------------------------------------
// Seeed XIAO ESP32S3 + PCM5102A I2S DAC (stereo line out)
//
//   PCM5102A   XIAO
//   BCK        D8  (GPIO7)
//   LCK        D9  (GPIO8)
//   DIN        D10 (GPIO9)
//   SCK        GND   (internal PLL from BCK)
//   XSMT       3.3V  (unmute)
//   FMT/FLT/DEMP GND (I2S, normal filter, no de-emphasis)
//   VIN/GND    5V/GND

#define BOARD_NAME   "XIAO ESP32S3 + PCM5102"
#define HAS_DISPLAY  0
#define HAS_ES8156   0
#define HAS_ADC_BUTTONS 0

#define I2S_MCLK_PIN -1  // not used by PCM5102A
#define I2S_BCLK_PIN 7
#define I2S_WS_PIN   8
#define I2S_DOUT_PIN 9

#define BTN_BOOT_PIN 0
#define LED_PIN      21  // user LED
#define LED_ACTIVE_LOW 1

#elif defined(BOARD_WEACT_RP2350)
// ---------------------------------------------------------------------------
// WeAct RP2350A_V20 (16MB flash) + WeAct I2S Earphone Module V1 (PCM5100A + HT97226)
//
//   Module P1   RP2350A_V20
//   1 VCC       VSYS (pin 39)  module has its own 3.3V LDO; do not use 3V3
//   2 GND       GND
//   3 /SD       GP21 (pin 27)  low = mute (DAC XSMT + amp shutdown)
//   4 MCLK      n.c.           100k pull-down on the module: DAC PLL from BCK
//   5 BCK       GP18 (pin 24)
//   6 DIN       GP20 (pin 26)
//   7 WS        GP19 (pin 25)  must be BCK + 1 (PIO I2S)
//
//   UART MIDI in: GP1 (pin 2, UART0 RX) via a 6N138 opto-coupler

#undef BOARD_NAME  // arduino-pico defines its own
#define BOARD_NAME   "WeAct RP2350A + I2S Earphone"
#define HAS_DISPLAY  0
#define HAS_ES8156   0
#define HAS_ADC_BUTTONS 0

#define I2S_MCLK_PIN -1
#define I2S_BCLK_PIN 18
#define I2S_WS_PIN   19
#define I2S_DOUT_PIN 20
#define DAC_MUTE_PIN 21

#define LED_PIN      25  // blue LED, active high
#define LED_ACTIVE_LOW 0

// The HT97226 headphone amp distorts above about -12dB of the BOX-Lite level
#define DEFAULT_MASTER_GAIN 0.09f

#else
#error "Select a board: -DBOARD_BOX_LITE, -DBOARD_XIAO_PCM5102 or -DBOARD_WEACT_RP2350"
#endif
