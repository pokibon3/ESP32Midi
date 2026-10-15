// I2S output via PIO (arduino-pico I2S library) to a PCM5100A/PCM5102A.
// 32-bit slots (BCK = 64 fs) so the DAC's PLL locks without MCLK.
#include "audio_out.h"

#include <Arduino.h>
#include <I2S.h>

#include "board.h"
#include "platform.h"

#ifndef AUDIO_DMA_DESC
#define AUDIO_DMA_DESC 3
#endif
#ifndef AUDIO_DMA_FRAMES
#define AUDIO_DMA_FRAMES 128
#endif

static_assert(I2S_WS_PIN == I2S_BCLK_PIN + 1, "arduino-pico I2S needs WS = BCLK + 1");

namespace audio_out {

static I2S s_i2s(OUTPUT);
static uint32_t s_underruns;

uint32_t underruns() { return s_underruns; }
uint32_t bufferFrames() { return AUDIO_DMA_DESC * AUDIO_DMA_FRAMES; }

void setVolume(uint8_t) {}  // PCM510x has no volume control

bool begin(uint32_t sampleRate) {
#ifdef DAC_MUTE_PIN
  pinMode(DAC_MUTE_PIN, OUTPUT);
  digitalWrite(DAC_MUTE_PIN, LOW);
#endif
  s_i2s.setBCLK(I2S_BCLK_PIN);
  s_i2s.setDATA(I2S_DOUT_PIN);
  s_i2s.setBitsPerSample(32);
  s_i2s.setBuffers(AUDIO_DMA_DESC, AUDIO_DMA_FRAMES * 2);
  if (!s_i2s.begin(sampleRate)) return false;
#ifdef DAC_MUTE_PIN
  delay(20);  // let the DAC PLL lock on BCK before unmuting
  digitalWrite(DAC_MUTE_PIN, HIGH);
#endif
  return true;
}

void AUDIO_HOT write(const int16_t* frames, size_t frameCount) {
  constexpr size_t CHUNK = 64;
  int32_t buf[CHUNK * 2];
  while (frameCount) {
    size_t n = frameCount < CHUNK ? frameCount : CHUNK;
    for (size_t i = 0; i < n * 2; i++) buf[i] = (int32_t)frames[i] << 16;
    const uint8_t* p = (const uint8_t*)buf;
    size_t left = n * 2 * sizeof(int32_t);
    while (left) {  // non-blocking write: spin until DMA frees a buffer
      size_t w = s_i2s.write(p, left);
      p += w;
      left -= w;
    }
    frames += n * 2;
    frameCount -= n;
  }
  if (s_i2s.getUnderflow()) s_underruns++;
}

}  // namespace audio_out
