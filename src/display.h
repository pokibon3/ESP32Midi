// Status screen (ST7789 320x240)
#pragma once
#include <stdint.h>

namespace display {

void begin();
void showError(const char* title, const char* msg);
void showHeader(const char* sfName, uint32_t presets, uint32_t regions);
void update(float masterGain);

}  // namespace display
