#include <Arduino.h>
#include "ArduinoJson.h"
#include "BLEScanner.h"

#ifdef BOARD_HAS_PSRAM
    #define RBMEM MALLOC_CAP_SPIRAM
#else
    #define RBMEM MALLOC_CAP_DEFAULT
#endif

static auto &bleScanner = BLEScanner::instance();

void setup() {
    Serial.begin(115200);
    bleScanner.begin(4096, 1000, 100, 99, 4096, 1, RBMEM);
}

void loop() {
    JsonDocument doc;
    char mac[16];
    if (bleScanner.process(doc, mac, sizeof(mac))) {
        serializeJsonPretty(doc, Serial);
        Serial.println();
    }
    delay(10);
}
