// Platform layer: memory, SoundFont flash mapping and the two-core audio scheduling.
// Implementations: src/platform/esp32 (ESP32-S3), src/platform/rp2350 (RP2350).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Hot audio code. ESP32: IRAM. RP2350: SRAM, so that sample reads through the
// XIP cache do not evict the render loop.
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_attr.h>
#define AUDIO_HOT IRAM_ATTR
#else
#define AUDIO_HOT __attribute__((section(".time_critical.audio"), noinline))
#endif

namespace platform {

// First thing in setup(). RP2350: raises the clock to SYNTH_CPU_MHZ if set.
void init();

// Zeroed memory in fast internal RAM (voices, effect buffers).
void* allocFast(size_t size);
// Zeroed memory for large tables (PSRAM on ESP32).
void* allocLarge(size_t size);
void* reallocLarge(void* p, size_t size);

// Memory-maps the raw SoundFont written by `pio run -t uploadsf2`.
// Returns nullptr on success, otherwise an error message.
const char* mapSoundFont(const uint8_t*& data, uint32_t& size);

// Runs `block` forever on the audio core. `helperJob` runs on the other core
// between helperStart() and helperWait(), both called from `block`.
bool startAudio(void (*block)(), void (*helperJob)());
void helperStart();
void helperWait();

// Called from loop(). ESP32: sleeps 1ms. RP2350: runs a pending helper job
// (core 0 is the helper core and has no other scheduler).
void idle();

// Reboots into the USB bootloader (RP2350 BOOTSEL). No-op on ESP32.
void rebootToBootloader();

// Diagnostics (-DDIAG_LED): records the main-loop stage; core 1 shows it on
// LED_PIN (fast blink = loop alive, N blinks + pause = stuck at stage N).
void diagStage(uint8_t stage);

uint32_t freeHeap();
uint32_t freePsram();

}  // namespace platform
