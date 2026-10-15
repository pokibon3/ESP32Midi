// RP2350 (arduino-pico): core 1 runs the audio loop (loop1), core 0 renders the
// helper half of the voices from loop(). If core 0 has not picked the job up by
// the time core 1 needs it, core 1 renders it itself, so a busy core 0 costs
// voices but never stalls the audio.
#include "platform.h"

#include <Arduino.h>
#include <hardware/clocks.h>
#include <hardware/structs/qmi.h>
#include <hardware/vreg.h>
#include <pico/time.h>

#include <atomic>

#include "board.h"
#include <stdlib.h>
#include <string.h>

// Filesystem area of the flash (board_build.filesystem_size); holds the raw SF2
extern "C" uint8_t _FS_start;
extern "C" uint8_t _FS_end;

namespace platform {

#ifdef SYNTH_CPU_MHZ
// Must not run from flash while the flash timing changes
static void __not_in_flash_func(slowFlashClock)(uint32_t div) {
  uint32_t t = qmi_hw->m[0].timing;
  t = (t & ~QMI_M0_TIMING_CLKDIV_BITS) | (div << QMI_M0_TIMING_CLKDIV_LSB);
  qmi_hw->m[0].timing = t;
  (void)*(volatile uint32_t*)XIP_BASE;  // dummy access, then barrier, for the change to take effect
  __asm volatile("dsb" ::: "memory");
}
#endif

void init() {
#ifdef SYNTH_CPU_MHZ
  // Overclock after boot instead of via F_CPU: the core voltage has to rise and
  // the flash clock (sys / CLKDIV) has to stay below ~100MHz first.
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  busy_wait_ms(10);
  uint32_t div = (SYNTH_CPU_MHZ + 99) / 100 + 1;  // 240MHz -> /4 = 60MHz
  slowFlashClock(div);
  set_sys_clock_khz(SYNTH_CPU_MHZ * 1000, true);
#endif
}

void* allocFast(size_t size) { return calloc(1, size); }
void* allocLarge(size_t size) { return calloc(1, size); }
void* reallocLarge(void* p, size_t size) { return realloc(p, size); }

const char* mapSoundFont(const uint8_t*& data, uint32_t& size) {
  const uint8_t* p = &_FS_start;
  const uint32_t area = &_FS_end - &_FS_start;
  if (area < 12) return "no SF2 area (board_build.filesystem_size)";
  if (memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "sfbk", 4) != 0) {
    return "no SoundFont in flash (run: pio run -t uploadsf2)";
  }
  uint32_t len;
  memcpy(&len, p + 4, 4);
  size = len + 8;
  if (size > area) return "SoundFont larger than SF2 area";
  data = p;  // XIP: flash is mapped at 0x10000000
  return nullptr;
}

namespace {

enum : int { IDLE, REQUESTED, RUNNING, DONE };

void (*volatile s_block)();
void (*s_helperJob)();
std::atomic<int> s_helper{IDLE};

bool claim() {
  int expected = REQUESTED;
  return s_helper.compare_exchange_strong(expected, RUNNING, std::memory_order_acquire);
}

}  // namespace

bool startAudio(void (*block)(), void (*helperJob)()) {
  s_helperJob = helperJob;
  s_block = block;
  return true;
}

void AUDIO_HOT helperStart() { s_helper.store(REQUESTED, std::memory_order_release); }

void AUDIO_HOT helperWait() {
  if (claim()) {  // core 0 did not get to it
    s_helperJob();
  } else {
    while (s_helper.load(std::memory_order_acquire) != DONE) {
    }
  }
  s_helper.store(IDLE, std::memory_order_relaxed);
}

void AUDIO_HOT idle() {
  if (claim()) {
    s_helperJob();
    s_helper.store(DONE, std::memory_order_release);
  }
}

void rebootToBootloader() { rp2040.rebootToBootloader(); }

namespace {
volatile uint8_t s_stage;
volatile uint32_t s_stageCount;
}  // namespace

void diagStage(uint8_t stage) {
  s_stage = stage;
  s_stageCount++;
}

uint32_t freeHeap() { return rp2040.getFreeHeap(); }
uint32_t freePsram() { return 0; }

}  // namespace platform

// 8KB stack for core 1 (default 4KB); the reverb alone uses 2KB when core 1
// renders the helper half itself
bool core1_separate_stack = true;

#if defined(DIAG_LED) && defined(LED_PIN)
// Runs on core 1 from its own timer IRQ, so it keeps going while core 1 renders
static bool diagTick(repeating_timer_t*) {
  static uint32_t lastCount, tick, stalledTicks;
  static uint8_t phase;
  uint32_t c = platform::s_stageCount;
  bool alive = c != lastCount;
  lastCount = c;
  stalledTicks = alive ? 0 : stalledTicks + 1;
  tick++;
  if (stalledTicks < 20) {  // 50ms ticks: loop alive within the last second
    gpio_put(LED_PIN, (tick / 5) & 1);
    phase = 0;
    return true;
  }
  // N blinks (200ms on / 200ms off), then 1.6s off
  uint32_t n = platform::s_stage;
  uint32_t slot = phase / 4;  // 4 ticks = 200ms
  gpio_put(LED_PIN, slot < n * 2 && !(slot & 1));
  if (++phase >= (n * 2 + 8) * 4) phase = 0;
  return true;
}
#endif

void setup1() {
#if defined(DIAG_LED) && defined(LED_PIN)
  static repeating_timer_t timer;
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);
  alarm_pool_t* pool = alarm_pool_create_with_unused_hardware_alarm(4);
  alarm_pool_add_repeating_timer_ms(pool, 50, diagTick, nullptr, &timer);
#endif
}

void loop1() {
  if (platform::s_block) platform::s_block();
}
