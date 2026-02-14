#include "BLEScanner.h"

#include <Arduino.h>
#include <vector>
#include <string>

#include "freertos/ringbuf.h"
#include "ringbuffer.hpp"
#include "esp_timer.h"

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#include "BTHomeDecoder.h"

// ---------------------------------------------------------------------------
// Timing helper (replaces fmicro.h dependency)
// ---------------------------------------------------------------------------
static inline float fseconds() {
    return ((float)esp_timer_get_time()) * 1.0e-6f;
}

// ---------------------------------------------------------------------------
// Hex conversion helpers
// ---------------------------------------------------------------------------
static bool hexStringToVector(const String &hexStr, std::vector<uint8_t> &buffer) {
    size_t len = hexStr.length();
    if (len & 1)
        return false;

    buffer.clear();
    if (len == 0)
        return true;

    buffer.resize(len / 2);
    uint8_t *out = buffer.data();
    const char *in = hexStr.c_str();

    for (size_t i = 0; i < len; i += 2) {
        uint8_t val = 0;
        for (int j = 0; j < 2; j++) {
            uint8_t nibble;
            char c = in[i + j];
            if (c >= '0' && c <= '9')
                nibble = c - '0';
            else if (c >= 'A' && c <= 'F')
                nibble = c - 'A' + 10;
            else if (c >= 'a' && c <= 'f')
                nibble = c - 'a' + 10;
            else {
                buffer.clear();
                return false;
            }
            val = (val << 4) | nibble;
        }
        *out++ = val;
    }
    return true;
}

static void bytesToHexString(const uint8_t *data, size_t len, String &hexStr) {
    static const char HEX_CHARS[] = "0123456789ABCDEF";
    hexStr = "";
    if (len == 0)
        return;
    hexStr.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        hexStr += HEX_CHARS[data[i] >> 4];
        hexStr += HEX_CHARS[data[i] & 0x0F];
    }
}

// // Non-static — also used by BTHomeDecoder.cpp
bool stringToHexString(const String &str, String &hexStr) {
    bytesToHexString((const uint8_t *)str.c_str(), str.length(), hexStr);
    return true;
}

// ---------------------------------------------------------------------------
// BLEScanner::Impl — hidden state
// ---------------------------------------------------------------------------
struct BLEScanner::Impl {
    espidf::RingBuffer *queue = nullptr;
    BLEScan *pBLEScan = nullptr;
    BTHomeDecoder bthDecoder;
    const char *bthKey = "";

    uint32_t scanTimeMs = 15000;
    uint16_t scanInterval = 100;
    uint16_t scanWindow = 99;
    bool activeScan = false;

    uint32_t queueFull = 0;
    uint32_t acquireFail = 0;
    uint32_t received = 0;
    uint32_t decoded = 0;
};

// Singleton storage — the Impl pointer lives on the single instance.
static BLEScanner::Impl *s_impl = nullptr;

static bool decodeBTHome(JsonObject BLEdata, JsonDocument &json,
                         BTHomeDecoder &decoder, const char *key) {
    std::vector<uint8_t> sd;
    if (!hexStringToVector(BLEdata["sd"], sd))
        return false;

    BTHomeDecodeResult bthRes = decoder.parseBTHomeV2(
                                    std::string(sd.begin(), sd.end()),
                                    BLEdata["mac"],
                                    key);

    if (bthRes.isBTHome && bthRes.decryptionSucceeded) {
        JsonObject root = json.to<JsonObject>();
        root["bthome_version"] = bthRes.bthomeVersion;
        JsonArray measArr = root["measurements"].to<JsonArray>();

        for (auto &m : bthRes.measurements) {
            JsonObject obj = measArr.add<JsonObject>();
            obj["object_id"] = m.objectID;
            obj["name"]      = m.name;
            obj["value"]     = m.value;
            obj["unit"]      = m.unit;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// BLE scan callback — enqueues raw advertisement data as MsgPack
// ---------------------------------------------------------------------------
class ScanCallback : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (!s_impl || !s_impl->queue)
            return;

        JsonDocument doc;
        JsonObject BLEdata = doc.to<JsonObject>();

        String mac = advertisedDevice.getAddress().toString();
        mac.toUpperCase();
        BLEdata["mac"] = (char *)mac.c_str();
        BLEdata["rssi"] = (int)advertisedDevice.getRSSI();

        if (advertisedDevice.haveName())
            BLEdata["name"] = (char *)advertisedDevice.getName().c_str();

        if (advertisedDevice.haveManufacturerData()) {
            String hexData;
            stringToHexString(advertisedDevice.getManufacturerData(), hexData);
            BLEdata["mfd"] = hexData;
        }

        if (advertisedDevice.haveServiceUUID())
            BLEdata["svcuuid"] = (char *)advertisedDevice.getServiceUUID().toString().c_str();

        int sdCount = advertisedDevice.getServiceDataUUIDCount();
        if (sdCount > 0) {
            int idx = sdCount - 1;
            BLEdata["svduuid"] = (char *)advertisedDevice.getServiceDataUUID(idx).toString().c_str();
            String hexData;
            stringToHexString(advertisedDevice.getServiceData(idx), hexData);
            BLEdata["sd"] = hexData;
        }

        if (advertisedDevice.haveTXPower())
            BLEdata["txpwr"] = (int8_t)advertisedDevice.getTXPower();

        BLEdata["time"] = fseconds();

        void *ble_adv = nullptr;
        size_t total = measureMsgPack(BLEdata);
        if (s_impl->queue->send_acquire((void **)&ble_adv, total, 0) != pdTRUE) {
            s_impl->acquireFail++;
            return;
        }

        size_t n = serializeMsgPack(BLEdata, ble_adv, total);
        if (n != total) {
            log_e("serializeMsgPack: expected %u got %u", total, n);
        } else {
            if (s_impl->queue->send_complete(ble_adv) != pdTRUE) {
                s_impl->queueFull++;
            } else {
                s_impl->queue->update_high_watermark();
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Scan task (runs forever on its own RTOS task)
// ---------------------------------------------------------------------------
static void scanTask(void *param) {
    auto *impl = static_cast<BLEScanner::Impl *>(param);

    BLEDevice::init("");
    impl->pBLEScan = BLEDevice::getScan();
    impl->pBLEScan->setAdvertisedDeviceCallbacks(new ScanCallback(), true, true);
    impl->pBLEScan->setActiveScan(impl->activeScan);
    impl->pBLEScan->setInterval(impl->scanInterval);
    impl->pBLEScan->setWindow(impl->scanWindow);

    while (true) {
        BLEScanResults *foundDevices = impl->pBLEScan->start(impl->scanTimeMs / 1000, false);
        log_d("Devices found: %d", foundDevices->getCount());
        impl->pBLEScan->clearResults();
        delay(1);
    }
}

// ---------------------------------------------------------------------------
// BLEScanner public API
// ---------------------------------------------------------------------------
BLEScanner &BLEScanner::instance() {
    static BLEScanner s;
    return s;
}

void BLEScanner::setBTHomeKey(const char *hexKey) {
    if (!_impl) {
        _impl = new Impl();
        s_impl = _impl;
    }
    _impl->bthKey = hexKey ? hexKey : "";
}

void BLEScanner::setActiveScan(bool active) {
    if (!_impl) {
        _impl = new Impl();
        s_impl = _impl;
    }
    _impl->activeScan = active;
}

BLEScanner::Stats BLEScanner::stats() const {
    Stats s = {};
    if (!_impl || !_impl->queue)
        return s;
    s.hwmBytes    = _impl->queue->get_high_watermark();
    s.totalBytes  = _impl->queue->get_total_size();
    s.hwmPercent  = s.totalBytes > 0 ? (uint8_t)((s.hwmBytes * 100) / s.totalBytes) : 0;
    s.queueFull   = _impl->queueFull;
    s.acquireFail = _impl->acquireFail;
    s.received    = _impl->received;
    s.decoded     = _impl->decoded;
    return s;
}

void BLEScanner::begin(size_t ringBufSize,
                       uint32_t scanTimeMs,
                       uint16_t scanInterval,
                       uint16_t scanWindow,
                       uint32_t taskStackSize,
                       UBaseType_t taskPriority,
                       UBaseType_t ringBufCap) {
    if (_started)
        return;
    _started = true;

    if (!_impl) {
        _impl = new Impl();
        s_impl = _impl;
    }

    _impl->scanTimeMs = scanTimeMs;
    _impl->scanInterval = scanInterval;
    _impl->scanWindow = scanWindow;

    _impl->queue = new espidf::RingBuffer();
    _impl->queue->create(ringBufSize, RINGBUF_TYPE_NOSPLIT, ringBufCap);

    xTaskCreate(scanTask, "ble_scan", taskStackSize, _impl, taskPriority, nullptr);
}

bool BLEScanner::deliver(JsonDocument &rawDoc, JsonDocument &outDoc) {
    bool decoded = false;
    std::vector<uint8_t> mfd;

    if (rawDoc["svduuid"].is<const char*>() &&
            String(rawDoc["svduuid"]).indexOf("fcd2") != -1) {
        decoded = decodeBTHome(rawDoc.as<JsonObject>(), outDoc,
                               _impl->bthDecoder, _impl->bthKey);
    }
    return decoded;
}

bool BLEScanner::process(JsonDocument &doc, char *mac, size_t macLen) {
    if (!_impl || !_impl->queue)
        return false;

    size_t size = 0;
    void *buffer = _impl->queue->receive(&size, 0);
    if (buffer == nullptr)
        return false;

    JsonDocument rawDoc;
    deserializeMsgPack(rawDoc, buffer, size);
    _impl->queue->return_item(buffer);
    _impl->received++;

    // Decode
    JsonDocument decodedDoc;
    bool decoded = deliver(rawDoc, decodedDoc);
    if (decoded)
        _impl->decoded++;

    // Pick output document
    JsonDocument &outDoc = decoded ? decodedDoc : rawDoc;

    // Merge common metadata into decoded results
    if (decoded) {
        outDoc["mac"]  = rawDoc["mac"];
        outDoc["time"] = rawDoc["time"];
        outDoc["rssi"] = rawDoc["rssi"];
        if (rawDoc["name"].is<const char*>())
            outDoc["name"] = rawDoc["name"];
        if (rawDoc["txpwr"].is<JsonVariant>())
            outDoc["txpwr"] = rawDoc["txpwr"];

        // Extract MAC (strip colons)
        String macStr = rawDoc["mac"].as<String>();
        macStr.replace(":", "");
        size_t copyLen = macStr.length();
        if (copyLen >= macLen)
            copyLen = macLen - 1;
        memcpy(mac, macStr.c_str(), copyLen);
        mac[copyLen] = '\0';

        // Move result into caller's doc
        doc.set(outDoc);
        return true;
    }
    return false;
}
