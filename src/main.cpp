// SoundFont GM synthesizer (ESP32-S3 / RP2350)
//
//  MIDI in : USB-MIDI (native USB port) / UART (MIDI_UART_RX_PIN)
//  Audio   : BOX-Lite: ES8156 DAC -> speaker / XIAO: PCM5102A -> stereo line out
//            RP2350: PCM5100A -> headphone amp
//  Buttons : BOX-Lite: PREV = volume down, NEXT = volume up,
//                      ENTER = panic (short press) / GM reset (hold 1 s)
//            XIAO:     BOOT = panic / GM reset (hold 1 s) (volume: GM Master Volume SysEx)
//            RP2350:   none (volume: GM Master Volume SysEx)
#include <Arduino.h>
#include <stdarg.h>
#include <string.h>

#include "audio_out.h"
#include "board.h"
#include "display.h"
#include "midi_in.h"
#include "platform.h"
#include "sf2.h"
#include "synth.h"

static sf2::SoundFont soundFont;
#ifndef DEFAULT_MASTER_GAIN
#define DEFAULT_MASTER_GAIN 0.35f
#endif
static float masterGain = DEFAULT_MASTER_GAIN;
// Heap left free for the rest of the firmware when presets are cached in it (no PSRAM)
#ifndef SF2_HEAP_RESERVE
#define SF2_HEAP_RESERVE (32 * 1024)
#endif

// Boot messages are kept and repeated when a serial monitor connects
// (native USB: the host is usually not listening yet while setup() runs).
static char bootLog[384];

static void bootPrintf(const char* fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  Serial.print(line);
  strncat(bootLog, line, sizeof(bootLog) - strlen(bootLog) - 1);
}

static bool running;  // synth started; the UI stays idle otherwise

// With a DAC volume control (ES8156) the synth keeps the default level and the
// DAC does the attenuation, so quiet settings keep the full sample resolution.
static void applyGain() {
  if (audio_out::setGain(masterGain / DEFAULT_MASTER_GAIN)) {
    synth::setMasterGain(DEFAULT_MASTER_GAIN);
  } else {
    synth::setMasterGain(masterGain);
  }
}

static void printStatus() {
  const synth::Stats& st = synth::stats();
  Serial.print(bootLog);
  Serial.printf("status: blocks %lu, voices %u, cpu %d%%, late %lu, xrun %lu, free heap %lu, presets %lu bytes\n",
                (unsigned long)st.blocks, st.activeVoices, (int)(st.cpuLoad * 100), (unsigned long)st.lateBlocks,
                (unsigned long)st.underruns, (unsigned long)platform::freeHeap(),
                (unsigned long)soundFont.cacheBytes());
}

enum Button { BTN_NONE, BTN_PREV, BTN_ENTER, BTN_NEXT };

static Button readButton() {
#if HAS_ADC_BUTTONS
  // Front buttons are a resistor ladder on GPIO1 (values from esp-bsp)
  int mv = analogReadMilliVolts(BTN_ADC_PIN);
  if (mv > 2310 && mv < 2510) return BTN_PREV;
  if (mv > 1880 && mv < 2080) return BTN_ENTER;
  if (mv > 720 && mv < 920) return BTN_NEXT;
  return BTN_NONE;
#elif defined(BTN_BOOT_PIN)
  return digitalRead(BTN_BOOT_PIN) == LOW ? BTN_ENTER : BTN_NONE;
#else
  return BTN_NONE;
#endif
}

static void updateLed() {
#if defined(LED_PIN) && !defined(DIAG_LED)
  bool on = synth::stats().activeVoices != 0;
  digitalWrite(LED_PIN, on != LED_ACTIVE_LOW ? HIGH : LOW);
#endif
}

static void handleButtons() {
  static Button last = BTN_NONE;
  static uint32_t pressedAt = 0;
  static uint32_t repeatAt = 0;
  static bool enterLong = false;
  constexpr uint32_t kLongPressMs = 1000;
  Button b = readButton();
  uint32_t now = millis();

  if (b != last) {
    // ENTER acts on release (panic) or after a 1 s hold (GM reset)
    if (last == BTN_ENTER && !enterLong) synth::panic();
    pressedAt = now;
    repeatAt = now + 400;
    enterLong = false;
    last = b;
    if (b == BTN_PREV || b == BTN_NEXT) {
      masterGain = b == BTN_PREV ? max(0.0f, masterGain - 0.02f) : min(1.0f, masterGain + 0.02f);
      applyGain();
    }
    return;
  }

  if (b == BTN_ENTER) {
    if (!enterLong && now - pressedAt >= kLongPressMs) {
      enterLong = true;
      synth::reset();
    }
  } else if ((b == BTN_PREV || b == BTN_NEXT) && now >= repeatAt) {
    repeatAt = now + 80;
    masterGain = b == BTN_PREV ? max(0.0f, masterGain - 0.02f) : min(1.0f, masterGain + 0.02f);
    applyGain();
  }
}

void setup() {
  platform::init();
  platform::diagStage(1);
  Serial.begin(115200);
#if !HAS_ADC_BUTTONS && defined(BTN_BOOT_PIN)
  pinMode(BTN_BOOT_PIN, INPUT_PULLUP);
#endif
#ifdef LED_PIN
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? LOW : HIGH);  // lit until setup() finishes
#endif
  midi_in::begin();
  platform::diagStage(2);
  display::begin();

  bootPrintf("[%s] boot\n", BOARD_NAME);
  uint32_t t0 = millis();
  bool ok = soundFont.load();
  platform::diagStage(3);
  if (!ok) {
    bootPrintf("SoundFont error: %s\n", soundFont.error());
    display::showError("SoundFont error", soundFont.error());
  } else {
    bootPrintf("SoundFont \"%s\": %lu presets, %lu regions, %lu samples (%lu ms)\n", soundFont.name(),
               (unsigned long)soundFont.presetCount, (unsigned long)soundFont.regionCount,
               (unsigned long)soundFont.sampleCount, (unsigned long)(millis() - t0));
    bootPrintf("Free heap %lu, PSRAM %lu\n", (unsigned long)platform::freeHeap(),
               (unsigned long)platform::freePsram());
    ok = synth::begin(&soundFont);
    platform::diagStage(4);
    if (!ok) {
      bootPrintf("Audio init failed\n");
      display::showError("Audio error", "I2S / DAC init failed");
    } else {
      applyGain();
      // Without PSRAM the built presets share the heap: keep some of it free
      if (!platform::freePsram()) {
        uint32_t heap = platform::freeHeap();
        soundFont.setCacheLimit(heap > SF2_HEAP_RESERVE ? heap - SF2_HEAP_RESERVE : 0);
      }
      running = true;
      display::showHeader(soundFont.name(), soundFont.presetCount, soundFont.regionCount);
      bootPrintf("Synth running\n");
    }
  }
  platform::diagStage(5);
#ifdef LED_PIN
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? HIGH : LOW);
#endif
}

void loop() {
  platform::diagStage(6);
  midi_in::poll();
  platform::diagStage(7);

  static uint32_t lastUi = 0;
  uint32_t now = millis();
  if (now - lastUi >= 50) {
    lastUi = now;
    handleButtons();
    static bool wasConnected = false;
    bool connected = (bool)Serial;
    if (connected && !wasConnected) printStatus();
    wasConnected = connected;
    platform::diagStage(8);
    if (running) {
      display::update(masterGain);
      updateLed();
    }
  }

  static uint32_t lastLog = 0;
  if (running && now - lastLog >= 1000) {
    lastLog = now;
    const synth::Stats& st = synth::stats();
    if (st.activeVoices) {
      Serial.printf("voices %u (peak %u) cpu %d%% (block max %d%%) late %lu xrun %lu clip %lu\n", st.activeVoices,
                    st.peakVoices, (int)(st.cpuLoad * 100), (int)(st.peakLoad * 100), (unsigned long)st.lateBlocks,
                    (unsigned long)st.underruns, (unsigned long)st.clipped);
      if (midi_in::bleConnected()) Serial.printf("ble jitter buffer %d ms\n", midi_in::bleMargin());
      synth::stats().peakLoad = 0;
    }
  }
  platform::diagStage(9);
  platform::idle();
  platform::diagStage(10);
}
