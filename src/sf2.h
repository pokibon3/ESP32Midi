// SoundFont 2 loader.
// The SF2 file is written raw into a flash partition and memory-mapped, so the
// 16-bit sample data is played straight out of flash. Only the preset /
// instrument hierarchy is flattened into "regions" in PSRAM.
#pragma once
#include <stdint.h>

namespace sf2 {

// SF2 generator operators (SoundFont 2.04, 8.1.2)
enum Gen : uint8_t {
  startAddrsOffset = 0,
  endAddrsOffset = 1,
  startloopAddrsOffset = 2,
  endloopAddrsOffset = 3,
  startAddrsCoarseOffset = 4,
  modLfoToPitch = 5,
  vibLfoToPitch = 6,
  modEnvToPitch = 7,
  initialFilterFc = 8,
  initialFilterQ = 9,
  modLfoToFilterFc = 10,
  modEnvToFilterFc = 11,
  endAddrsCoarseOffset = 12,
  modLfoToVolume = 13,
  chorusEffectsSend = 15,
  reverbEffectsSend = 16,
  pan = 17,
  delayModLFO = 21,
  freqModLFO = 22,
  delayVibLFO = 23,
  freqVibLFO = 24,
  delayModEnv = 25,
  attackModEnv = 26,
  holdModEnv = 27,
  decayModEnv = 28,
  sustainModEnv = 29,
  releaseModEnv = 30,
  keynumToModEnvHold = 31,
  keynumToModEnvDecay = 32,
  delayVolEnv = 33,
  attackVolEnv = 34,
  holdVolEnv = 35,
  decayVolEnv = 36,
  sustainVolEnv = 37,
  releaseVolEnv = 38,
  keynumToVolEnvHold = 39,
  keynumToVolEnvDecay = 40,
  instrument = 41,
  keyRange = 43,
  velRange = 44,
  startloopAddrsCoarseOffset = 45,
  keynum = 46,
  velocity = 47,
  initialAttenuation = 48,
  endloopAddrsCoarseOffset = 50,
  coarseTune = 51,
  fineTune = 52,
  sampleID = 53,
  sampleModes = 54,
  scaleTuning = 56,
  exclusiveClass = 57,
  overridingRootKey = 58,
  GEN_COUNT = 61,
};

struct Region {
  uint8_t loKey, hiKey, loVel, hiVel;
  uint32_t start, end, loopStart, loopEnd;  // absolute sample indices
  uint32_t sampleRate;
  uint8_t loopMode;  // 0: none, 1: continuous, 3: loop until release
  uint8_t rootKey;
  int16_t tuneCents;  // coarse + fine + sample pitch correction
  int16_t gen[GEN_COUNT];  // merged generator values (instrument + preset)
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
  bool loadFromPartition(const char* label);

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
