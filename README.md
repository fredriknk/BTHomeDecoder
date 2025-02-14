# BtHomeDecoder for esp32

Decoder for the BTHome bluetooth protocol without the need of a mqtt server or anything else. just run it on an ESP32 and get messages back over serial. Made this to help debug my soilssense bluetooth sensor.

Note, use the specified versions of the NimBLEDevice@1.3.1 and ArduinoJson@6.18.3 when running the main. 

Example output from serial when running the main.py on an esp32device

```
===== BTHome Advertisement Decoded =====
{
  "id": "C3:DC:13:B3:B0:3C",
  "name": "ssen",
  "bthome_version": 2,
  "bthome_encrypted": false,
  "rssi": -40,
  "measurements": [
    {
      "object_id": 2,
      "name": "temperature",
      "value": 23.77
    },
    {
      "object_id": 46,
      "name": "humidity",
      "value": 15
    },
    {
      "object_id": 5,
      "name": "illuminance",
      "value": 728
    },
    {
      "object_id": 12,
      "name": "battery_voltage",
      "value": 3.241
    },
    {
      "object_id": 47,
      "name": "soil_moisture",
      "value": 0
    },
    {
      "object_id": 1,
      "name": "battery_percent",
      "value": 100
    }
  ]
}
========================================
```