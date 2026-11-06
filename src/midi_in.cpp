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

// Each ring entry is (due time in ms, low 24 bits) << 8 | MIDI byte.
// Bytes leave the ring in order once their due time has come.
constexpr uint32_t kBleRing = 1024;  // power of two
uint32_t bleRing[kBleRing];
std::atomic<uint32_t> bleHead{0}, bleTail{0};
std::atomic<uint32_t> bleDropped{0};
std::atomic<bool> bleLinkUp{false};
std::atomic<int> bleMarginMs{0};
Parser bleParser;

// ---- BLE task only ----
bool bleInSysex = false;  // SysEx continues across packets
uint32_t bleLastDue = 0;  // keeps the byte order when due times go backwards

// Jitter buffer. The sender stamps every message with its 13-bit millisecond
// clock. lag = arrival - timestamp (plus an unknown clock offset); the packet
// with the smallest lag got through fastest, so a message is played at
// arrival + (minLag - lag) + margin, which restores the sender's spacing as
// long as its extra lag stays within the margin. The margin follows the lag
// spread of the last window: small for live playing, larger when packets bunch.
constexpr int kWindowMs = 2000;
constexpr int kMarginMin = 8, kMarginMax = 150, kMarginStart = 40;
struct {
  bool anchored = false;
  int32_t ref = 0;        // first lag: keeps lags away from the 13-bit wrap
  int32_t base = 0;       // smallest lag (relative to ref)
  int32_t margin = kMarginStart;
  int32_t winMin = 0, winMax = 0;
  bool winUsed = false;
  uint32_t winStart = 0;
} jb;

inline int32_t wrap13(int32_t v) {  // to -4096..4095
  v &= 0x1FFF;
  return v >= 0x1000 ? v - 0x2000 : v;
}

void jitterReset() {
  jb.anchored = false;
  jb.margin = kMarginStart;
  bleMarginMs = jb.margin;
}

// Returns the local due time (ms) of a message stamped `ts` that arrived at `now`
uint32_t jitterDue(uint16_t ts, uint32_t now) {
  int32_t lag = wrap13((int32_t)(now & 0x1FFF) - ts);
  if (!jb.anchored) {
    jb.anchored = true;
    jb.ref = lag;
    jb.base = 0;
    jb.winUsed = false;
    jb.winStart = now;
  }
  int32_t l = wrap13(lag - jb.ref);
  if (l > 1500 || l < -1500) {  // clock jump or a sender without timestamps: re-anchor
    jb.ref = lag;
    jb.base = 0;
    jb.winUsed = false;
    l = 0;
  }
  if (l < jb.base) jb.base = l;  // a faster packet: better estimate right away
  if (!jb.winUsed) {
    jb.winMin = jb.winMax = l;
    jb.winUsed = true;
  } else {
    jb.winMin = min(jb.winMin, l);
    jb.winMax = max(jb.winMax, l);
  }
  if (now - jb.winStart >= (uint32_t)kWindowMs) {
    // Clock drift can raise the minimum: follow it slowly
    if (jb.winMin > jb.base) jb.base += min<int32_t>(jb.winMin - jb.base, 2);
    // Margin: grow at once, shrink slowly (each change shifts the timing)
    int32_t want = constrain(jb.winMax - jb.winMin + 5, kMarginMin, kMarginMax);
    jb.margin = want > jb.margin ? want : max(want, jb.margin - 2);
    bleMarginMs = jb.margin;
    jb.winUsed = false;
    jb.winStart = now;
  }
  int32_t delay = jb.base - l + jb.margin;
  uint32_t due = now + (delay > 0 ? delay : 0);
  if ((int32_t)(due - bleLastDue) < 0) due = bleLastDue;
  bleLastDue = due;
  return due;
}

void bleEmit(uint8_t b, uint32_t due) {
  uint32_t head = bleHead.load(std::memory_order_relaxed);
  if (head - bleTail.load(std::memory_order_acquire) >= kBleRing) {
    bleDropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  bleRing[head & (kBleRing - 1)] = (due << 8) | b;
  bleHead.store(head + 1, std::memory_order_release);
}

// Packet: header (10hhhhhh = timestamp bits 12-7), then per message a timestamp
// byte (1lllllll = bits 6-0) and the message. Data bytes without a timestamp
// use running status and the previous timestamp; a smaller low part than the
// previous one means the low 7 bits wrapped. Inside SysEx only F7 (and
// realtime) is preceded by a timestamp; continuation packets start with data.
void bleDecode(const uint8_t* d, size_t len) {
  if (len < 2 || !(d[0] & 0x80)) return;
  uint32_t now = millis();
  uint16_t hi = d[0] & 0x3F;
  int lastLow = -1;
  uint32_t due = bleLastDue - now < 0x80000000u ? bleLastDue : now;  // SysEx continuation
  size_t i = 1;
  while (i < len) {
    uint8_t b = d[i++];
    if (b & 0x80) {  // timestamp: the next byte is a status byte or running-status data
      if (i >= len) break;
      uint8_t low = b & 0x7F;
      if (lastLow >= 0 && low < lastLow) hi = (hi + 1) & 0x3F;
      lastLow = low;
      due = jitterDue((hi << 7) | low, now);
      b = d[i++];
      if (bleInSysex && b < 0xF8) bleInSysex = false;  // F7 or a status byte ends SysEx
      if (b == 0xF0) bleInSysex = true;
    }
    bleEmit(b, due);
  }
}

class BleMidiCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override { bleDecode(c->getData(), c->getLength()); }
};

class BleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    bleLinkUp = true;
    jitterReset();
  }
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
  uint32_t now24 = millis() & 0xFFFFFF;
  while (tail != head) {
    uint32_t e = bleRing[tail & (kBleRing - 1)];
    int32_t wait = (int32_t)(((e >> 8) - now24) << 8) >> 8;  // signed 24-bit
    if (wait > 0) break;
    bleParser.feed(e & 0xFF);
    tail++;
  }
  bleTail.store(tail, std::memory_order_release);
#endif
  while (Serial.available()) serialParser.feed(Serial.read());
}

bool bleEnabled() { return HAVE_BLE_MIDI; }

int bleMargin() {
#if HAVE_BLE_MIDI
  return bleMarginMs.load(std::memory_order_relaxed);
#else
  return 0;
#endif
}

bool bleConnected() {
#if HAVE_BLE_MIDI
  return bleLinkUp.load(std::memory_order_relaxed);
#else
  return false;
#endif
}

}  // namespace midi_in
