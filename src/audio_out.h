// I2S DAC output (ES8156 on BOX-Lite, PCM5102A on XIAO, PCM5100A on RP2350)
// Implementations: src/platform/<platform>/audio_out.cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace audio_out {

bool begin(uint32_t sampleRate);
// Blocking write of interleaved stereo frames, 32-bit left-justified
// (the DAC uses the top 24 bits).
void write(const int32_t* frames, size_t frameCount);
// DAC volume as a linear gain (1.0 = 0dB, 0 = mute). Returns false when the
// DAC has no volume control (PCM510x); the caller then scales the samples.
bool setGain(float gain);
// Number of DMA buffers that ran out of data (audible dropouts)
uint32_t underruns();
// Output buffering in frames (DMA)
uint32_t bufferFrames();

}  // namespace audio_out
