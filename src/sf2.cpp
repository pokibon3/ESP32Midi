#include "sf2.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include <initializer_list>

#include "platform.h"

namespace sf2 {

namespace {

// SF2 generator operators (SoundFont 2.04, 8.1.2)
namespace gen {
enum : uint8_t {
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
  COUNT = 61,
};
}  // namespace gen

// Generator operator of each Param, in Param order
constexpr uint8_t kParamGen[PARAM_COUNT] = {
    gen::keynum,           gen::velocity,           gen::exclusiveClass,      gen::scaleTuning,
    gen::initialAttenuation, gen::pan,              gen::initialFilterFc,     gen::initialFilterQ,
    gen::modLfoToPitch,    gen::vibLfoToPitch,      gen::modEnvToPitch,       gen::modLfoToFilterFc,
    gen::modEnvToFilterFc, gen::modLfoToVolume,     gen::chorusEffectsSend,   gen::reverbEffectsSend,
    gen::delayModLFO,      gen::freqModLFO,         gen::delayVibLFO,         gen::freqVibLFO,
    gen::delayModEnv,      gen::attackModEnv,       gen::holdModEnv,          gen::decayModEnv,
    gen::sustainModEnv,    gen::releaseModEnv,      gen::keynumToModEnvHold,  gen::keynumToModEnvDecay,
    gen::delayVolEnv,      gen::attackVolEnv,       gen::holdVolEnv,          gen::decayVolEnv,
    gen::sustainVolEnv,    gen::releaseVolEnv,      gen::keynumToVolEnvHold,  gen::keynumToVolEnvDecay,
};

// pdta records are packed and unaligned; always read through memcpy.
inline uint16_t rd16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}
inline uint32_t rd32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

struct Chunk {
  const uint8_t* data = nullptr;
  uint32_t size = 0;
};

// Find sub-chunk `id` inside a LIST body [p, p+size).
Chunk findChunk(const uint8_t* p, uint32_t size, const char* id) {
  uint32_t off = 0;
  while (off + 8 <= size) {
    uint32_t len = rd32(p + off + 4);
    if (memcmp(p + off, id, 4) == 0) return {p + off + 8, len};
    off += 8 + len + (len & 1);
  }
  return {};
}

// Find LIST chunk with form type `type` at RIFF top level.
Chunk findList(const uint8_t* p, uint32_t size, const char* type) {
  uint32_t off = 0;
  while (off + 12 <= size) {
    uint32_t len = rd32(p + off + 4);
    if (memcmp(p + off, "LIST", 4) == 0 && memcmp(p + off + 8, type, 4) == 0) {
      return {p + off + 12, len - 4};
    }
    off += 8 + len + (len & 1);
  }
  return {};
}

constexpr uint32_t PHDR_SIZE = 38, BAG_SIZE = 4, GEN_SIZE = 4, INST_SIZE = 22, SHDR_SIZE = 46;

// Generators that are not summed between preset and instrument level.
bool isNonAdditive(int op) {
  switch (op) {
    case gen::startAddrsOffset: case gen::endAddrsOffset: case gen::startloopAddrsOffset:
    case gen::endloopAddrsOffset: case gen::startAddrsCoarseOffset: case gen::endAddrsCoarseOffset:
    case gen::startloopAddrsCoarseOffset: case gen::endloopAddrsCoarseOffset:
    case gen::instrument: case gen::keyRange: case gen::velRange: case gen::keynum: case gen::velocity:
    case gen::sampleID: case gen::sampleModes: case gen::exclusiveClass: case gen::overridingRootKey:
      return true;
    default:
      return false;
  }
}

void instrumentDefaults(int16_t* g) {
  memset(g, 0, sizeof(int16_t) * gen::COUNT);
  g[gen::initialFilterFc] = 13500;
  for (int op : {gen::delayModLFO, gen::delayVibLFO, gen::delayModEnv, gen::attackModEnv, gen::holdModEnv,
                 gen::decayModEnv, gen::releaseModEnv, gen::delayVolEnv, gen::attackVolEnv, gen::holdVolEnv,
                 gen::decayVolEnv, gen::releaseVolEnv}) {
    g[op] = -12000;
  }
  g[gen::keyRange] = 0x7F00;
  g[gen::velRange] = 0x7F00;
  g[gen::keynum] = -1;
  g[gen::velocity] = -1;
  g[gen::scaleTuning] = 100;
  g[gen::overridingRootKey] = -1;
}

void presetDefaults(int16_t* g) {
  memset(g, 0, sizeof(int16_t) * gen::COUNT);
  g[gen::keyRange] = 0x7F00;
  g[gen::velRange] = 0x7F00;
}

void applyGens(int16_t* g, const uint8_t* gens, uint32_t first, uint32_t last) {
  for (uint32_t i = first; i < last; i++) {
    const uint8_t* rec = gens + i * GEN_SIZE;
    uint16_t op = rd16(rec);
    if (op < gen::COUNT) g[op] = (int16_t)rd16(rec + 2);
  }
}

// Range generators: low byte = lo, high byte = hi
inline uint8_t rangeLo(int16_t v) { return (uint16_t)v & 0xFF; }
inline uint8_t rangeHi(int16_t v) { return (uint16_t)v >> 8; }

// Deduplicating store of PARAM_COUNT-sized generator arrays. Most regions of
// an instrument share their articulation, so this roughly halves the table.
class ParamPool {
 public:
  ~ParamPool() { free(table_); }

  // Returns the index of `p` in the pool (adding it when new), -1 when out of memory.
  int32_t intern(const int16_t* p) {
    if ((count + 1) * 2 > tableSize_ && !rehash(tableSize_ ? tableSize_ * 2 : 1024)) return -1;
    uint32_t mask = tableSize_ - 1;
    for (uint32_t h = hash(p) & mask;; h = (h + 1) & mask) {
      uint32_t e = table_[h];
      if (!e) break;
      if (memcmp(data + (e - 1) * PARAM_COUNT, p, BYTES) == 0) return e - 1;
    }
    if (count == cap) {
      uint32_t newCap = cap ? cap * 2 : 256;
      int16_t* grown = (int16_t*)platform::reallocLarge(data, newCap * BYTES);
      if (!grown) return -1;
      data = grown;
      cap = newCap;
    }
    memcpy(data + count * PARAM_COUNT, p, BYTES);
    insert(count);
    return count++;
  }

  // Pre-allocates room for `n` sets so that intern() does not need to grow.
  bool reserve(uint32_t n) {
    if (n <= cap) return true;
    int16_t* d = (int16_t*)platform::reallocLarge(data, n * BYTES);
    if (!d) return false;
    data = d;
    cap = n;
    return true;
  }

  int16_t* data = nullptr;
  uint32_t count = 0, cap = 0;

 private:
  static constexpr size_t BYTES = PARAM_COUNT * sizeof(int16_t);

  static uint32_t hash(const int16_t* p) {  // FNV-1a
    const uint8_t* b = (const uint8_t*)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < BYTES; i++) h = (h ^ b[i]) * 16777619u;
    return h;
  }
  void insert(uint32_t index) {  // table entries are index + 1, 0 = empty
    uint32_t mask = tableSize_ - 1;
    uint32_t h = hash(data + index * PARAM_COUNT) & mask;
    while (table_[h]) h = (h + 1) & mask;
    table_[h] = index + 1;
  }
  bool rehash(uint32_t size) {
    uint32_t* t = (uint32_t*)platform::allocLarge(size * sizeof(uint32_t));
    if (!t) return false;
    free(table_);
    table_ = t;
    tableSize_ = size;
    for (uint32_t i = 0; i < count; i++) insert(i);
    return true;
  }

  uint32_t* table_ = nullptr;
  uint32_t tableSize_ = 0;
};

}  // namespace

bool SoundFont::load() {
  const uint8_t* data = nullptr;
  uint32_t size = 0;
  if (const char* err = platform::mapSoundFont(data, size)) return fail(err);
  return parse(data, size);
}

bool SoundFont::parse(const uint8_t* data, uint32_t size) {
  const uint8_t* body = data + 12;
  uint32_t bodySize = size - 12;

  Chunk info = findList(body, bodySize, "INFO");
  Chunk sdta = findList(body, bodySize, "sdta");
  Chunk pdta = findList(body, bodySize, "pdta");
  if (!sdta.data || !pdta.data) return fail("broken SF2 (missing sdta/pdta)");

  if (info.data) {
    Chunk ifil = findChunk(info.data, info.size, "ifil");
    if (ifil.data && rd16(ifil.data) >= 3) return fail("SF3 (compressed) is not supported");
    Chunk inam = findChunk(info.data, info.size, "INAM");
    if (inam.data) {
      uint32_t n = inam.size < sizeof(name_) - 1 ? inam.size : sizeof(name_) - 1;
      memcpy(name_, inam.data, n);
      name_[n] = 0;
    }
  }

  Chunk smpl = findChunk(sdta.data, sdta.size, "smpl");
  if (!smpl.data) return fail("no sample data");
  samples = (const int16_t*)smpl.data;
  sampleCount = smpl.size / 2;

  Chunk phdr = findChunk(pdta.data, pdta.size, "phdr");
  Chunk pbag = findChunk(pdta.data, pdta.size, "pbag");
  Chunk pgen = findChunk(pdta.data, pdta.size, "pgen");
  Chunk inst = findChunk(pdta.data, pdta.size, "inst");
  Chunk ibag = findChunk(pdta.data, pdta.size, "ibag");
  Chunk igen = findChunk(pdta.data, pdta.size, "igen");
  Chunk shdr = findChunk(pdta.data, pdta.size, "shdr");
  if (!phdr.data || !pbag.data || !pgen.data || !inst.data || !ibag.data || !igen.data || !shdr.data) {
    return fail("broken SF2 (missing pdta chunk)");
  }

  const uint32_t nPhdr = phdr.size / PHDR_SIZE;  // includes terminal EOP record
  const uint32_t nPbag = pbag.size / BAG_SIZE;
  const uint32_t nPgen = pgen.size / GEN_SIZE;
  const uint32_t nInst = inst.size / INST_SIZE;
  const uint32_t nIbag = ibag.size / BAG_SIZE;
  const uint32_t nIgen = igen.size / GEN_SIZE;
  const uint32_t nShdr = shdr.size / SHDR_SIZE;
  if (nPhdr < 2 || nInst < 2 || nShdr < 2) return fail("SF2 has no presets");

  Preset* outPresets = (Preset*)platform::allocLarge((nPhdr - 1) * sizeof(Preset));
  if (!outPresets) return fail("out of memory");
  Region* outRegions = nullptr;
  uint32_t nRegions = 0;
  ParamPool pool;

  int16_t pGlobal[gen::COUNT], pZone[gen::COUNT], iGlobal[gen::COUNT], iZone[gen::COUNT];

  // Pass 0 counts the regions, pass 1 fills exactly sized tables: growing them
  // would need old + new copies at once, which does not fit without PSRAM.
  for (int pass = 0; pass < 2; pass++) {
    const bool store = pass == 1;
    if (store) {
      outRegions = (Region*)platform::allocLarge((nRegions ? nRegions : 1) * sizeof(Region));
      if (!outRegions || !pool.reserve(nRegions)) return fail("out of memory");
      nRegions = 0;
    }
    for (uint32_t pi = 0; pi + 1 < nPhdr; pi++) {
      const uint8_t* ph = phdr.data + pi * PHDR_SIZE;
      Preset& pr = outPresets[pi];
      memcpy(pr.name, ph, 20);
      pr.name[20] = 0;
      pr.program = rd16(ph + 20);
      pr.bank = rd16(ph + 22);
      pr.firstRegion = nRegions;

      uint32_t bag0 = rd16(ph + 24), bag1 = rd16(ph + PHDR_SIZE + 24);
      if (bag1 > nPbag) bag1 = nPbag;
      presetDefaults(pGlobal);

      for (uint32_t b = bag0; b < bag1; b++) {
        uint32_t g0 = rd16(pbag.data + b * BAG_SIZE);
        uint32_t g1 = (b + 1 < nPbag) ? rd16(pbag.data + (b + 1) * BAG_SIZE) : nPgen;
        if (g1 > nPgen) g1 = nPgen;
        if (g0 >= g1) continue;

        // A preset zone without a trailing 'instrument' generator is the global zone
        bool hasInst = rd16(pgen.data + (g1 - 1) * GEN_SIZE) == gen::instrument;
        if (!hasInst) {
          if (b == bag0) applyGens(pGlobal, pgen.data, g0, g1);
          continue;
        }
        memcpy(pZone, pGlobal, sizeof(pZone));
        applyGens(pZone, pgen.data, g0, g1);
        uint32_t ii = (uint16_t)pZone[gen::instrument];
        if (ii + 1 >= nInst) continue;

        const uint8_t* in = inst.data + ii * INST_SIZE;
        uint32_t ib0 = rd16(in + 20), ib1 = rd16(in + INST_SIZE + 20);
        if (ib1 > nIbag) ib1 = nIbag;
        instrumentDefaults(iGlobal);

        for (uint32_t ib = ib0; ib < ib1; ib++) {
          uint32_t h0 = rd16(ibag.data + ib * BAG_SIZE);
          uint32_t h1 = (ib + 1 < nIbag) ? rd16(ibag.data + (ib + 1) * BAG_SIZE) : nIgen;
          if (h1 > nIgen) h1 = nIgen;
          if (h0 >= h1) continue;

          bool hasSample = rd16(igen.data + (h1 - 1) * GEN_SIZE) == gen::sampleID;
          if (!hasSample) {
            if (ib == ib0) applyGens(iGlobal, igen.data, h0, h1);
            continue;
          }
          memcpy(iZone, iGlobal, sizeof(iZone));
          applyGens(iZone, igen.data, h0, h1);

          uint32_t si = (uint16_t)iZone[gen::sampleID];
          if (si + 1 >= nShdr) continue;
          const uint8_t* sh = shdr.data + si * SHDR_SIZE;
          if (rd16(sh + 44) & 0x8000) continue;  // ROM sample

          // Key/velocity ranges are the intersection of both levels
          uint8_t loKey = max(rangeLo(pZone[gen::keyRange]), rangeLo(iZone[gen::keyRange]));
          uint8_t hiKey = min(rangeHi(pZone[gen::keyRange]), rangeHi(iZone[gen::keyRange]));
          uint8_t loVel = max(rangeLo(pZone[gen::velRange]), rangeLo(iZone[gen::velRange]));
          uint8_t hiVel = min(rangeHi(pZone[gen::velRange]), rangeHi(iZone[gen::velRange]));
          if (loKey > hiKey || loVel > hiVel) continue;

          Region scratch;
          Region& r = store ? outRegions[nRegions] : scratch;
          int16_t g[gen::COUNT];
          for (int op = 0; op < gen::COUNT; op++) {
            g[op] = isNonAdditive(op) ? iZone[op] : (int16_t)(iZone[op] + pZone[op]);
          }
          r.loKey = loKey;
          r.hiKey = hiKey;
          r.loVel = loVel;
          r.hiVel = hiVel;

          int64_t start = (int64_t)rd32(sh + 20) + g[gen::startAddrsOffset] + g[gen::startAddrsCoarseOffset] * 32768;
          int64_t end = (int64_t)rd32(sh + 24) + g[gen::endAddrsOffset] + g[gen::endAddrsCoarseOffset] * 32768;
          int64_t ls = (int64_t)rd32(sh + 28) + g[gen::startloopAddrsOffset] +
                       g[gen::startloopAddrsCoarseOffset] * 32768;
          int64_t le = (int64_t)rd32(sh + 32) + g[gen::endloopAddrsOffset] + g[gen::endloopAddrsCoarseOffset] * 32768;
          int64_t last = (int64_t)sampleCount - 1;
          start = constrain(start, 0, last);
          end = constrain(end, start, last);
          if (end - start < 2) continue;
          r.start = start;
          r.end = end;
          r.loopMode = g[gen::sampleModes] & 3;
          if (r.loopMode == 2) r.loopMode = 0;
          if (ls < start || le > end || le - ls < 2) r.loopMode = 0;
          r.loopStart = r.loopMode ? ls : start;
          r.loopEnd = r.loopMode ? le : end;

          r.sampleRate = rd32(sh + 36);
          if (r.sampleRate < 400) r.sampleRate = 44100;
          uint8_t origPitch = sh[40];
          if (origPitch > 127) origPitch = 60;
          int16_t rootOverride = g[gen::overridingRootKey];
          r.rootKey = (rootOverride >= 0 && rootOverride <= 127) ? rootOverride : origPitch;
          r.tuneCents = g[gen::coarseTune] * 100 + g[gen::fineTune] + (int8_t)sh[41];

          if (store) {
            int16_t params[PARAM_COUNT];
            for (int i = 0; i < PARAM_COUNT; i++) params[i] = g[kParamGen[i]];
            int32_t index = pool.intern(params);
            if (index < 0) return fail("out of memory");
            // The pool may still move; store the index and resolve it below
            r.gen = reinterpret_cast<const int16_t*>((uintptr_t)index);
          }
          nRegions++;
        }
      }
      pr.regionCount = nRegions - pr.firstRegion;
    }
  }

  // The pool was sized for one set per region; release the unused part
  if (pool.count) {
    if (int16_t* d = (int16_t*)platform::reallocLarge(pool.data, pool.count * PARAM_COUNT * sizeof(int16_t))) {
      pool.data = d;
    }
  }
  for (uint32_t i = 0; i < nRegions; i++) {
    outRegions[i].gen = pool.data + (uintptr_t)outRegions[i].gen * PARAM_COUNT;
  }

  presets = outPresets;
  presetCount = nPhdr - 1;
  regions = outRegions;
  regionCount = nRegions;
  paramSetCount = pool.count;
  if (!name_[0]) strcpy(name_, "SoundFont");
  return true;
}

const Preset* SoundFont::findPreset(uint16_t bank, uint16_t program) const {
  for (uint32_t i = 0; i < presetCount; i++) {
    if (presets[i].bank == bank && presets[i].program == program && presets[i].regionCount) {
      return &presets[i];
    }
  }
  return nullptr;
}

}  // namespace sf2
