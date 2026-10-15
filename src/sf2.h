// SoundFont 2 loader.
// The SF2 file is written raw into flash and memory-mapped, so the 16-bit
// sample data is played straight out of flash. Only the preset / instrument
// hierarchy is flattened into "regions" in RAM (PSRAM on ESP32).
#pragma once
#include <stdint.h>

namespace sf2 {

// Generators the synth reads at note-on / control rate, stored per region in
// this order. Sample addresses, ranges, root key and tuning are folded into the
// Region fields instead.
enum Param : uint8_t {
  keynum,
  velocity,
  exclusiveClass,
  scaleTuning,
  initialAttenuation,
  pan,
  initialFilterFc,
  initialFilterQ,
  modLfoToPitch,
  vibLfoToPitch,
  modEnvToPitch,
  modLfoToFilterFc,
  modEnvToFilterFc,
  modLfoToVolume,
  chorusEffectsSend,
  reverbEffectsSend,
  delayModLFO,
  freqModLFO,
  delayVibLFO,
  freqVibLFO,
  delayModEnv,
  attackModEnv,
  holdModEnv,
  decayModEnv,
  sustainModEnv,
  releaseModEnv,
  keynumToModEnvHold,
  keynumToModEnvDecay,
  delayVolEnv,
  attackVolEnv,
  holdVolEnv,
  decayVolEnv,
  sustainVolEnv,
  releaseVolEnv,
  keynumToVolEnvHold,
  keynumToVolEnvDecay,
  PARAM_COUNT,
};

struct Region {
  uint8_t loKey, hiKey, loVel, hiVel;
  uint32_t start, end, loopStart, loopEnd;  // absolute sample indices
  uint32_t sampleRate;
  uint8_t loopMode;  // 0: none, 1: continuous, 3: loop until release
  uint8_t rootKey;
  int16_t tuneCents;  // coarse + fine + sample pitch correction
  // PARAM_COUNT merged generator values (instrument + preset). Regions with
  // identical values share one array.
  const int16_t* gen;
};

struct Preset {
  char name[21];
  uint16_t bank;
  uint16_t program;
  uint32_t firstRegion;
  uint32_t regionCount;
};

class SoundFont {
 public:
  // Maps the SoundFont in flash (platform::mapSoundFont) and parses it.
  bool load();

  // Exact match only. Returns nullptr when not present.
  const Preset* findPreset(uint16_t bank, uint16_t program) const;

  const char* name() const { return name_; }
  const char* error() const { return error_; }

  const int16_t* samples = nullptr;
  uint32_t sampleCount = 0;
  const Region* regions = nullptr;
  uint32_t regionCount = 0;
  const Preset* presets = nullptr;
  uint32_t presetCount = 0;
  uint32_t paramSetCount = 0;  // distinct generator arrays shared by the regions

 private:
  bool parse(const uint8_t* data, uint32_t size);
  bool fail(const char* msg) {
    error_ = msg;
    return false;
  }

  char name_[64] = "";
  const char* error_ = "";
};

}  // namespace sf2
