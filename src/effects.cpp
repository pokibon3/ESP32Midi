#include "effects.h"

#include <Arduino.h>
#include <math.h>

namespace effects {

namespace {

float* allocBuffer(int n) {
  return (float*)heap_caps_calloc(n, sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

}  // namespace

// ---------------------------------------------------------------------------
// Freeverb (Jezar at Dreampoint, public domain), tunings scaled from 44.1kHz

bool Reverb::begin(float sampleRate) {
  static const int kCombTuning[NUM_COMBS] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
  static const int kAllpassTuning[NUM_ALLPASSES] = {556, 441, 341, 225};
  constexpr int kStereoSpread = 23;
  const float scale = sampleRate / 44100.0f;

  for (int c = 0; c < 2; c++) {
    int spread = c ? kStereoSpread : 0;
    for (int i = 0; i < NUM_COMBS; i++) {
      Comb& cb = comb_[c][i];
      cb.size = (int)((kCombTuning[i] + spread) * scale);
      cb.buf = allocBuffer(cb.size);
      cb.idx = 0;
      cb.store = 0;
      if (!cb.buf) return false;
    }
    for (int i = 0; i < NUM_ALLPASSES; i++) {
      Allpass& ap = allpass_[c][i];
      ap.size = (int)((kAllpassTuning[i] + spread) * scale);
      ap.buf = allocBuffer(ap.size);
      ap.idx = 0;
      if (!ap.buf) return false;
    }
  }
  setRoomSize(0.6f);
  setDamping(0.4f);
  return true;
}

void Reverb::setRoomSize(float v) { feedback_ = v * 0.28f + 0.7f; }

void Reverb::setDamping(float v) {
  damp1_ = v * 0.4f;
  damp2_ = 1.0f - damp1_;
}

void Reverb::process(const float* in, float* out, int n, float level) {
  constexpr float kFixedGain = 0.015f;
  constexpr int kMaxBlock = 256;
  float x[kMaxBlock], acc[kMaxBlock];
  if (n > kMaxBlock) n = kMaxBlock;
  for (int s = 0; s < n; s++) x[s] = in[s] * kFixedGain;

  // Each filter runs over the whole block so its state stays in registers
  for (int c = 0; c < 2; c++) {
    for (int s = 0; s < n; s++) acc[s] = 0;
    for (int i = 0; i < NUM_COMBS; i++) {
      Comb& cb = comb_[c][i];
      float* buf = cb.buf;
      int idx = cb.idx;
      const int size = cb.size;
      float store = cb.store;
      const float fb = feedback_, d1 = damp1_, d2 = damp2_;
      for (int s = 0; s < n; s++) {
        float y = buf[idx];
        store = y * d2 + store * d1;
        buf[idx] = x[s] + store * fb;
        if (++idx >= size) idx = 0;
        acc[s] += y;
      }
      cb.idx = idx;
      cb.store = store;
    }
    for (int i = 0; i < NUM_ALLPASSES; i++) {
      Allpass& ap = allpass_[c][i];
      float* buf = ap.buf;
      int idx = ap.idx;
      const int size = ap.size;
      for (int s = 0; s < n; s++) {
        float b = buf[idx];
        buf[idx] = acc[s] + b * 0.5f;
        if (++idx >= size) idx = 0;
        acc[s] = b - acc[s];
      }
      ap.idx = idx;
    }
    for (int s = 0; s < n; s++) out[s * 2 + c] += acc[s] * level;
  }
}

// ---------------------------------------------------------------------------
// Chorus: one delay line, two taps modulated by quadrature sine LFOs

bool Chorus::begin(float sampleRate) {
  fs_ = sampleRate;
  buf_ = allocBuffer(BUF_SIZE);
  if (!buf_) return false;
  phaseInc_ = 0.4f / sampleRate;    // 0.4 Hz
  baseDelay_ = 0.012f * sampleRate;  // 12 ms
  depth_ = 0.004f * sampleRate;      // +-4 ms
  return true;
}

void Chorus::process(const float* in, float* out, int n, float level) {
  constexpr int MASK = BUF_SIZE - 1;
  // LFO is evaluated at block boundaries and interpolated inside the block
  float p0 = phase_, p1 = phase_ + phaseInc_ * n;
  float dL0 = baseDelay_ + depth_ * sinf(2.0f * (float)M_PI * p0);
  float dL1 = baseDelay_ + depth_ * sinf(2.0f * (float)M_PI * p1);
  float dR0 = baseDelay_ + depth_ * cosf(2.0f * (float)M_PI * p0);
  float dR1 = baseDelay_ + depth_ * cosf(2.0f * (float)M_PI * p1);
  phase_ = p1 - floorf(p1);
  const float stepL = (dL1 - dL0) / n, stepR = (dR1 - dR0) / n;

  float dL = dL0, dR = dR0;
  for (int s = 0; s < n; s++) {
    buf_[idx_] = in[s] + lastL_ * feedback_;

    float rl = idx_ - dL;
    int il = (int)floorf(rl);
    float fl = rl - il;
    float a = buf_[il & MASK], b = buf_[(il + 1) & MASK];
    float yl = a + (b - a) * fl;

    float rr = idx_ - dR;
    int ir = (int)floorf(rr);
    float fr = rr - ir;
    a = buf_[ir & MASK];
    b = buf_[(ir + 1) & MASK];
    float yr = a + (b - a) * fr;

    lastL_ = yl;
    out[s * 2] += yl * level;
    out[s * 2 + 1] += yr * level;
    idx_ = (idx_ + 1) & MASK;
    dL += stepL;
    dR += stepR;
  }
}

}  // namespace effects
