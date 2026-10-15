#include "platform.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

namespace platform {

void init() {}

void* allocFast(size_t size) { return heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
void* allocLarge(size_t size) { return ps_calloc(1, size); }
void* reallocLarge(void* p, size_t size) { return ps_realloc(p, size); }

const char* mapSoundFont(const uint8_t*& data, uint32_t& size) {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "sf2");
  if (!part) return "sf2 partition not found";

  uint8_t head[12];
  if (esp_partition_read(part, 0, head, sizeof(head)) != ESP_OK) return "partition read failed";
  if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "sfbk", 4) != 0) {
    return "no SoundFont in flash (run: pio run -t uploadsf2)";
  }
  uint32_t len;
  memcpy(&len, head + 4, 4);
  size = len + 8;
  if (size > part->size) return "SoundFont larger than partition";

  const void* ptr = nullptr;
  esp_partition_mmap_handle_t handle;
  if (esp_partition_mmap(part, 0, size, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK) {
    return "mmap failed";
  }
  data = (const uint8_t*)ptr;
  return nullptr;
}

namespace {

void (*s_block)();
void (*s_helperJob)();
TaskHandle_t s_audioTask, s_helperTask;

void helperTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_helperJob();
    xTaskNotifyGive(s_audioTask);
  }
}

void audioTask(void*) {
  for (;;) s_block();
}

}  // namespace

bool startAudio(void (*block)(), void (*helperJob)()) {
  s_block = block;
  s_helperJob = helperJob;
  // The helper must exist before the audio task starts notifying it
  if (xTaskCreatePinnedToCore(helperTask, "synth2", 6144, nullptr, configMAX_PRIORITIES - 2, &s_helperTask, 0) !=
      pdPASS) {
    return false;
  }
  return xTaskCreatePinnedToCore(audioTask, "synth", 6144, nullptr, configMAX_PRIORITIES - 2, &s_audioTask, 1) ==
         pdPASS;
}

void helperStart() { xTaskNotifyGive(s_helperTask); }
void helperWait() { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); }

void idle() { delay(1); }

void rebootToBootloader() {}
void diagStage(uint8_t) {}  // ESP32 uses the 1200bps touch of TinyUSB CDC

uint32_t freeHeap() { return ESP.getFreeHeap(); }
uint32_t freePsram() { return ESP.getFreePsram(); }

}  // namespace platform
