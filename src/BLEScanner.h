/// @file BLEScanner.h
/// @brief Singleton BLE advertisement scanner with built-in device decoders.
///
/// Scans for BLE advertisements in a dedicated FreeRTOS task and queues raw
/// data via a ring buffer. The caller drains the queue from the main loop by
/// calling process(), which deserializes, decodes (if a known device type is
/// recognized), and returns a populated JsonDocument plus the device MAC.
///
/// Supported device decoders:
///   - Ruuvi Tag (V5 format)
///   - Mopeka tank level sensors
///   - TPMS tire pressure sensors (0x0100 and 0x00AC variants)
///   - Otodata tank monitors
///   - Rotarex ELG level gauges
///   - BTHome v2 (with optional AES decryption)
///
/// Usage:
/// @code
///   auto &scanner = BLEScanner::instance();
///   scanner.setActiveScan(false);           // optional, before begin()
///   scanner.setBTHomeKey("431d39c1...");     // optional, 32-char hex
///   scanner.begin();                        // starts RTOS scan task
///
///   // in loop():
///   JsonDocument doc;
///   char mac[16];
///   if (scanner.process(doc, mac, sizeof(mac))) {
///       // publish or handle doc + mac
///   }
/// @endcode

#pragma once
#include <cstddef>
#include <cstdint>

#define ARDUINOJSON_USE_LONG_LONG 1
#include "ArduinoJson.h"

class BLEScanner {
public:
    static BLEScanner &instance();

    BLEScanner(const BLEScanner &) = delete;
    BLEScanner &operator=(const BLEScanner &) = delete;

    /// Opaque implementation detail (defined in BLEScanner.cpp)
    struct Impl;

    /// Initialize and start the BLE scanning RTOS task.
    /// Idempotent — second call is a no-op.
    void begin(size_t ringBufSize = 2048,
               uint32_t scanTimeMs = 15000,
               uint16_t scanInterval = 100,
               uint16_t scanWindow = 99,
               uint32_t taskStackSize = 4096,
               UBaseType_t taskPriority = 1,
               UBaseType_t ringBufCap = MALLOC_CAP_DEFAULT);

    /// Drain one item from the ring buffer, decode and populate doc.
    /// mac is filled with the colon-stripped uppercase MAC (e.g. "AABBCCDDEEFF").
    /// Returns true if an item was processed, false if queue was empty.
    bool process(JsonDocument &doc, char *mac, size_t macLen);

    /// Set BTHome decryption key (32-char hex string). Empty disables decryption.
    void setBTHomeKey(const char *hexKey);

    /// Enable or disable active scanning. Call before begin().
    void setActiveScan(bool active);

    /// Ring buffer and queue statistics.
    struct Stats {
        size_t hwmBytes;      ///< High water mark (peak bytes used)
        size_t totalBytes;    ///< Ring buffer total capacity
        uint8_t hwmPercent;   ///< High water mark as percentage of total
        uint32_t queueFull;   ///< Times send_complete failed (queue full)
        uint32_t acquireFail; ///< Times send_acquire failed (no space)
        uint32_t received;    ///< Total messages dequeued
        uint32_t decoded;     ///< Messages matched by a decoder
    };

    /// Return current ring buffer statistics.
    Stats stats() const;

private:
    BLEScanner() = default;

    Impl *_impl = nullptr;
    bool _started = false;

    bool deliver(JsonDocument &rawDoc, JsonDocument &outDoc);
};
