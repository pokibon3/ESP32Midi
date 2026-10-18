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
// Scratch space while a preset is built; the result is copied out.
class ParamPool {
 public:
  ~ParamPool() {
    free(table_);
    free(data);
  }

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
      uint32_t newCap = cap ? cap + cap / 2 : 256;
      int16_t* grown = (int16_t*)platform::reallocLarge(data, newCap * BYTES);
      if (!grown) return -1;
      data = grown;
      cap = newCap;
    }
    memcpy(data + count * PARAM_COUNT, p, BYTES);
    insert(count);
    return count++;
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
  pbag_ = pbag.data;
  pgen_ = pgen.data;
  inst_ = inst.data;
  ibag_ = ibag.data;
  igen_ = igen.data;
  shdr_ = shdr.data;
  nPbag_ = pbag.size / BAG_SIZE;
  nPgen_ = pgen.size / GEN_SIZE;
  nInst_ = inst.size / INST_SIZE;
  nIbag_ = ibag.size / BAG_SIZE;
  nIgen_ = igen.size / GEN_SIZE;
  nShdr_ = shdr.size / SHDR_SIZE;

  const uint32_t nPhdr = phdr.size / PHDR_SIZE;  // includes terminal EOP record
  if (nPhdr < 2 || nInst_ < 2 || nShdr_ < 2) return fail("SF2 has no presets");

  Preset* outPresets = (Preset*)platform::allocLarge((nPhdr - 1) * sizeof(Preset));
  if (!outPresets) return fail("out of memory");
  regionCount = 0;
  for (uint32_t pi = 0; pi + 1 < nPhdr; pi++) {
    const uint8_t* ph = phdr.data + pi * PHDR_SIZE;
    Preset& pr = outPresets[pi];
    memcpy(pr.name, ph, 20);
    pr.name[20] = 0;
    pr.program = rd16(ph + 20);
    pr.bank = rd16(ph + 22);
    pr.bag0 = rd16(ph + 24);
    pr.bag1 = rd16(ph + PHDR_SIZE + 24);
    walkRegions(pr, [&](const Region&, const int16_t*) { pr.regionCount++; });
    regionCount += pr.regionCount;
  }
  presets = outPresets;
  presetCount = nPhdr - 1;
  if (!name_[0]) strcpy(name_, "SoundFont");
  return true;
}

// Flattens the preset's zones into regions: emit(region, merged generators).
// region.gen is left unset.
template <typename Emit>
void SoundFont::walkRegions(const Preset& pr, Emit&& emit) const {
  int16_t pGlobal[gen::COUNT], pZone[gen::COUNT], iGlobal[gen::COUNT], iZone[gen::COUNT];
  uint32_t bag1 = pr.bag1 > nPbag_ ? nPbag_ : pr.bag1;
  presetDefaults(pGlobal);

  for (uint32_t b = pr.bag0; b < bag1; b++) {
    uint32_t g0 = rd16(pbag_ + b * BAG_SIZE);
    uint32_t g1 = (b + 1 < nPbag_) ? rd16(pbag_ + (b + 1) * BAG_SIZE) : nPgen_;
    if (g1 > nPgen_) g1 = nPgen_;
    if (g0 >= g1) continue;

    // A preset zone without a trailing 'instrument' generator is the global zone
    bool hasInst = rd16(pgen_ + (g1 - 1) * GEN_SIZE) == gen::instrument;
    if (!hasInst) {
      if (b == pr.bag0) applyGens(pGlobal, pgen_, g0, g1);
      continue;
    }
    memcpy(pZone, pGlobal, sizeof(pZone));
    applyGens(pZone, pgen_, g0, g1);
    uint32_t ii = (uint16_t)pZone[gen::instrument];
    if (ii + 1 >= nInst_) continue;

    const uint8_t* in = inst_ + ii * INST_SIZE;
    uint32_t ib0 = rd16(in + 20), ib1 = rd16(in + INST_SIZE + 20);
    if (ib1 > nIbag_) ib1 = nIbag_;
    instrumentDefaults(iGlobal);

    for (uint32_t ib = ib0; ib < ib1; ib++) {
      uint32_t h0 = rd16(ibag_ + ib * BAG_SIZE);
      uint32_t h1 = (ib + 1 < nIbag_) ? rd16(ibag_ + (ib + 1) * BAG_SIZE) : nIgen_;
      if (h1 > nIgen_) h1 = nIgen_;
      if (h0 >= h1) continue;

      bool hasSample = rd16(igen_ + (h1 - 1) * GEN_SIZE) == gen::sampleID;
      if (!hasSample) {
        if (ib == ib0) applyGens(iGlobal, igen_, h0, h1);
        continue;
      }
      memcpy(iZone, iGlobal, sizeof(iZone));
      applyGens(iZone, igen_, h0, h1);

      uint32_t si = (uint16_t)iZone[gen::sampleID];
      if (si + 1 >= nShdr_) continue;
      const uint8_t* sh = shdr_ + si * SHDR_SIZE;
      if (rd16(sh + 44) & 0x8000) continue;  // ROM sample

      // Key/velocity ranges are the intersection of both levels
      uint8_t loKey = max(rangeLo(pZone[gen::keyRange]), rangeLo(iZone[gen::keyRange]));
      uint8_t hiKey = min(rangeHi(pZone[gen::keyRange]), rangeHi(iZone[gen::keyRange]));
      uint8_t loVel = max(rangeLo(pZone[gen::velRange]), rangeLo(iZone[gen::velRange]));
      uint8_t hiVel = min(rangeHi(pZone[gen::velRange]), rangeHi(iZone[gen::velRange]));
      if (loKey > hiKey || loVel > hiVel) continue;

      Region r;
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
      r.gen = nullptr;
      emit(r, g);
    }
  }
}

// Builds the preset's region table: Region[regionCount] followed by the
// distinct generator arrays, in one allocation.
bool SoundFont::build(Preset& p) {
  ParamPool pool;
  int16_t params[PARAM_COUNT];
  bool ok = true;
  walkRegions(p, [&](const Region&, const int16_t* g) {
    for (int i = 0; i < PARAM_COUNT; i++) params[i] = g[kParamGen[i]];
    if (pool.intern(params) < 0) ok = false;
  });
  if (!ok) return false;

  const uint32_t bytes = p.regionCount * sizeof(Region) + pool.count * PARAM_COUNT * sizeof(int16_t);
  void* mem = platform::allocLarge(bytes);
  while (!mem && evictOne(&p)) mem = platform::allocLarge(bytes);
  if (!mem) return false;

  Region* regs = (Region*)mem;
  int16_t* sets = (int16_t*)(regs + p.regionCount);
  memcpy(sets, pool.data, pool.count * PARAM_COUNT * sizeof(int16_t));
  uint32_t n = 0;
  walkRegions(p, [&](const Region& r, const int16_t* g) {
    for (int i = 0; i < PARAM_COUNT; i++) params[i] = g[kParamGen[i]];
    regs[n] = r;
    regs[n].gen = sets + pool.intern(params) * PARAM_COUNT;  // already present: no allocation
    n++;
  });
  p.regions = regs;
  p.bytes = bytes;
  cacheBytes_ += bytes;
  return true;
}

// Frees the least recently used built preset that is not `keep` and not in use.
bool SoundFont::evictOne(const Preset* keep) {
  Preset* victim = nullptr;
  for (uint32_t i = 0; i < presetCount; i++) {
    Preset& p = presets[i];
    if (!p.regions || &p == keep || (inUse_ && inUse_(&p))) continue;
    if (!victim || (int32_t)(p.lastUse - victim->lastUse) < 0) victim = &p;
  }
  if (!victim) return false;
  free(victim->regions);
  victim->regions = nullptr;
  cacheBytes_ -= victim->bytes;
  victim->bytes = 0;
  return true;
}

const Region* SoundFont::regionsOf(const Preset* preset) {
  Preset& p = presets[preset - presets];
  p.lastUse = ++useCounter_;
  if (p.regions || !p.regionCount) return p.regions;
  // Upper bound of the table size (no shared generator arrays)
  const uint32_t estimate = p.regionCount * (sizeof(Region) + PARAM_COUNT * sizeof(int16_t));
  while (cacheBytes_ + estimate > cacheLimit_ && evictOne(&p)) {
  }
  if (!build(p)) {
    while (evictOne(&p)) {
    }
    build(p);
  }
  return p.regions;
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
