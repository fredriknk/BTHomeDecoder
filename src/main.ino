#include <Arduino.h>
#include "NimBLEDevice.h"//https://github.com/h2zero/NimBLE-Arduino.git#1.3.1
#include "ArduinoJson.h"//ArduinoJson@6.18.3
#include "BTHomeDecoder.h"

static BTHomeDecoder bthDecoder;

// We'll store the latest reading in a global struct
// or you can store them in a queue if you expect many devices
struct BTHomeReading {
  bool valid = false;
  String mac;
  String devName;
  int rssi;
  StaticJsonDocument<1024> doc;
};

static BTHomeReading latestReading;

// Create a global flag to indicate new reading is ready
static volatile bool newReadingReady = false;

class MyAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {

  std::string convertServiceData(std::string deviceServiceData) {
    int len = (int)deviceServiceData.length();
    char buf[2*len + 1];
    for (int i=0; i<len; i++) {
      sprintf(buf + 2*i, "%02x", (uint8_t)deviceServiceData[i]);
    }
    buf[2*len] = 0;
    return std::string(buf);
  }

  void onResult(BLEAdvertisedDevice* advertisedDevice) override {
    // If no service data, skip
    if (!advertisedDevice->haveServiceData()) return;

    int count = advertisedDevice->getServiceDataCount();
    for (int j=0; j<count; j++) {
      // Check if FCD2 => BTHome
      std::string uuid = advertisedDevice->getServiceDataUUID(j).toString();
      if (uuid.find("fcd2") == std::string::npos) {
        continue;
      }

      // Convert to vector
      std::string rawData = advertisedDevice->getServiceData(j);
      std::vector<uint8_t> dataVec(rawData.begin(), rawData.end());

      // Decode
      BTHomeDecodeResult bthRes = bthDecoder.parseBTHomeV2(
          dataVec,
          advertisedDevice->getAddress().toString().c_str(),
          "" // no encryption
      );

      if (bthRes.isBTHome && bthRes.decryptionSucceeded) {
        // -----------------------------------------------------------
        // 1) Copy everything into our global BTHomeReading struct
        //    so we are not depending on NimBLE's memory after return
        // -----------------------------------------------------------
        latestReading.valid = true;

        // Copy the MAC address into a String
        String mac = advertisedDevice->getAddress().toString().c_str();
        mac.toUpperCase();
        latestReading.mac = mac;

        // Copy the name
        // WARNING: getName() memory might go away after callback,
        // so store it in a local String now
        String devName;
        if (advertisedDevice->haveName()) {
          devName = advertisedDevice->getName().c_str();
        } else {
          devName = "NoName";
        }
        latestReading.devName = devName;

        // Copy other fields
        latestReading.rssi = (advertisedDevice->haveRSSI() ? advertisedDevice->getRSSI() : 0);

        // 2) Build the JSON doc in the global struct
        //    (No more printing here, do it in loop())
        latestReading.doc.clear();
        JsonObject root = latestReading.doc.to<JsonObject>();

        root["id"] = latestReading.mac;
        root["name"] = latestReading.devName;
        root["bthome_version"] = bthRes.bthomeVersion;
        root["bthome_encrypted"] = bthRes.isEncrypted;
        root["rssi"] = latestReading.rssi;

        JsonArray measArr = root.createNestedArray("measurements");
        for (auto &m : bthRes.measurements) {
          JsonObject obj = measArr.createNestedObject();
          obj["object_id"] = m.objectID;
          obj["name"]      = m.name;
          obj["value"]     = m.value;
        }

        // Set the flag
        newReadingReady = true;
      }
    }
  }
};

void setup() {
  Serial.begin(115200);
  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), false);
  scan->setActiveScan(true);
  scan->setInterval(97);
  scan->setWindow(37);
  scan->setMaxResults(0);
}

void loop() {
  // Start scanning if not already
  NimBLEScan* scan = NimBLEDevice::getScan();
  if(!scan->isScanning()) {
    scan->start(0, nullptr, false);
  }

  // Check if new reading is ready
  if(newReadingReady) {
    // Turn off scanning while we handle print, to avoid interruption
    scan->stop();

    Serial.println("===== BTHome Advertisement Decoded =====");
    serializeJsonPretty(latestReading.doc, Serial);
    Serial.println("\n========================================\n");

    // Clear the flag
    newReadingReady = false;
    latestReading.valid = false;

    // Resume scanning
    scan->start(0, nullptr, false);
  }

  delay(500);
}
