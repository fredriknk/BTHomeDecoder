/**
 * BTHomeScan — example sketch for the BTHomeDecoder library.
 *
 * Scans for BLE advertisements on an ESP32, decodes any BTHome v2
 * payloads, and prints them as pretty-printed JSON on Serial.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLEScanner.h>

#ifdef BOARD_HAS_PSRAM
    #define RBMEM MALLOC_CAP_SPIRAM
#else
    #define RBMEM MALLOC_CAP_DEFAULT
#endif

static auto &bleScanner = BLEScanner::instance();

void setup() {
    Serial.begin(115200);

    // Optional: set a BTHome decryption key (32-char hex string)
    // bleScanner.setBTHomeKey("00112233445566778899aabbccddeeff");

    bleScanner.begin(4096,   // ring buffer size
                     1000,   // scan time (ms)
                     100,    // scan interval
                     99,     // scan window
                     4096,   // task stack size
                     1,      // task priority
                     RBMEM); // ring buffer memory capability
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
