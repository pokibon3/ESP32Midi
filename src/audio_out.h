// I2S + ES8156 DAC output
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace audio_out {

bool begin(uint32_t sampleRate);
// Blocking write of interleaved stereo int16 frames.
void write(const int16_t* frames, size_t frameCount);
// 0..255 (ES8156 digital volume, 0xBF = 0dB)
void setVolume(uint8_t v);
// Number of DMA buffers that ran out of data (audible dropouts)
uint32_t underruns();
// Output buffering in frames (DMA)
uint32_t bufferFrames();

}  // namespace audio_out
