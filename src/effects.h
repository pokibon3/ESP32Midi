// Send effects: reverb (Freeverb) and stereo chorus
#pragma once
#include <stdint.h>

namespace effects {

class Reverb {
 public:
  bool begin(float sampleRate);
  void setRoomSize(float v);  // 0..1
  void setDamping(float v);   // 0..1
  // Adds the stereo wet signal of mono `in` to interleaved `out`.
  void process(const float* in, float* out, int n, float level);

 private:
  struct Comb {
    float* buf;
    int size, idx;
    float store;
  };
  struct Allpass {
    float* buf;
    int size, idx;
  };
  static constexpr int NUM_COMBS = 8;
  static constexpr int NUM_ALLPASSES = 4;
  Comb comb_[2][NUM_COMBS];
  Allpass allpass_[2][NUM_ALLPASSES];
  float feedback_ = 0.84f;
  float damp1_ = 0.2f, damp2_ = 0.8f;
};

class Chorus {
 public:
  bool begin(float sampleRate);
  // Adds the stereo wet signal of mono `in` to interleaved `out`.
  void process(const float* in, float* out, int n, float level);

 private:
  static constexpr int BUF_SIZE = 2048;  // power of two, > max delay
  float* buf_ = nullptr;
  int idx_ = 0;
  float fs_ = 32000;
  float phase_ = 0, phaseInc_ = 0;
  float baseDelay_ = 0, depth_ = 0;
  float feedback_ = 0.25f;
  float lastL_ = 0;
};

}  // namespace effects
