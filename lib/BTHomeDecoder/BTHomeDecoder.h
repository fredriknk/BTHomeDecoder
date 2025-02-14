#pragma once

#include <Arduino.h>
#include <vector>
#include <string>
#include "mbedtls/ccm.h"

// ------------------------------------------------------------
//  Structs
// ------------------------------------------------------------
struct BTHomeMeasurement {
    uint8_t objectID;
    float value;
    String name;
    bool isValid;
};

struct BTHomeDecodeResult {
    bool isBTHome;
    bool isBTHomeV2;
    uint8_t bthomeVersion;
    bool isEncrypted;
    bool decryptionSucceeded;
    bool isTriggerBased;
    std::vector<BTHomeMeasurement> measurements;
};

// ------------------------------------------------------------
//  BTHomeDecoder Class
// ------------------------------------------------------------
class BTHomeDecoder {
public:
    BTHomeDecoder() {}
    ~BTHomeDecoder() {}

    BTHomeDecodeResult parseBTHomeV2(
        const std::vector<uint8_t>& serviceData,
        const std::string& macString,
        const std::string& keyHex
    );

private:
    // Helper methods
    bool   macStringToBytes(const std::string &macStr, uint8_t macOut[6]);
    bool   decryptAESCCM(const uint8_t* ciphertext, size_t ciphertextLen,
                         const uint8_t* macBytes, uint8_t advInfo,
                         const uint8_t* key, const uint8_t* counter,
                         uint8_t* plaintextOut, size_t &plaintextLenOut);

    int    getObjectDataLength(uint8_t objID);
    float  getObjectFactor(uint8_t objID);
    String getObjectName(uint8_t objID);

    float  parseSignedLittle(const uint8_t* data, size_t len, float factor);
    float  parseUnsignedLittle(const uint8_t* data, size_t len, float factor);
};

