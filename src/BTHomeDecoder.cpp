#include "BTHomeDecoder.h"

// Declaration for stringToHexString from blescan.cpp
bool stringToHexString(const String &str, String &hexStr);

// ----------------------------
//  parseBTHomeV2
// ----------------------------
BTHomeDecodeResult BTHomeDecoder::parseBTHomeV2(
    // const std::vector<uint8_t> &serviceData,
    const std::string &serviceData,
    const std::string &macString,
    const std::string &keyHex) {
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
    bool hasMac = (advInfo & 0x02) != 0;
    bool triggerBased = (advInfo & 0x04) != 0;
    uint8_t version = (advInfo >> 5) & 0x07;

    result.isBTHome = true; // because presumably the 0xFCD2 service UUID was matched externally
    result.bthomeVersion = version;
    result.isEncrypted = encryptionFlag;
    result.isTriggerBased = triggerBased;
    if (version == 2) {
        result.isBTHomeV2 = true;
    }

    // Skip over advInfo + MAC if present
    size_t index = 1;
    // size_t index = 0;
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

    String tempStr(reinterpret_cast<const char*>(payload.data()), payload.size());
    String hexStr;
    stringToHexString(tempStr, hexStr);
    std::string hp = hexStr.c_str();
    log_d("--DEBUG: mac=%s payload=%s", macString.c_str(), hp.c_str());

    // If encrypted, decrypt
    if (encryptionFlag) {
        if (keyHex.size() != 32) {
            // invalid key
            return result;
        }

        uint8_t key[16];
        for (int i = 0; i < 16; i++) {
            String sub = String(keyHex.c_str() + i * 2, 2);
            key[i] = (uint8_t)strtol(sub.c_str(), nullptr, 16);
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
        int dataLen;

        log_v("DEBUG: idx=%d, payload.size()=%d", idx, payload.size());
        if (idx + 1 > payload.size())
            break;
        uint8_t objID = payload[idx];
        idx++; // skip over objId
        if (hasLengthByte(objID)) {
            // skip over length byte
            dataLen = payload[idx];
            idx++;
        } else {
            dataLen = getObjectDataLength(objID);
        }
        log_v("DEBUG: objectID=0x%02X dataLen=%d", objID, dataLen);
        if (dataLen < 0) {
            log_d("DEBUG: Unknown objectID => stopping parse");
            break;
        }
        if (idx + dataLen > payload.size()) {
            log_d("DEBUG: Not enough bytes => stopping parse idx=%d dataLen=%d pl=%d", idx, dataLen, payload.size());
            break;
        }

        float factor = getObjectFactor(objID);
        bool isSigned = getObjectSignedNess(objID);
        // if (objID == 0x02) isSigned = true; // example
        float val = 0.0f;
        if (isSigned) {
            val = parseSignedLittle(&payload[idx], dataLen, factor);
        } else {
            val = parseUnsignedLittle(&payload[idx], dataLen, factor);
        }

        log_d("DEBUG: objID=0x%02X => val=%.2f, factor=%.3f", objID, val, factor);

        BTHomeMeasurement meas;
        meas.objectID = objID;
        meas.value = val;
        meas.name = getObjectName(objID);
        meas.unit = getObjectUnit(objID);
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
        if (isxdigit(c))
            cleaned.push_back(c);
    }
    if (cleaned.size() != 12)
        return false;

    for (int i = 0; i < 6; i++) {
        std::string byteStr = cleaned.substr(i * 2, 2);
        macOut[i] = (uint8_t)strtol(byteStr.c_str(), nullptr, 16);
    }
    return true;
}

bool BTHomeDecoder::decryptAESCCM(
    const uint8_t *ciphertext, size_t ciphertextLen,
    const uint8_t *macBytes, uint8_t advInfo,
    const uint8_t *key, const uint8_t *counter,
    uint8_t *plaintextOut, size_t &plaintextLenOut) {
    // BTHome Nonce => mac(6) + 0xD2 0xFC + advInfo(1) + counter(4) = 13
    uint8_t nonce[13];
    memcpy(nonce, macBytes, 6);
    nonce[6] = 0xD2;
    nonce[7] = 0xFC;
    nonce[8] = advInfo;
    memcpy(&nonce[9], counter, 4);

    // last 4 bytes of ciphertext => MIC
    if (ciphertextLen < 4)
        return false;
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
              (uint8_t *)(ciphertext + rawCipherLen),
              micLen);
    mbedtls_ccm_free(&ctx);

    if (ret != 0)
        return false;
    plaintextLenOut = rawCipherLen;
    return true;
}

bool BTHomeDecoder::hasLengthByte(uint8_t objID) {
    switch (objID) {
        case 0x53:
        case 0x54:
            return true;
        default:
            return false;
    }
}

int BTHomeDecoder::getObjectDataLength(uint8_t objID) {
    switch (objID) {
        case 0x00: // packet id
            return 1;
        case 0x01: // battery percentage (1 byte)
            return 1;
        case 0x02:
        case 0x03:
        case 0x08:
        case 0x0C: // battery voltage (2 bytes)
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x3C:
        case 0x3F:
        case 0x40:
        case 0x41:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
        case 0x48:
        case 0x51:
        case 0x52:
        case 0x56:
        case 0x5E:
        case 0x5F:
            return 2;
        case 0x04:
        case 0x05: // illuminance (3 bytes)
        case 0x0A:
        case 0x0B:
        case 0x42:
        case 0x49:
        case 0x4B:
            return 3;
        // Binary sensors (all uint8, 1 byte)
        case 0x0F: // generic boolean
        case 0x10: // power (binary)
        case 0x11: // opening
        case 0x15: // battery (binary)
        case 0x16: // battery charging
        case 0x17: // carbon monoxide
        case 0x18: // cold
        case 0x19: // connectivity
        case 0x1A: // door
        case 0x1B: // garage door
        case 0x1C: // gas (binary)
        case 0x1D: // heat
        case 0x1E: // light
        case 0x1F: // lock
        case 0x20: // moisture (binary)
        case 0x21: // motion
        case 0x22: // moving
        case 0x23: // occupancy
        case 0x24: // plug
        case 0x25: // presence
        case 0x26: // problem
        case 0x27: // running
        case 0x28: // safety
        case 0x29: // smoke
        case 0x2A: // sound
        case 0x2B: // tamper
        case 0x2C: // vibration
        case 0x2D: // window
        case 0x2E: // humidity (1 byte)
        case 0x2F: // soil moisture (1 byte)
        case 0x3A: // button
        case 0x57: // temperature (sint8)
        case 0x58: // temperature (sint8, factor 0.35)
            return 1;
        case 0x3E:
        case 0x4C:
        case 0x4D:
        case 0x4E:
        case 0x4F:
        case 0x50:
        case 0x5B:
        case 0x5C:
            return 4;
        case 0x5D: // current (sint16)
        case 0x61: // rotational speed
            return 2;
        case 0x60: // channel
            return 1;
        case 0x62: // speed (signed)
        case 0x63: // acceleration (signed)
            return 4;
        case 0xF0: // device type id
            return 2;
        case 0xF1: // firmware version (uint32)
            return 4;
        case 0xF2: // firmware version (uint24)
            return 3;
        default:
            return -1;
    }
}

float BTHomeDecoder::getObjectFactor(uint8_t objID) {
    switch (objID) {
        case 0x01:
            // Battery % is 1 byte, 0..100 => factor = 1 => raw 98 => 98%
            return 1.0f;
        case 0x02:
        case 0x03:
        case 0x08:
        case 0x14:
            return 0.01f;
        case 0x04:
        case 0x0B:
            return 0.01f;
        case 0x05:
            // Illuminance is 3 bytes * factor 0.01 => e.g. raw = 12345 => 123.45 lux
            // or pick a different factor if your device uses another scale.
            return 0.01f;
        case 0x0A:
        case 0x0C:
            // Battery voltage is 2 bytes in millivolts => factor = 0.001 => 3241 => 3.241 V
            return 0.001f;
        case 0x2E:
            // If you stored humidity as a single byte (0-100),
            // factor = 1.0 => final value = raw (like 55 => 55%)
            return 1.0f;
        case 0x2F:
            // Soil moisture is 1 byte, 0..100 => factor = 1 => raw 47 => 47%
            return 1.0f;
        case 0x3F: // rotation
        case 0x5F: // precipitation
            return 0.1f;
        case 0x45:
        case 0x46:
        case 0x47:
        case 0x4A:
        case 0x41: // distance (m)
            return 0.1f;
        case 0x42:
        case 0x43:
        case 0x4B:
        case 0x4C:
        case 0x4D:
        case 0x4E:
        case 0x4F:
        case 0x51:
        case 0x52:
        case 0x5D: // current (sint16)
            return 0.001f;
        case 0x44:
        case 0x5C:
        case 0x5E:
            return 0.01f;
        case 0x62: // speed (signed)
        case 0x63: // acceleration (signed)
            return 0.000001f;
        default:
            return 1.0f;
    }
}

bool BTHomeDecoder::getObjectSignedNess(uint8_t objID) {
    switch (objID) {
        case 0x02: // temperature
        case 0x08: // dewpoint
        case 0x3F: // rotation
        case 0x45: // temperature
        case 0x57: // temperature (sint8)
        case 0x58: // temperature (sint8, factor 0.35)
        case 0x5A: // count (sint16)
        case 0x5B: // count (sint32)
        case 0x5C: // power (sint32)
        case 0x5D: // current (sint16)
        case 0x62: // speed (signed)
        case 0x63: // acceleration (signed)
            return true;
        default:
            return false;
    }
}

String BTHomeDecoder::getObjectUnit(uint8_t objID) {
    switch (objID) {
        case 0x01: // battery
        case 0x03: // humidity
        case 0x14: // moisture
        case 0x2E: // humidity
        case 0x2F: // moisture
            return "percent";
        case 0x02: // temperature
        case 0x08: // dewpoint
        case 0x45: // temperature
            return "°C";
        case 0x04: // pressure
            return "hPa";
        case 0x05: // illuminance
            return "lux";
        case 0x06: // mass (kg)
            return "kg";
        case 0x07: // mass (lb)
            return "lb";
        case 0x0A: // energy
        case 0x4D: // energy
            return "kWh";
        case 0x0B: // power
            return "W";
        case 0x0C: // voltage
        case 0x4A: // voltage
            return "V";
        case 0x0D: // pm2.5
        case 0x0E: // pm10
        case 0x13: // tvoc
            return "ug/m3";
        case 0x12: // co2
            return "ppm";
        case 0x3F: // rotation
            return "°";
        case 0x40: // distance (mm)
            return "mm";
        case 0x41: // distance (m)
            return "m";
        case 0x42: // duration
            return "s";
        case 0x43: // current
            return "A";
        case 0x44: // speed
        case 0x62: // speed
            return "m/s";
        case 0x47: // volume
        case 0x4E: // volume
        case 0x4F: // water
            return "L";
        case 0x48: // volume
            return "mL";
        case 0x49: // volume Flow Rate
            return "m3/hr";
        case 0x4B: // gas
        case 0x4C: // gas
            return "m3";
        case 0x51: // acceleration
        case 0x63: // acceleration
            return "m/s²";
        case 0x52: // gyroscope
            return "°/s";
        case 0x2D:
            return "open";
        default:
            return "";
    }
}

String BTHomeDecoder::getObjectName(uint8_t objID) {
    switch (objID) {
        case 0x00:
            return "packet_id";
        case 0x01:
            return "battery_percent";
        case 0x02:
            return "temperature";
        case 0x03:
            return "humidity";
        case 0x04:
            return "pressure";
        case 0x05:
            return "illuminance";
        case 0x0A:
            return "energy";
        case 0x0B:
            return "power";
        case 0x0C:
            return "battery_voltage";
        case 0x12:
            return "CO2";
        case 0x13:
            return "VOC";
        case 0x14:
            return "moisture";
        // Binary sensors
        case 0x0F:
            return "generic_boolean";
        case 0x10:
            return "power_binary";
        case 0x11:
            return "opening";
        case 0x15:
            return "battery_low";
        case 0x16:
            return "battery_charging";
        case 0x17:
            return "carbon_monoxide";
        case 0x18:
            return "cold";
        case 0x19:
            return "connectivity";
        case 0x1A:
            return "door";
        case 0x1B:
            return "garage_door";
        case 0x1C:
            return "gas_detected";
        case 0x1D:
            return "heat";
        case 0x1E:
            return "light";
        case 0x1F:
            return "lock";
        case 0x20:
            return "moisture_binary";
        case 0x21:
            return "motion";
        case 0x22:
            return "moving";
        case 0x23:
            return "occupancy";
        case 0x24:
            return "plug";
        case 0x25:
            return "presence";
        case 0x26:
            return "problem";
        case 0x27:
            return "running";
        case 0x28:
            return "safety";
        case 0x29:
            return "smoke";
        case 0x2A:
            return "sound";
        case 0x2B:
            return "tamper";
        case 0x2C:
            return "vibration";
        case 0x2D:
            return "window";
        case 0x2E:
            return "humidity";
        case 0x2F:
            return "soil_moisture";
        case 0x3A:
            return "button";
        case 0x3C:
            return "dimmer";
        case 0x3F:
            return "rotation";
        case 0x40:
            return "distance_mm";
        case 0x41:
            return "distance_m";
        case 0x42:
            return "duration_sec";
        case 0x43:
            return "current_A";
        case 0x44:
            return "speed_mps";
        case 0x45:
            return "temperature_0.1C";
        case 0x46:
            return "UV_index";
        case 0x47:
            return "volume_liters";
        case 0x48:
            return "volume_milliliters";
        case 0x49:
            return "flow_rate";
        case 0x4A:
            return "voltage_V";
        case 0x4B:
            return "gas_m3";
        case 0x50:
            return "timestamp";
        case 0x53:
            return "text";
        case 0x54:
            return "raw";
        case 0x55:
            return "volume_storage";
        case 0x56:
            return "conductivity";
        case 0x57:
        case 0x58:
            return "temperature";
        case 0x59:
        case 0x5A:
        case 0x5B:
            return "count";
        case 0x5C:
            return "power";
        case 0x5D:
            return "current";
        case 0x5E:
            return "direction";
        case 0x5F:
            return "precipitation";
        case 0x60:
            return "channel";
        case 0x61:
            return "rotational_speed";
        case 0x62:
            return "speed";
        case 0x63:
            return "acceleration";
        case 0xF0:
            return "device_type_id";
        case 0xF1:
        case 0xF2:
            return "firmware_version";
        default:
            return "unknown";
    }
}

float BTHomeDecoder::parseSignedLittle(const uint8_t *data, size_t len, float factor) {
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

float BTHomeDecoder::parseUnsignedLittle(const uint8_t *data, size_t len, float factor) {
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