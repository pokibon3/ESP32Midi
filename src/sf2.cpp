#include "sf2.h"

#include <Arduino.h>
#include <esp_partition.h>
#include <string.h>

namespace sf2 {

namespace {

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
    case startAddrsOffset: case endAddrsOffset: case startloopAddrsOffset:
    case endloopAddrsOffset: case startAddrsCoarseOffset: case endAddrsCoarseOffset:
    case startloopAddrsCoarseOffset: case endloopAddrsCoarseOffset:
    case instrument: case keyRange: case velRange: case keynum: case velocity:
    case sampleID: case sampleModes: case exclusiveClass: case overridingRootKey:
      return true;
    default:
      return false;
  }
}

void instrumentDefaults(int16_t* g) {
  memset(g, 0, sizeof(int16_t) * GEN_COUNT);
  g[initialFilterFc] = 13500;
  for (int op : {delayModLFO, delayVibLFO, delayModEnv, attackModEnv, holdModEnv, decayModEnv,
                 releaseModEnv, delayVolEnv, attackVolEnv, holdVolEnv, decayVolEnv, releaseVolEnv}) {
    g[op] = -12000;
  }
  g[keyRange] = 0x7F00;
  g[velRange] = 0x7F00;
  g[keynum] = -1;
  g[velocity] = -1;
  g[scaleTuning] = 100;
  g[overridingRootKey] = -1;
}

void presetDefaults(int16_t* g) {
  memset(g, 0, sizeof(int16_t) * GEN_COUNT);
  g[keyRange] = 0x7F00;
  g[velRange] = 0x7F00;
}

void applyGens(int16_t* g, const uint8_t* gens, uint32_t first, uint32_t last) {
  for (uint32_t i = first; i < last; i++) {
    const uint8_t* rec = gens + i * GEN_SIZE;
    uint16_t op = rd16(rec);
    if (op < GEN_COUNT) g[op] = (int16_t)rd16(rec + 2);
  }
}

// Range generators: low byte = lo, high byte = hi
inline uint8_t rangeLo(int16_t v) { return (uint16_t)v & 0xFF; }
inline uint8_t rangeHi(int16_t v) { return (uint16_t)v >> 8; }

}  // namespace

bool SoundFont::loadFromPartition(const char* label) {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
  if (!part) return fail("sf2 partition not found");

  uint8_t head[12];
  if (esp_partition_read(part, 0, head, sizeof(head)) != ESP_OK) return fail("partition read failed");
  if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "sfbk", 4) != 0) {
    return fail("no SoundFont in flash (run: pio run -t uploadsf2)");
  }
  uint32_t size = rd32(head + 4) + 8;
  if (size > part->size) return fail("SoundFont larger than partition");

  const void* ptr = nullptr;
  esp_partition_mmap_handle_t handle;
  if (esp_partition_mmap(part, 0, size, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK) {
    return fail("mmap failed");
  }
  return parse((const uint8_t*)ptr, size);
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

  Preset* outPresets = (Preset*)ps_calloc(nPhdr - 1, sizeof(Preset));
  uint32_t regionCap = 1024, nRegions = 0;
  Region* outRegions = (Region*)ps_malloc(regionCap * sizeof(Region));
  if (!outPresets || !outRegions) return fail("out of memory");

  int16_t pGlobal[GEN_COUNT], pZone[GEN_COUNT], iGlobal[GEN_COUNT], iZone[GEN_COUNT];

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
      bool hasInst = rd16(pgen.data + (g1 - 1) * GEN_SIZE) == instrument;
      if (!hasInst) {
        if (b == bag0) applyGens(pGlobal, pgen.data, g0, g1);
        continue;
      }
      memcpy(pZone, pGlobal, sizeof(pZone));
      applyGens(pZone, pgen.data, g0, g1);
      uint32_t ii = (uint16_t)pZone[instrument];
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

        bool hasSample = rd16(igen.data + (h1 - 1) * GEN_SIZE) == sampleID;
        if (!hasSample) {
          if (ib == ib0) applyGens(iGlobal, igen.data, h0, h1);
          continue;
        }
        memcpy(iZone, iGlobal, sizeof(iZone));
        applyGens(iZone, igen.data, h0, h1);

        uint32_t si = (uint16_t)iZone[sampleID];
        if (si + 1 >= nShdr) continue;
        const uint8_t* sh = shdr.data + si * SHDR_SIZE;
        if (rd16(sh + 44) & 0x8000) continue;  // ROM sample

        // Key/velocity ranges are the intersection of both levels
        uint8_t loKey = max(rangeLo(pZone[keyRange]), rangeLo(iZone[keyRange]));
        uint8_t hiKey = min(rangeHi(pZone[keyRange]), rangeHi(iZone[keyRange]));
        uint8_t loVel = max(rangeLo(pZone[velRange]), rangeLo(iZone[velRange]));
        uint8_t hiVel = min(rangeHi(pZone[velRange]), rangeHi(iZone[velRange]));
        if (loKey > hiKey || loVel > hiVel) continue;

        if (nRegions == regionCap) {
          regionCap *= 2;
          Region* grown = (Region*)ps_realloc(outRegions, regionCap * sizeof(Region));
          if (!grown) return fail("out of memory");
          outRegions = grown;
        }
        Region& r = outRegions[nRegions];
        for (int op = 0; op < GEN_COUNT; op++) {
          r.gen[op] = isNonAdditive(op) ? iZone[op] : (int16_t)(iZone[op] + pZone[op]);
        }
        const int16_t* g = r.gen;
        r.loKey = loKey;
        r.hiKey = hiKey;
        r.loVel = loVel;
        r.hiVel = hiVel;

        int64_t start = (int64_t)rd32(sh + 20) + g[startAddrsOffset] + g[startAddrsCoarseOffset] * 32768;
        int64_t end = (int64_t)rd32(sh + 24) + g[endAddrsOffset] + g[endAddrsCoarseOffset] * 32768;
        int64_t ls = (int64_t)rd32(sh + 28) + g[startloopAddrsOffset] + g[startloopAddrsCoarseOffset] * 32768;
        int64_t le = (int64_t)rd32(sh + 32) + g[endloopAddrsOffset] + g[endloopAddrsCoarseOffset] * 32768;
        int64_t last = (int64_t)sampleCount - 1;
        start = constrain(start, 0, last);
        end = constrain(end, start, last);
        if (end - start < 2) continue;
        r.start = start;
        r.end = end;
        r.loopMode = g[sampleModes] & 3;
        if (r.loopMode == 2) r.loopMode = 0;
        if (ls < start || le > end || le - ls < 2) r.loopMode = 0;
        r.loopStart = r.loopMode ? ls : start;
        r.loopEnd = r.loopMode ? le : end;

        r.sampleRate = rd32(sh + 36);
        if (r.sampleRate < 400) r.sampleRate = 44100;
        uint8_t origPitch = sh[40];
        if (origPitch > 127) origPitch = 60;
        r.rootKey = (g[overridingRootKey] >= 0 && g[overridingRootKey] <= 127) ? g[overridingRootKey] : origPitch;
        r.tuneCents = g[coarseTune] * 100 + g[fineTune] + (int8_t)sh[41];
        nRegions++;
      }
    }
    pr.regionCount = nRegions - pr.firstRegion;
  }

  presets = outPresets;
  presetCount = nPhdr - 1;
  regions = outRegions;
  regionCount = nRegions;
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
