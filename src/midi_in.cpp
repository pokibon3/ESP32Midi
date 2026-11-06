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

#ifndef MIDI_BLE_ENABLE
#define MIDI_BLE_ENABLE 0
#endif
#if MIDI_BLE_ENABLE && defined(ARDUINO_ARCH_ESP32)
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <BLE2902.h>
#endif
#include <atomic>
#define HAVE_BLE_MIDI 1
#else
#define HAVE_BLE_MIDI 0
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
USBMIDI usbMidi("ESP32 MIDI Box");
Parser usbParser;
#endif
#if MIDI_UART_RX_PIN >= 0
Parser uartParser;
#endif
Parser serialParser;  // raw MIDI bytes over the USB serial port (for testing)

#if HAVE_BLE_MIDI
// ---- BLE-MIDI (Bluetooth LE MIDI 1.0 spec) ----
// The BLE host task decodes each packet into a plain MIDI byte stream and puts it
// in a single-producer ring; poll() (loop task) feeds it to the parser, because
// the synth event queue takes a single producer.
const char* const kMidiService = "03B80E5A-EDE8-4B33-A751-6CE34EC4C700";
const char* const kMidiChar = "7772E5DB-3868-4112-A1A9-F2669D106BF3";

constexpr uint32_t kBleRing = 2048;  // power of two
uint8_t bleRing[kBleRing];
std::atomic<uint32_t> bleHead{0}, bleTail{0};
std::atomic<uint32_t> bleDropped{0};
std::atomic<bool> bleLinkUp{false};
bool bleInSysex = false;  // BLE task only: SysEx continues across packets
Parser bleParser;

void bleEmit(uint8_t b) {
  uint32_t head = bleHead.load(std::memory_order_relaxed);
  if (head - bleTail.load(std::memory_order_acquire) >= kBleRing) {
    bleDropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  bleRing[head & (kBleRing - 1)] = b;
  bleHead.store(head + 1, std::memory_order_release);
}

// Packet: header (10tttttt), then [timestamp (1ttttttt)] status/data ...
// A timestamp precedes every status byte; data bytes without one use running
// status. Inside SysEx only F7 (and realtime) is preceded by a timestamp.
void bleDecode(const uint8_t* d, size_t len) {
  if (len < 2 || !(d[0] & 0x80)) return;
  size_t i = 1;
  while (i < len) {
    uint8_t b = d[i++];
    if (b & 0x80) {  // timestamp: the next byte is a status byte or running-status data
      if (i >= len) break;
      b = d[i++];
      if (bleInSysex && b < 0xF8) bleInSysex = false;  // F7 or a status byte ends SysEx
      if (b == 0xF0) bleInSysex = true;
    }
    bleEmit(b);
  }
}

class BleMidiCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override { bleDecode(c->getData(), c->getLength()); }
};

class BleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleLinkUp = true; }
  void onDisconnect(BLEServer*) override {
    bleLinkUp = false;
    bleInSysex = false;
  }
#if defined(CONFIG_NIMBLE_ENABLED)
  // Ask for a short connection interval (7.5-15ms) to keep latency low
  void onConnect(BLEServer* s, ble_gap_conn_desc* desc) override {
    s->requestConnParams(desc->conn_handle, 6, 12, 0, 200);
  }
#endif
};

void bleBegin() {
  BLEDevice::init(MIDI_BLE_NAME);
  BLEDevice::setMTU(128);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new BleServerCallbacks());
  server->advertiseOnDisconnect(true);
  BLEService* service = server->createService(kMidiService);
  BLECharacteristic* chr = service->createCharacteristic(
      kMidiChar, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE_NR |
                     BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)
  chr->addDescriptor(new BLE2902());
#endif
  chr->setCallbacks(new BleMidiCallbacks());
  service->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(kMidiService);
  adv->setScanResponse(true);  // the 128-bit UUID fills the advertisement; name goes in the scan response
  BLEDevice::startAdvertising();
}
#endif

// Number of valid MIDI bytes in a USB-MIDI event packet, by Code Index Number
const uint8_t kCinLength[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

}  // namespace

void begin() {
#if HAVE_BLE_MIDI
  bleBegin();
#endif
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
#if HAVE_BLE_MIDI
  uint32_t tail = bleTail.load(std::memory_order_relaxed);
  uint32_t head = bleHead.load(std::memory_order_acquire);
  while (tail != head) bleParser.feed(bleRing[tail++ & (kBleRing - 1)]);
  bleTail.store(tail, std::memory_order_release);
#endif
  while (Serial.available()) serialParser.feed(Serial.read());
}

bool bleEnabled() { return HAVE_BLE_MIDI; }

bool bleConnected() {
#if HAVE_BLE_MIDI
  return bleLinkUp.load(std::memory_order_relaxed);
#else
  return false;
#endif
}

}  // namespace midi_in
