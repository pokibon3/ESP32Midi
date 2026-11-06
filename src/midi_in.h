// MIDI input: USB-MIDI device + optional UART (DIN) MIDI + optional BLE-MIDI (ESP32)
#pragma once

#ifndef MIDI_UART_RX_PIN
#define MIDI_UART_RX_PIN -1
#endif
// BLE-MIDI peripheral (-DMIDI_BLE_ENABLE=1, ESP32-S3 only)
#ifndef MIDI_BLE_NAME
#define MIDI_BLE_NAME "ESP32 MIDI Box"
#endif

namespace midi_in {

// Must be called before USB.begin()
void begin();
// Polls all inputs and forwards messages to the synth.
void poll();
// BLE-MIDI built in / a central is connected
bool bleEnabled();
bool bleConnected();

}  // namespace midi_in
