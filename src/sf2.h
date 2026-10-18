// SoundFont 2 loader.
// The SF2 file is written raw into flash and memory-mapped, so the 16-bit
// sample data is played straight out of flash. The preset / instrument
// hierarchy is flattened into "regions" in RAM (PSRAM on ESP32) per preset,
// when a channel selects it; presets nothing uses are freed again when the
// cache is over its limit or memory runs out.
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
  uint16_t bag0, bag1;  // pbag range in the file
  uint32_t regionCount;
  // Built on demand (SoundFont::regionsOf)
  Region* regions;
  uint32_t bytes;
  uint32_t lastUse;
};

class SoundFont {
 public:
  // Maps the SoundFont in flash (platform::mapSoundFont) and parses it.
  bool load();

  // Exact match only. Returns nullptr when not present.
  const Preset* findPreset(uint16_t bank, uint16_t program) const;

  // The preset's regionCount regions, built on first use. nullptr when out of memory.
  const Region* regionsOf(const Preset* preset);

  // Presets for which inUse returns true are never freed.
  void setInUse(bool (*inUse)(const Preset*)) { inUse_ = inUse; }
  // Free built presets beyond this many bytes (when not in use)
  void setCacheLimit(uint32_t bytes) { cacheLimit_ = bytes; }
  uint32_t cacheBytes() const { return cacheBytes_; }

  const char* name() const { return name_; }
  const char* error() const { return error_; }

  const int16_t* samples = nullptr;
  uint32_t sampleCount = 0;
  Preset* presets = nullptr;
  uint32_t presetCount = 0;
  uint32_t regionCount = 0;  // all presets

 private:
  bool parse(const uint8_t* data, uint32_t size);
  template <typename Emit>
  void walkRegions(const Preset& pr, Emit&& emit) const;
  bool build(Preset& p);
  bool evictOne(const Preset* keep);
  bool fail(const char* msg) {
    error_ = msg;
    return false;
  }

  char name_[64] = "";
  const char* error_ = "";

  const uint8_t *pbag_ = nullptr, *pgen_ = nullptr, *inst_ = nullptr, *ibag_ = nullptr, *igen_ = nullptr,
                *shdr_ = nullptr;
  uint32_t nPbag_ = 0, nPgen_ = 0, nInst_ = 0, nIbag_ = 0, nIgen_ = 0, nShdr_ = 0;
  bool (*inUse_)(const Preset*) = nullptr;
  uint32_t cacheLimit_ = UINT32_MAX;
  uint32_t cacheBytes_ = 0;
  uint32_t useCounter_ = 0;
};

}  // namespace sf2
