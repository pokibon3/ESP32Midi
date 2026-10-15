// SoundFont based GM synthesizer
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "sf2.h"

#ifndef SYNTH_SAMPLE_RATE
#define SYNTH_SAMPLE_RATE 32000
#endif
#ifndef SYNTH_MAX_VOICES
#define SYNTH_MAX_VOICES 128
#endif

namespace synth {

struct Stats {
  uint16_t activeVoices;
  uint16_t peakVoices;
  float cpuLoad;       // render time / real time (0..1)
  float peakLoad;      // worst single block since the reader last reset it
  uint32_t lateBlocks;  // blocks that took longer than real time to render
  uint32_t underruns;   // DMA underruns (audible dropouts)
  uint32_t blocks;      // rendered blocks (audio loop alive)
  uint32_t clipped;     // output samples clipped at full scale
  uint8_t program[16];
  bool drum[16];
  uint8_t activity[16];  // last note-on velocity, decayed by the UI
  const char* presetName[16];
};

// Starts audio output and rendering on both cores. `sf` must outlive the synth.
bool begin(const sf2::SoundFont* sf, uint32_t sampleRate = SYNTH_SAMPLE_RATE);

// The event functions below queue to the audio core. Call them from one
// thread only (the main loop).

// Channel voice / mode messages (status byte 0x80..0xEF).
void midiMessage(uint8_t status, uint8_t d1, uint8_t d2);
// Complete SysEx message including F0 ... F7.
void sysex(const uint8_t* data, size_t len);
// GM reset.
void reset();

void setMasterGain(float gain);
// Return levels of the reverb / chorus send effects (1.0 = default)
void setEffectLevels(float reverb, float chorus);
Stats& stats();

}  // namespace synth
