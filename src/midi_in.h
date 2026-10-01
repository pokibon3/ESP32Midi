// MIDI input: USB-MIDI device + optional UART (DIN) MIDI
#pragma once

#ifndef MIDI_UART_RX_PIN
#define MIDI_UART_RX_PIN -1
#endif

namespace midi_in {

// Must be called before USB.begin()
void begin();
// Polls all inputs and forwards messages to the synth.
void poll();

}  // namespace midi_in
