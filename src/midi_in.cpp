#include "midi_in.h"

#include <Arduino.h>
#include <string.h>
#ifndef MIDI_USB_ENABLE
#define MIDI_USB_ENABLE 1
#endif
#if !MIDI_USB_ENABLE
#define HAVE_USB_MIDI 0
#elif defined(ARDUINO_ARCH_RP2040)
// Adafruit TinyUSB stack (-DUSE_TINYUSB): with the core's own USB stack, adding
// the MIDIUSB interface after boot left both CDC and MIDI without data.
#include <Adafruit_TinyUSB.h>
#define HAVE_USB_MIDI 1
#elif !ARDUINO_USB_MODE
#include <USB.h>
#include <USBMIDI.h>
#define HAVE_USB_MIDI 1
#endif

#include "platform.h"
#include "synth.h"

namespace midi_in {

namespace {

// F0 7D 'B' 'O' 'O' 'T' F7 (non-commercial ID): reboot into the bootloader.
// Used by `pio run -t uploadsf2` when the 1200bps touch does not work.
const uint8_t kBootSysex[] = {0xF0, 0x7D, 'B', 'O', 'O', 'T', 0xF7};

void handleSysex(const uint8_t* d, size_t len) {
  if (len == sizeof(kBootSysex) && memcmp(d, kBootSysex, len) == 0) {
    platform::rebootToBootloader();
    return;
  }
  synth::sysex(d, len);
}

// Byte stream parser with running status and SysEx support
class Parser {
 public:
  void feed(uint8_t b) {
    if (b >= 0xF8) return;  // realtime: ignored
    if (b == 0xF0) {
      sysexLen_ = 0;
      inSysex_ = true;
      pushSysex(b);
      return;
    }
    if (inSysex_) {
      if (b < 0x80 || b == 0xF7) {
        pushSysex(b);
        if (b == 0xF7) {
          inSysex_ = false;
          if (sysexLen_ <= sizeof(sysex_)) handleSysex(sysex_, sysexLen_);
        }
        return;
      }
      inSysex_ = false;  // aborted by a status byte
    }
    if (b >= 0x80) {
      if (b >= 0xF0) {  // system common: cancels running status
        status_ = 0;
        return;
      }
      status_ = b;
      count_ = 0;
      return;
    }
    if (!status_) return;
    data_[count_++] = b;
    uint8_t type = status_ & 0xF0;
    uint8_t need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
    if (count_ >= need) {
      synth::midiMessage(status_, data_[0], need == 2 ? data_[1] : 0);
      count_ = 0;
    }
  }

 private:
  void pushSysex(uint8_t b) {
    if (sysexLen_ < sizeof(sysex_)) sysex_[sysexLen_] = b;
    sysexLen_++;
  }

  uint8_t status_ = 0;
  uint8_t data_[2];
  uint8_t count_ = 0;
  bool inSysex_ = false;
  uint8_t sysex_[32];
  size_t sysexLen_ = 0;
};

#if HAVE_USB_MIDI && defined(ARDUINO_ARCH_RP2040)
Adafruit_USBD_MIDI usbMidi;
Parser usbParser;
#elif HAVE_USB_MIDI
USBMIDI usbMidi("ESP32-S3 GM Synth");
Parser usbParser;
#endif
#if MIDI_UART_RX_PIN >= 0
Parser uartParser;
#endif
Parser serialParser;  // raw MIDI bytes over the USB serial port (for testing)

// Number of valid MIDI bytes in a USB-MIDI event packet, by Code Index Number
const uint8_t kCinLength[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

}  // namespace

void begin() {
#if HAVE_USB_MIDI && defined(ARDUINO_ARCH_RP2040)
  usbMidi.setStringDescriptor("RP2350 GM Synth");
#endif
#if HAVE_USB_MIDI
  usbMidi.begin();
#endif
#if HAVE_USB_MIDI && defined(ARDUINO_ARCH_RP2040)
  // The MIDI interface only appears after re-enumeration
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
#endif
#if MIDI_UART_RX_PIN >= 0
#if defined(ARDUINO_ARCH_RP2040)
  Serial1.setRX(MIDI_UART_RX_PIN);
  Serial1.begin(31250);
#else
  Serial1.begin(31250, SERIAL_8N1, MIDI_UART_RX_PIN, -1);
#endif
#endif
}

void poll() {
#if HAVE_USB_MIDI && defined(ARDUINO_ARCH_RP2040)
#ifdef TINYUSB_NEED_POLLING_TASK
  TinyUSBDevice.task();
#endif
  uint8_t pkt[4];
  while (usbMidi.readPacket(pkt)) {
    uint8_t n = kCinLength[pkt[0] & 0x0F];
    for (uint8_t i = 0; i < n; i++) usbParser.feed(pkt[1 + i]);
  }
#elif HAVE_USB_MIDI
  midiEventPacket_t pkt;
  while (usbMidi.readPacket(&pkt)) {
    uint8_t n = kCinLength[pkt.header & 0x0F];
    const uint8_t bytes[3] = {pkt.byte1, pkt.byte2, pkt.byte3};
    for (uint8_t i = 0; i < n; i++) usbParser.feed(bytes[i]);
  }
#endif
#if MIDI_UART_RX_PIN >= 0
  while (Serial1.available()) uartParser.feed(Serial1.read());
#endif
  while (Serial.available()) serialParser.feed(Serial.read());
}

}  // namespace midi_in
