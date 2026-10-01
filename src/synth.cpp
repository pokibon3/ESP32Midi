#include "synth.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

#include "audio_out.h"
#include "effects.h"

namespace synth {

using namespace sf2;

namespace {

constexpr int BLOCK = 64;  // frames per control/render block
constexpr int NUM_VOICES = SYNTH_MAX_VOICES;
constexpr int DRUM_CHANNEL = 9;
constexpr float SILENCE_DB = 96.0f;

// ---------------------------------------------------------------------------
// Unit helpers

inline float timecentsToSec(int tc) {
  if (tc <= -12000) return 0.0f;
  if (tc > 8000) tc = 8000;
  return exp2f(tc / 1200.0f);
}
// Fast 2^x (rel. error < 2e-5), used at control rate for every voice
inline float fastExp2(float x) {
  if (x < -126.0f) return 0.0f;
  if (x > 126.0f) x = 126.0f;
  float fl = floorf(x);
  float f = x - fl;
  float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
  union {
    float f;
    int32_t i;
  } u{p};
  u.i += (int32_t)fl << 23;
  return u.f;
}
// sin(x) for x in [-pi/2, pi/2]
inline float sinHalfPi(float x) {
  float x2 = x * x;
  return x * (1.0f - x2 * (1.0f / 6 - x2 * (1.0f / 120 - x2 * (1.0f / 5040 - x2 * (1.0f / 362880)))));
}
// sin / cos for x in [0, pi]
inline float fastSin(float x) { return sinHalfPi(x > (float)M_PI_2 ? (float)M_PI - x : x); }
inline float fastCos(float x) { return sinHalfPi((float)M_PI_2 - x); }

inline float absCentsToHz(float cents) { return 8.176f * fastExp2(cents * (1.0f / 1200.0f)); }
inline float dbToGain(float db) { return fastExp2(db * 0.16609640f); }  // log2(10)/20

// ---------------------------------------------------------------------------
// Envelopes / LFO (evaluated once per block)

enum Stage : uint8_t { DELAY, ATTACK, HOLD, DECAY, SUSTAIN, RELEASE, DONE };

// Volume envelope. Attack is linear in amplitude, decay/release linear in dB.
struct VolEnv {
  Stage stage;
  float t, lin, db;
  float delay, attack, hold, decayRate, sustainDb, releaseRate;

  void start() {
    stage = DELAY;
    t = 0;
    lin = 0;
    db = SILENCE_DB;
  }
  void release() {
    switch (stage) {
      case DELAY: stage = DONE; return;
      case ATTACK: db = -20.0f * log10f(fmaxf(lin, 1e-5f)); break;
      case HOLD: db = 0; break;
      default: break;
    }
    stage = RELEASE;
  }
  void kill() {  // fast fade, used for exclusive class and voice reuse
    release();
    releaseRate = 100.0f / 0.005f;
  }
  void process(float dt) {
    switch (stage) {
      case DELAY:
        if ((t += dt) >= delay) { stage = ATTACK; t = 0; }
        break;
      case ATTACK:
        lin = attack > 0 ? lin + dt / attack : 1.0f;
        if (lin >= 1.0f) { lin = 1.0f; db = 0; stage = HOLD; t = 0; }
        break;
      case HOLD:
        if ((t += dt) >= hold) stage = DECAY;
        break;
      case DECAY:
        db += decayRate * dt;
        if (db >= sustainDb) {
          db = sustainDb;
          stage = sustainDb >= SILENCE_DB ? DONE : SUSTAIN;
        }
        break;
      case SUSTAIN:
        break;
      case RELEASE:
        db += releaseRate * dt;
        if (db >= SILENCE_DB) stage = DONE;
        break;
      case DONE:
        break;
    }
  }
  float gain() const {
    switch (stage) {
      case DELAY: case DONE: return 0.0f;
      case ATTACK: return lin;
      default: return dbToGain(-db);
    }
  }
};

// Modulation envelope, 0..1 linear.
struct ModEnv {
  Stage stage;
  float t, value;
  float delay, attack, hold, decay, sustain, release;

  void start() { stage = DELAY; t = 0; value = 0; }
  void noteOff() { if (stage != DONE) stage = RELEASE; }
  void process(float dt) {
    switch (stage) {
      case DELAY:
        if ((t += dt) >= delay) { stage = ATTACK; t = 0; }
        break;
      case ATTACK:
        value = attack > 0 ? value + dt / attack : 1.0f;
        if (value >= 1.0f) { value = 1.0f; stage = HOLD; t = 0; }
        break;
      case HOLD:
        if ((t += dt) >= hold) stage = DECAY;
        break;
      case DECAY:
        value = decay > 0 ? value - dt / decay : sustain;
        if (value <= sustain) { value = sustain; stage = SUSTAIN; }
        break;
      case SUSTAIN:
        break;
      case RELEASE:
        value = release > 0 ? value - dt / release : 0.0f;
        if (value <= 0) { value = 0; stage = DONE; }
        break;
      case DONE:
        break;
    }
  }
};

// Triangle LFO, -1..1
struct Lfo {
  float delay, phase, inc, value;
  void start(float delaySec, float hz, float dt) {
    delay = delaySec;
    phase = 0;
    inc = hz * dt;
    value = 0;
  }
  void process(float dt) {
    if (delay > 0) { delay -= dt; return; }
    phase += inc;
    if (phase >= 1.0f) phase -= floorf(phase);
    value = phase < 0.25f ? phase * 4.0f : phase < 0.75f ? 2.0f - phase * 4.0f : phase * 4.0f - 4.0f;
  }
};

// ---------------------------------------------------------------------------

struct Channel {
  uint8_t program, bankMsb;
  uint8_t volume, expression, pan, modWheel;
  uint8_t reverb, chorus;  // CC91 / CC93
  bool sustain, drum;
  uint16_t rpn;
  int16_t bend;  // -8192..8191
  float bendRange;  // semitones
  const Preset* preset;
  // derived
  float gain, panPos, bendCents;
  float reverbSend, chorusSend;
};

struct Voice {
  bool active;
  bool released;   // note-off received (or pedal released)
  bool sustained;  // note-off received while sustain pedal held
  uint8_t ch, key, vel;
  uint8_t exclusiveClass;
  uint8_t loopMode;
  uint32_t age;
  const Region* r;

  uint64_t pos;  // 32.32 fixed point sample index
  uint32_t end, loopStart, loopEnd;

  float pitchCents;  // relative to root, incl. tuning
  float rateRatio;   // sampleRate / outputRate
  float noteGain;
  float panPos;
  float filterQ, filterGain;

  VolEnv vol;
  ModEnv mod;
  Lfo vib, modLfo;

  float amp;  // gain applied at end of previous block
  float revSend, choSend;  // send levels for this block
  float lastCents;
  uint64_t inc;
  float lastPan, panL, panR;
  bool filterOn;
  float lastFc;
  float b0, b1, b2, a1, a2, z1, z2;
};

struct Event {
  uint8_t status, d1, d2;
};

const SoundFont* g_sf;
float g_fs;
float g_dt;  // block duration
float g_master = 0.35f;
Channel g_ch[16];
Voice* g_voices;
uint32_t g_ageCounter;
Stats g_stats;

QueueHandle_t g_events;
TaskHandle_t g_audioTask, g_workerTask;

constexpr int SPLIT = NUM_VOICES / 2;
int16_t g_out[BLOCK * 2];

// ---------------------------------------------------------------------------
// Channel state

void updateChannelDerived(Channel& c) {
  float v = c.volume / 127.0f, e = c.expression / 127.0f;
  c.gain = v * v * e * e;
  c.panPos = (c.pan - 64) / 128.0f;
  c.bendCents = c.bend / 8192.0f * c.bendRange * 100.0f;
  c.reverbSend = c.reverb / 127.0f;
  c.chorusSend = c.chorus / 127.0f;
}

void selectPreset(Channel& c) {
  const Preset* p = nullptr;
  if (c.drum) {
    p = g_sf->findPreset(128, c.program);
    if (!p) p = g_sf->findPreset(128, 0);
  } else {
    p = g_sf->findPreset(c.bankMsb, c.program);
    if (!p) p = g_sf->findPreset(0, c.program);
  }
  c.preset = p;
}

void resetControllers(Channel& c) {
  c.expression = 127;
  c.modWheel = 0;
  c.sustain = false;
  c.bend = 0;
  c.rpn = 0x3FFF;
  updateChannelDerived(c);
}

void resetChannel(int i) {
  Channel& c = g_ch[i];
  memset(&c, 0, sizeof(c));
  c.volume = 100;
  c.pan = 64;
  c.reverb = 40;  // GM default
  c.chorus = 0;
  c.bendRange = 2.0f;
  c.drum = (i == DRUM_CHANNEL);
  resetControllers(c);
  selectPreset(c);
}

// ---------------------------------------------------------------------------
// Voice management

void releaseVoice(Voice& v) {
  v.released = true;
  v.sustained = false;
  v.vol.release();
  v.mod.noteOff();
}

Voice* allocVoice() {
  // Voices [0, SPLIT) render on core 1, [SPLIT, NUM_VOICES) on core 0.
  // Pick a free voice from the less loaded half to keep both cores busy.
  int activeA = 0, activeB = 0;
  Voice* freeA = nullptr;
  Voice* freeB = nullptr;
  for (int i = 0; i < NUM_VOICES; i++) {
    Voice& v = g_voices[i];
    if (v.active) {
      (i < SPLIT ? activeA : activeB)++;
    } else if (i < SPLIT) {
      if (!freeA) freeA = &v;
    } else if (!freeB) {
      freeB = &v;
    }
  }
  if (freeA && freeB) return activeA <= activeB ? freeA : freeB;
  if (freeA || freeB) return freeA ? freeA : freeB;

  Voice* best = nullptr;
  float bestScore = 1e30f;
  for (int i = 0; i < NUM_VOICES; i++) {
    Voice& v = g_voices[i];
    // Prefer stealing released & quiet voices, then the oldest ones.
    float score = (v.released ? 0.0f : 2.0f) + v.vol.gain() + (g_ageCounter - v.age) * -1e-7f;
    if (score < bestScore) {
      bestScore = score;
      best = &v;
    }
  }
  return best;
}

void startVoice(Voice& v, uint8_t ch, uint8_t key, uint8_t vel, const Region& r) {
  const int16_t* g = r.gen;
  int k = g[keynum] >= 0 ? g[keynum] : key;
  int vv = g[velocity] >= 0 ? g[velocity] : vel;

  v.active = true;
  v.released = false;
  v.sustained = false;
  v.ch = ch;
  v.key = key;
  v.vel = vel;
  v.exclusiveClass = (uint8_t)g[exclusiveClass];
  v.age = ++g_ageCounter;
  v.r = &r;

  v.pos = (uint64_t)r.start << 32;
  v.end = r.end;
  v.loopMode = r.loopMode;
  v.loopStart = r.loopStart;
  v.loopEnd = r.loopEnd;

  v.pitchCents = (k - r.rootKey) * g[scaleTuning] + r.tuneCents;
  v.rateRatio = (float)r.sampleRate / g_fs;

  // Attenuation: EMU-style 0.4 scale (as FluidSynth); velocity: GM curve
  float atten = constrain(g[initialAttenuation], 0, 1440) * 0.4f;
  float vf = vv / 127.0f;
  v.noteGain = powf(10.0f, -atten / 200.0f) * vf * vf;
  v.panPos = constrain(g[pan], -500, 500) / 1000.0f;

  float qDb = constrain(g[initialFilterQ], 0, 960) / 10.0f;
  float qLin = dbToGain(qDb);
  v.filterQ = 0.7071f * qLin;
  v.filterGain = 1.0f / sqrtf(qLin);
  v.filterOn = false;
  v.lastFc = -1;
  v.z1 = v.z2 = 0;

  VolEnv& e = v.vol;
  e.delay = timecentsToSec(g[delayVolEnv]);
  e.attack = timecentsToSec(g[attackVolEnv]);
  e.hold = timecentsToSec(g[holdVolEnv] + (60 - k) * g[keynumToVolEnvHold]);
  float decay = timecentsToSec(g[decayVolEnv] + (60 - k) * g[keynumToVolEnvDecay]);
  e.decayRate = decay > 0 ? 100.0f / decay : 1e6f;
  e.sustainDb = constrain(g[sustainVolEnv], 0, 1440) / 10.0f;
  float rel = timecentsToSec(g[releaseVolEnv]);
  e.releaseRate = 100.0f / fmaxf(rel, 0.005f);
  e.start();

  ModEnv& m = v.mod;
  m.delay = timecentsToSec(g[delayModEnv]);
  m.attack = timecentsToSec(g[attackModEnv]);
  m.hold = timecentsToSec(g[holdModEnv] + (60 - k) * g[keynumToModEnvHold]);
  m.decay = timecentsToSec(g[decayModEnv] + (60 - k) * g[keynumToModEnvDecay]);
  m.sustain = 1.0f - constrain(g[sustainModEnv], 0, 1000) / 1000.0f;
  m.release = timecentsToSec(g[releaseModEnv]);
  m.start();

  v.vib.start(timecentsToSec(g[delayVibLFO]), absCentsToHz(g[freqVibLFO]), g_dt);
  v.modLfo.start(timecentsToSec(g[delayModLFO]), absCentsToHz(g[freqModLFO]), g_dt);

  v.amp = 0;
  v.lastCents = 1e9f;
  v.lastPan = 1e9f;
}

void noteOff(uint8_t ch, uint8_t key) {
  Channel& c = g_ch[ch];
  for (int i = 0; i < NUM_VOICES; i++) {
    Voice& v = g_voices[i];
    if (!v.active || v.released || v.ch != ch || v.key != key) continue;
    if (c.sustain) {
      v.sustained = true;
    } else {
      releaseVoice(v);
    }
  }
}

void noteOn(uint8_t ch, uint8_t key, uint8_t vel) {
  if (vel == 0) {
    noteOff(ch, key);
    return;
  }
  const Preset* p = g_ch[ch].preset;
  if (!p) return;
  g_stats.activity[ch] = vel;

  // Voices started by this note-on (e.g. a stereo pair) must not cut each other
  const uint32_t ageBefore = g_ageCounter;
  const Region* regions = g_sf->regions + p->firstRegion;
  for (uint32_t i = 0; i < p->regionCount; i++) {
    const Region& r = regions[i];
    if (key < r.loKey || key > r.hiKey || vel < r.loVel || vel > r.hiVel) continue;
    uint8_t ex = (uint8_t)r.gen[exclusiveClass];
    if (ex) {
      for (int j = 0; j < NUM_VOICES; j++) {
        Voice& o = g_voices[j];
        if (o.active && o.ch == ch && o.exclusiveClass == ex && (int32_t)(o.age - ageBefore) <= 0) {
          o.vol.kill();
        }
      }
    }
    Voice* v = allocVoice();
    if (v) startVoice(*v, ch, key, vel, r);
  }
}

void allNotesOff(uint8_t ch) {
  for (int i = 0; i < NUM_VOICES; i++) {
    Voice& v = g_voices[i];
    if (v.active && v.ch == ch && !v.released) releaseVoice(v);
  }
}

void allSoundOff(uint8_t ch) {
  for (int i = 0; i < NUM_VOICES; i++) {
    Voice& v = g_voices[i];
    if (v.active && v.ch == ch) v.vol.kill();
  }
}

void controlChange(uint8_t ch, uint8_t cc, uint8_t val) {
  Channel& c = g_ch[ch];
  switch (cc) {
    case 0: c.bankMsb = val; break;
    case 1: c.modWheel = val; break;
    case 6:  // data entry MSB
      if (c.rpn == 0) {
        c.bendRange = val;
        updateChannelDerived(c);
      }
      break;
    case 7: c.volume = val; updateChannelDerived(c); break;
    case 10: c.pan = val; updateChannelDerived(c); break;
    case 11: c.expression = val; updateChannelDerived(c); break;
    case 91: c.reverb = val; updateChannelDerived(c); break;
    case 93: c.chorus = val; updateChannelDerived(c); break;
    case 64: {
      bool on = val >= 64;
      if (c.sustain && !on) {
        for (int i = 0; i < NUM_VOICES; i++) {
          Voice& v = g_voices[i];
          if (v.active && v.ch == ch && v.sustained) releaseVoice(v);
        }
      }
      c.sustain = on;
      break;
    }
    case 98: case 99: c.rpn = 0x3FFF; break;  // NRPN: unsupported
    case 100: c.rpn = (c.rpn & 0x3F80) | val; break;
    case 101: c.rpn = (c.rpn & 0x007F) | (val << 7); break;
    case 120: allSoundOff(ch); break;
    case 121: resetControllers(c); break;
    case 123: case 124: case 125: case 126: case 127: allNotesOff(ch); break;
    default: break;
  }
}

constexpr uint8_t EV_RESET = 0xFF;

void handleEvent(const Event& e) {
  if (e.status == EV_RESET) {
    for (int i = 0; i < NUM_VOICES; i++) {
      if (g_voices[i].active) g_voices[i].vol.kill();
    }
    for (int i = 0; i < 16; i++) resetChannel(i);
    return;
  }
  uint8_t ch = e.status & 0x0F;
  Channel& c = g_ch[ch];
  switch (e.status & 0xF0) {
    case 0x80: noteOff(ch, e.d1); break;
    case 0x90: noteOn(ch, e.d1, e.d2); break;
    case 0xB0: controlChange(ch, e.d1, e.d2); break;
    case 0xC0:
      c.program = e.d1;
      selectPreset(c);
      break;
    case 0xE0:
      c.bend = (int16_t)((e.d2 << 7) | e.d1) - 8192;
      updateChannelDerived(c);
      break;
    default: break;  // aftertouch: ignored
  }
}

// ---------------------------------------------------------------------------
// Rendering

// Per-block control update. Returns false when the voice has finished.
bool updateVoiceControl(Voice& v, uint64_t& inc, float& amp) {
  v.vol.process(g_dt);
  if (v.vol.stage == DONE) return false;
  v.mod.process(g_dt);
  v.vib.process(g_dt);
  v.modLfo.process(g_dt);

  const Channel& c = g_ch[v.ch];
  const int16_t* g = v.r->gen;
  float modEnv = v.mod.value;

  float cents = v.pitchCents + c.bendCents +
                v.vib.value * (g[vibLfoToPitch] + c.modWheel * (50.0f / 127.0f)) +
                v.modLfo.value * g[modLfoToPitch] + modEnv * g[modEnvToPitch];
  if (cents != v.lastCents) {
    v.lastCents = cents;
    v.inc = (uint64_t)(fastExp2(cents * (1.0f / 1200.0f)) * v.rateRatio * 4294967296.0f);
  }
  inc = v.inc;

  float fc = g[initialFilterFc] + modEnv * g[modEnvToFilterFc] + v.modLfo.value * g[modLfoToFilterFc];
  float fcHz = absCentsToHz(fminf(fc, 13500.0f));
  bool on = fc < 13500.0f && fcHz < g_fs * 0.45f;
  if (on && fabsf(fc - v.lastFc) > 2.0f) {
    float w0 = 2.0f * (float)M_PI * fmaxf(fcHz, 20.0f) / g_fs;
    float cw = fastCos(w0), alpha = fastSin(w0) / (2.0f * v.filterQ);
    float a0 = 1.0f / (1.0f + alpha);
    v.b1 = (1.0f - cw) * a0 * v.filterGain;
    v.b0 = v.b2 = v.b1 * 0.5f;
    v.a1 = -2.0f * cw * a0;
    v.a2 = (1.0f - alpha) * a0;
    v.lastFc = fc;
  }
  if (!on) {
    v.lastFc = -1;
    v.z1 = v.z2 = 0;
  }
  v.filterOn = on;

  amp = v.noteGain * c.gain * v.vol.gain();
  if (g[modLfoToVolume]) amp *= dbToGain(-v.modLfo.value * g[modLfoToVolume] * 0.1f);
  float p = constrain(v.panPos + c.panPos, -0.5f, 0.5f) + 0.5f;  // 0..1
  if (p != v.lastPan) {
    v.lastPan = p;
    float a = p * (float)M_PI_2;
    v.panL = fastCos(a);
    v.panR = fastSin(a);
  }
  // SF2 send generators (0.1% units) plus the channel CC91/CC93 level
  v.revSend = constrain(g[reverbEffectsSend] * 0.001f + c.reverbSend, 0.0f, 1.0f);
  v.choSend = constrain(g[chorusEffectsSend] * 0.001f + c.chorusSend, 0.0f, 1.0f);
  return true;
}

struct Bus {
  float dry[BLOCK * 2];
  float reverb[BLOCK];
  float chorus[BLOCK];
};

template <bool FILTER, bool SEND>
IRAM_ATTR void renderVoice(Voice& v, Bus& bus, uint64_t inc, float amp1) {
  const int16_t* s = g_sf->samples;
  float amp = v.amp;
  const float dAmp = (amp1 - amp) * (1.0f / BLOCK);
  const float pl = v.panL, pr = v.panR, rs = v.revSend, cs = v.choSend;
  float* mix = bus.dry;
  uint64_t pos = v.pos;
  const bool looping = v.loopMode == 1 || (v.loopMode == 3 && !v.released);
  const uint32_t limit = looping ? v.loopEnd : v.end;
  const uint64_t loopLen = (uint64_t)(v.loopEnd - v.loopStart) << 32;
  float z1 = v.z1, z2 = v.z2;
  const float b0 = v.b0, b1 = v.b1, b2 = v.b2, a1 = v.a1, a2 = v.a2;

  for (int i = 0; i < BLOCK; i++) {
    uint32_t idx = (uint32_t)(pos >> 32);
    float fr = (float)(uint32_t)pos * (1.0f / 4294967296.0f);
    float x0 = s[idx];
    float x = x0 + ((float)s[idx + 1] - x0) * fr;
    if (FILTER) {
      float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      x = y;
    }
    x *= amp;
    amp += dAmp;
    mix[i * 2] += x * pl;
    mix[i * 2 + 1] += x * pr;
    if (SEND) {
      bus.reverb[i] += x * rs;
      bus.chorus[i] += x * cs;
    }

    pos += inc;
    if ((uint32_t)(pos >> 32) >= limit) {
      if (looping) {
        do pos -= loopLen; while ((uint32_t)(pos >> 32) >= limit);
      } else {
        v.vol.stage = DONE;  // reached sample end
        break;
      }
    }
  }
  v.pos = pos;
  v.amp = amp1;
  v.z1 = z1;
  v.z2 = z2;
}

int renderRange(int from, int to, Bus& bus) {
  memset(&bus, 0, sizeof(bus));
  int active = 0;
  for (int i = from; i < to; i++) {
    Voice& v = g_voices[i];
    if (!v.active) continue;
    uint64_t inc;
    float amp;
    if (!updateVoiceControl(v, inc, amp)) {
      v.active = false;
      continue;
    }
    bool send = v.revSend > 0 || v.choSend > 0;
    if (v.filterOn) {
      send ? renderVoice<true, true>(v, bus, inc, amp) : renderVoice<true, false>(v, bus, inc, amp);
    } else {
      send ? renderVoice<false, true>(v, bus, inc, amp) : renderVoice<false, false>(v, bus, inc, amp);
    }
    if (v.vol.stage == DONE) v.active = false;
    active++;
  }
  return active;
}

Bus g_busA, g_busB;
float g_fxIn[BLOCK];
// Reverb runs on the worker core one block behind: input of block N-1,
// output added to block N.
float g_revIn[BLOCK];
float g_revOut[BLOCK * 2];
effects::Reverb g_reverb;
effects::Chorus g_chorus;
float g_reverbLevel = 1.0f, g_chorusLevel = 1.0f;

volatile int g_workerActive;

void workerTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    g_workerActive = renderRange(SPLIT, NUM_VOICES, g_busB);
    memset(g_revOut, 0, sizeof(g_revOut));
    g_reverb.process(g_revIn, g_revOut, BLOCK, g_reverbLevel);
    xTaskNotifyGive(g_audioTask);
  }
}

void audioTask(void*) {
  const float blockUs = BLOCK * 1e6f / g_fs;
  float load = 0;
  for (;;) {
    Event e;
    while (xQueueReceive(g_events, &e, 0) == pdTRUE) handleEvent(e);

    uint32_t t0 = micros();
    xTaskNotifyGive(g_workerTask);
    int active = renderRange(0, SPLIT, g_busA);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    active += g_workerActive;

    float* mix = g_busA.dry;
    for (int i = 0; i < BLOCK * 2; i++) mix[i] += g_busB.dry[i] + g_revOut[i];
    for (int i = 0; i < BLOCK; i++) g_revIn[i] = g_busA.reverb[i] + g_busB.reverb[i];
    for (int i = 0; i < BLOCK; i++) g_fxIn[i] = g_busA.chorus[i] + g_busB.chorus[i];
    g_chorus.process(g_fxIn, mix, BLOCK, g_chorusLevel);

    const float gain = g_master;
    for (int i = 0; i < BLOCK * 2; i++) {
      float x = mix[i] * gain;
      g_out[i] = (int16_t)constrain(x, -32767.0f, 32767.0f);
    }
    uint32_t elapsed = micros() - t0;

    float l = elapsed / blockUs;
    load = load * 0.98f + l * 0.02f;
    if (l > 1.0f) g_stats.underruns++;
    g_stats.cpuLoad = load;
    g_stats.activeVoices = active;
    if (active > g_stats.peakVoices) g_stats.peakVoices = active;
    for (int c = 0; c < 16; c++) {
      g_stats.program[c] = g_ch[c].program;
      g_stats.drum[c] = g_ch[c].drum;
      g_stats.presetName[c] = g_ch[c].preset ? g_ch[c].preset->name : "---";
    }

    audio_out::write(g_out, BLOCK);
  }
}

}  // namespace

bool begin(const SoundFont* sf, uint32_t sampleRate) {
  g_sf = sf;
  g_fs = sampleRate;
  g_dt = BLOCK / g_fs;
  g_voices = (Voice*)heap_caps_calloc(NUM_VOICES, sizeof(Voice), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!g_voices) return false;
  for (int i = 0; i < 16; i++) resetChannel(i);

  if (!g_reverb.begin(g_fs) || !g_chorus.begin(g_fs)) return false;

  g_events = xQueueCreate(512, sizeof(Event));
  if (!audio_out::begin(sampleRate)) return false;

  // The worker must exist before the audio task starts notifying it
  xTaskCreatePinnedToCore(workerTask, "synth2", 6144, nullptr, configMAX_PRIORITIES - 2, &g_workerTask, 0);
  xTaskCreatePinnedToCore(audioTask, "synth", 6144, nullptr, configMAX_PRIORITIES - 2, &g_audioTask, 1);
  return true;
}

void midiMessage(uint8_t status, uint8_t d1, uint8_t d2) {
  if (!g_events || status < 0x80 || status >= 0xF0) return;
  Event e{status, d1, d2};
  xQueueSend(g_events, &e, 0);
}

void reset() {
  if (!g_events) return;
  Event e{EV_RESET, 0, 0};
  xQueueSend(g_events, &e, 0);
}

void sysex(const uint8_t* d, size_t len) {
  // GM System On: F0 7E xx 09 01 F7 / GS Reset: F0 41 xx 42 12 40 00 7F 00 41 F7
  // XG System On: F0 43 1x 4C 00 00 7E 00 F7
  if (len >= 6 && d[1] == 0x7E && d[3] == 0x09 && (d[4] == 0x01 || d[4] == 0x03)) {
    reset();
  } else if (len >= 11 && d[1] == 0x41 && d[3] == 0x42 && d[4] == 0x12 && d[5] == 0x40 &&
             d[6] == 0x00 && d[7] == 0x7F) {
    reset();
  } else if (len >= 9 && d[1] == 0x43 && (d[2] & 0xF0) == 0x10 && d[3] == 0x4C && d[6] == 0x7E) {
    reset();
  }
}

void setMasterGain(float gain) { g_master = gain; }

void setEffectLevels(float reverb, float chorus) {
  g_reverbLevel = reverb;
  g_chorusLevel = chorus;
}

Stats& stats() { return g_stats; }

}  // namespace synth
