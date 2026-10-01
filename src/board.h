// ESP32-S3-BOX-Lite pin assignments
#pragma once

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
#define I2S_DIN_PIN  16

// Speaker amplifier enable
#define PA_EN_PIN 46

// Buttons: BOOT = GPIO0, front 3 buttons = resistor ladder on GPIO1 (ADC)
#define BTN_BOOT_PIN 0
#define BTN_ADC_PIN  1
