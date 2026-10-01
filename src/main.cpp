// ESP32-S3-BOX-Lite SoundFont GM synthesizer
//
//  MIDI in : USB-MIDI (native USB port) / UART (MIDI_UART_RX_PIN)
//  Audio   : ES8156 DAC -> speaker
//  Buttons : PREV = volume down, NEXT = volume up, ENTER = GM reset
#include <Arduino.h>

#include "board.h"
#include "display.h"
#include "midi_in.h"
#include "sf2.h"
#include "synth.h"

static sf2::SoundFont soundFont;
static float masterGain = 0.35f;

enum Button { BTN_NONE, BTN_PREV, BTN_ENTER, BTN_NEXT };

// Front buttons are a resistor ladder on GPIO1 (values from esp-bsp)
static Button readButton() {
  int mv = analogReadMilliVolts(BTN_ADC_PIN);
  if (mv > 2310 && mv < 2510) return BTN_PREV;
  if (mv > 1880 && mv < 2080) return BTN_ENTER;
  if (mv > 720 && mv < 920) return BTN_NEXT;
  return BTN_NONE;
}

static void handleButtons() {
  static Button last = BTN_NONE;
  static uint32_t repeatAt = 0;
  Button b = readButton();
  uint32_t now = millis();
  bool pressed = b != last || (b != BTN_NONE && now >= repeatAt);
  if (b != last) repeatAt = now + 400;
  last = b;
  if (!pressed || b == BTN_NONE) return;
  if (now >= repeatAt) repeatAt = now + 80;

  switch (b) {
    case BTN_PREV: masterGain = max(0.0f, masterGain - 0.02f); break;
    case BTN_NEXT: masterGain = min(1.0f, masterGain + 0.02f); break;
    case BTN_ENTER: synth::reset(); break;
    default: break;
  }
  synth::setMasterGain(masterGain);
}

void setup() {
  Serial.begin(115200);
  midi_in::begin();
  display::begin();

  uint32_t t0 = millis();
  if (!soundFont.loadFromPartition("sf2")) {
    Serial.printf("SoundFont error: %s\n", soundFont.error());
    display::showError("SoundFont error", soundFont.error());
    return;
  }
  Serial.printf("SoundFont \"%s\": %lu presets, %lu regions, %lu samples (%lu ms)\n", soundFont.name(),
                (unsigned long)soundFont.presetCount, (unsigned long)soundFont.regionCount,
                (unsigned long)soundFont.sampleCount, (unsigned long)(millis() - t0));
  Serial.printf("Free heap %lu, PSRAM %lu\n", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());

  if (!synth::begin(&soundFont)) {
    display::showError("Audio error", "I2S / ES8156 init failed");
    return;
  }
  synth::setMasterGain(masterGain);
  display::showHeader(soundFont.name(), soundFont.presetCount, soundFont.regionCount);
}

void loop() {
  midi_in::poll();

  static uint32_t lastUi = 0;
  uint32_t now = millis();
  if (now - lastUi >= 50) {
    lastUi = now;
    handleButtons();
    if (soundFont.regions) display::update(masterGain);
  }

  static uint32_t lastLog = 0;
  if (soundFont.regions && now - lastLog >= 1000) {
    lastLog = now;
    const synth::Stats& st = synth::stats();
    if (st.activeVoices) {
      Serial.printf("voices %u (peak %u) cpu %d%% xrun %lu\n", st.activeVoices, st.peakVoices,
                    (int)(st.cpuLoad * 100), (unsigned long)st.underruns);
    }
  }
  delay(1);
}
