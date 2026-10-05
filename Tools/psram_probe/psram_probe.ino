#include <Arduino.h>
#include "esp32-hal-psram.h"
#include "esp_heap_caps.h"

void printProbe() {
  Serial.printf("PSRAM_PROBE found=%u size=%u free=%u largest=%u\n",
                (unsigned)psramFound(),
                (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreePsram(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  printProbe();
}

void loop() {
  delay(1000);
  printProbe();
}
