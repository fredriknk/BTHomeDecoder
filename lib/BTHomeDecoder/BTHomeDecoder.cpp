#include "BTHomeDecoder.h"

// ----------------------------
//  parseBTHomeV2
// ----------------------------
BTHomeDecodeResult BTHomeDecoder::parseBTHomeV2(
    const std::vector<uint8_t>& serviceData,
    const std::string& macString,
    const std::string& keyHex
)
{
    BTHomeDecodeResult result;
    result.isBTHome = false;
    result.isBTHomeV2 = false;
    result.bthomeVersion = 0;
    result.isEncrypted = false;
    result.decryptionSucceeded = false;
    result.isTriggerBased = false;

    // Must have at least 1 byte to read the adv_info
    if (serviceData.size() < 1) {
        return result;
    }

    uint8_t advInfo = serviceData[0];
    bool encryptionFlag = (advInfo & 0x01) != 0;
    bool hasMac         = (advInfo & 0x02) != 0;
    bool triggerBased   = (advInfo & 0x04) != 0;
    uint8_t version     = (advInfo >> 5) & 0x07;

    result.isBTHome     = true;  // because presumably the 0xFCD2 service UUID was matched externally
    result.bthomeVersion = version;
    result.isEncrypted   = encryptionFlag;
    result.isTriggerBased = triggerBased;
    if (version == 2) {
        result.isBTHomeV2 = true;
    }

    // Skip over advInfo + MAC if present
    size_t index = 1;
    if (hasMac) {
        if (serviceData.size() < 7) {
            return result; // not enough data
        }
        index += 6; // skip the "reversed MAC"
    }

    if (index >= serviceData.size()) {
        return result;
    }

    // The remainder is payload
    std::vector<uint8_t> payload(serviceData.begin() + index, serviceData.end());

    // If encrypted, decrypt
    if (encryptionFlag) {
        if (keyHex.size() != 32) {
            // invalid key
            return result;
        }

        uint8_t key[16];
        for (int i = 0; i < 16; i++) {
            String sub = String(keyHex.c_str() + i*2, 2);
            key[i] = (uint8_t) strtol(sub.c_str(), nullptr, 16);
        }

        // Convert MAC string to byte array (normal order)
        uint8_t macBytes[6];
        if (!macStringToBytes(macString, macBytes)) {
            memset(macBytes, 0, 6); // fallback
        }

        // BTHome v2: last 8 bytes in payload => [counter(4) + mic(4)]
        if (payload.size() < 8) {
            return result;
        }

        size_t totalLen = payload.size();
        size_t offsetCounter = totalLen - 8;
        // size_t offsetMic     = totalLen - 4;

        uint8_t counter[4];
        memcpy(counter, &payload[offsetCounter], 4);

        // Decrypt in-place
        std::vector<uint8_t> decrypted(totalLen);
        size_t outLen = 0;
        bool ok = decryptAESCCM(payload.data(), totalLen,
                                macBytes, advInfo,
                                key, counter,
                                decrypted.data(), outLen);

        if (!ok) {
            return result; // decryption failed
        }

        result.decryptionSucceeded = true;
        // Replace payload with the plaintext
        payload.resize(outLen);
        memcpy(payload.data(), decrypted.data(), outLen);
    } else {
        result.decryptionSucceeded = true;
    }

    // Parse objects
    size_t idx = 0;
    while (idx < payload.size()) {
        //Serial.printf("DEBUG: idx=%d, payload.size()=%d\n", idx, payload.size());
        if (idx + 1 > payload.size()) break;    
        uint8_t objID = payload[idx];
        idx++;
    
        //Serial.printf("DEBUG: Found objectID=0x%02X\n", objID);
    
        int dataLen = getObjectDataLength(objID);
        //Serial.printf("DEBUG: dataLen=%d\n", dataLen);
        if (dataLen < 0) {
            //Serial.println("DEBUG: Unknown objectID => stopping parse");
            break;
        }
        if (idx + dataLen > payload.size()) {
            //Serial.println("DEBUG: Not enough bytes => stopping parse");
            break;
        }
    
        float factor = getObjectFactor(objID);
        bool isSigned = false; 
        // if (objID == 0x02) isSigned = true; // example
        float val = 0.0f;
        if (isSigned) {
            val = parseSignedLittle(&payload[idx], dataLen, factor);
        } else {
            val = parseUnsignedLittle(&payload[idx], dataLen, factor);
        }
    
        //Serial.printf("DEBUG: objID=0x%02X => val=%.2f, factor=%.3f\n", objID, val, factor);
    
        BTHomeMeasurement meas;
        meas.objectID = objID;
        meas.value = val;
        meas.name = getObjectName(objID);
        meas.isValid = true;
    
        result.measurements.push_back(meas);
    
        idx += dataLen;
    }

    return result;
}

// ----------------------------
//  Helper Methods
// ----------------------------
bool BTHomeDecoder::macStringToBytes(const std::string &macStr, uint8_t macOut[6]) {
    std::string cleaned;
    for (char c : macStr) {
        if (isxdigit(c)) cleaned.push_back(c);
    }
    if (cleaned.size() != 12) return false;

    for (int i = 0; i < 6; i++) {
        std::string byteStr = cleaned.substr(i*2, 2);
        macOut[i] = (uint8_t) strtol(byteStr.c_str(), nullptr, 16);
    }
    return true;
}

bool BTHomeDecoder::decryptAESCCM(
    const uint8_t* ciphertext, size_t ciphertextLen,
    const uint8_t* macBytes, uint8_t advInfo,
    const uint8_t* key, const uint8_t* counter,
    uint8_t* plaintextOut, size_t &plaintextLenOut
) {
    // BTHome Nonce => mac(6) + 0xD2 0xFC + advInfo(1) + counter(4) = 13
    uint8_t nonce[13];
    memcpy(nonce, macBytes, 6);
    nonce[6] = 0xD2;
    nonce[7] = 0xFC;
    nonce[8] = advInfo;
    memcpy(&nonce[9], counter, 4);

    // last 4 bytes of ciphertext => MIC
    if (ciphertextLen < 4) return false;
    size_t micLen = 4;
    size_t rawCipherLen = ciphertextLen - micLen;

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int ret = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret != 0) {
        mbedtls_ccm_free(&ctx);
        return false;
    }

    ret = mbedtls_ccm_auth_decrypt(
        &ctx,
        rawCipherLen,
        nonce, sizeof(nonce),
        nullptr, 0, // no AAD
        ciphertext, plaintextOut,
        (uint8_t*)(ciphertext + rawCipherLen),
        micLen
    );
    mbedtls_ccm_free(&ctx);

    if (ret != 0) return false;
    plaintextLenOut = rawCipherLen;
    return true;
}

int BTHomeDecoder::getObjectDataLength(uint8_t objID) {
    switch (objID) {
        case 0x02: case 0x03: case 0x08: case 0x12: case 0x13: case 0x14:
        case 0x40: case 0x41: case 0x43: case 0x44: case 0x45: case 0x46:
        case 0x47: case 0x48: case 0x51: case 0x52: case 0x56: case 0x57:
        case 0x58: case 0x5E: case 0x5F:
            return 2;
        case 0x04: case 0x0A: case 0x0B: case 0x42: case 0x49: case 0x4B:
            return 3;
        case 0x3E: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50:
        case 0x5B: case 0x5C:
            return 4;
        case 0x3C:
            return 2;
        case 0x2E: // humidity (1 byte)
            return 1;
        case 0x05: // illuminance (3 bytes)
            return 3;
        case 0x0C: // battery voltage (2 bytes)
            return 2;
        case 0x2F: // soil moisture (1 byte)
            return 1;
        case 0x01: // battery percentage (1 byte)
            return 1;
        default:
            return -1;
    }
}

float BTHomeDecoder::getObjectFactor(uint8_t objID) {
    switch (objID) {
        case 0x02: case 0x03: case 0x08: case 0x14: case 0x5E:
            return 0.01f;
        case 0x04: case 0x0B:
            return 0.01f;
        case 0x0A:
            return 0.001f;
        case 0x3F:
            return 0.1f;
        case 0x42:
            return 0.001f;
        case 0x43: case 0x51: case 0x52:
            return 0.001f;
        case 0x44:
            return 0.01f;
        case 0x45:
            return 0.1f;
        case 0x46:
            return 0.1f;
        case 0x47:
            return 0.1f;
        case 0x4A:
            return 0.1f;
        case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            return 0.001f;
        case 0x5C:
            return 0.01f;

        case 0x2E: 
        // If you stored humidity as a single byte (0-100),
        // factor = 1.0 => final value = raw (like 55 => 55%)
        return 1.0f; 

        case 0x05:
            // Illuminance is 3 bytes * factor 0.01 => e.g. raw = 12345 => 123.45 lux
            // or pick a different factor if your device uses another scale.
            return 0.01f; 

        case 0x0C:
            // Battery voltage is 2 bytes in millivolts => factor = 0.001 => 3241 => 3.241 V
            return 0.001f; 

        case 0x2F:
            // Soil moisture is 1 byte, 0..100 => factor = 1 => raw 47 => 47%
            return 1.0f;

        case 0x01:
            // Battery % is 1 byte, 0..100 => factor = 1 => raw 98 => 98%
            return 1.0f;
        default:
            return 1.0f;
    }
}

String BTHomeDecoder::getObjectName(uint8_t objID) {
    switch (objID) {
        case 0x02: return "temperature";
        case 0x03: return "humidity";
        case 0x04: return "pressure";
        case 0x0A: return "energy";
        case 0x0B: return "power";
        case 0x12: return "CO2";
        case 0x13: return "VOC";
        case 0x14: return "moisture";
        case 0x3A: return "button";
        case 0x3C: return "dimmer";
        case 0x40: return "distance_mm";
        case 0x41: return "distance_m";
        case 0x42: return "duration_sec";
        case 0x43: return "current_A";
        case 0x44: return "speed_mps";
        case 0x45: return "temperature_0.1C";
        case 0x46: return "UV_index";
        case 0x47: return "volume_liters";
        case 0x48: return "volume_milliliters";
        case 0x49: return "flow_rate";
        case 0x4A: return "voltage_V";
        case 0x4B: return "gas_m3";
        case 0x50: return "timestamp";
        case 0x2E: return "humidity";
        case 0x05: return "illuminance";
        case 0x0C: return "battery_voltage";
        case 0x2F: return "soil_moisture";
        case 0x01: return "battery_percent";
        default: return "unknown";
    }
}

float BTHomeDecoder::parseSignedLittle(const uint8_t* data, size_t len, float factor) {
    if (len == 1) {
        int8_t raw = (int8_t)data[0];
        return raw * factor;
    } else if (len == 2) {
        int16_t raw = (int16_t)((data[1] << 8) | data[0]);
        return raw * factor;
    } else if (len == 3) {
        int32_t raw = (int32_t)((data[2] << 16) | (data[1] << 8) | data[0]);
        return raw * factor;
    } else if (len == 4) {
        int32_t raw = (int32_t)((data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0]);
        return raw * factor;
    }
    return 0.0f;
}

float BTHomeDecoder::parseUnsignedLittle(const uint8_t* data, size_t len, float factor) {
    if (len == 1) {
        uint8_t raw = data[0];
        return raw * factor;
    } else if (len == 2) {
        uint16_t raw = (uint16_t)((data[1] << 8) | data[0]);
        return raw * factor;
    } else if (len == 3) {
        uint32_t raw = (uint32_t)((data[2] << 16) | (data[1] << 8) | data[0]);
        return raw * factor;
    } else if (len == 4) {
        uint32_t raw = (uint32_t)((data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0]);
        return raw * factor;
    }
    return 0.0f;
}