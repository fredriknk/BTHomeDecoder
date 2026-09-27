# Agent Learnings

## arduino-esp32 4.0.0 (pioarduino platform-espressif32 61.x RC1) breaks BLE API

`platformio.ini` pins `platform = pioarduino .../61.04.00-RC1/...zip`, which ships
`framework-arduinoespressif32 @ 4.0.0`. That version fully rewrote the BLE library:

- `BLEDevice.h` / `BLEScan.h` / `BLEAdvertisedDevice.h` gone — replaced by single `<BLE.h>`
  exposing a global `BLE` singleton (`BLEClass`).
- `BLEDevice::init()` / `BLEDevice::getScan()` → `BLE.begin()` / `BLE.getScan()`.
- `BLEScan` is now a lightweight copyable handle (value type), not a pointer.
- `BLEAdvertisedDeviceCallbacks` class removed. Use `BLEScan::onResult(std::function<void(const BLEAdvertisedDevice&)>)`
  instead of `setAdvertisedDeviceCallbacks(new Callbacks(), ...)`.
- `pBLEScan->start(seconds, false)` returning `BLEScanResults*` → `pBLEScan.startBlocking(durationMs)`
  returning `BLEScan::Results` by value (`.size()` instead of `->getCount()`).
- `advertisedDevice.getManufacturerData()` / `getServiceData(idx)` no longer return `String`/`std::string`
  — now `const uint8_t* getManufacturerData(size_t *len)` and `const uint8_t* getServiceData(size_t idx, size_t *len)`.
- `getServiceDataUUIDCount()` renamed to `getServiceDataCount()`.
- `getAddress().toString()`, `getName()`, `haveX()`, `getTXPower()`, `getServiceUUID()` accessors kept
  same names/signatures — no ifdef needed there. **But** on the pre-4.x (3.x) lib these accessors are
  all non-`const` member functions (`BLEAddress getAddress();` not `... getAddress() const;`). A shared
  `onAdvertised(const BLEAdvertisedDevice &advertisedDevice)` free function compiles fine against the
  4.x lib but fails `-fpermissive` ("passing const ... discards qualifiers") against 3.x. Fix: take the
  parameter **by value**, `onAdvertised(BLEAdvertisedDevice advertisedDevice)` — matches the original
  3.x callback signature (`onResult(BLEAdvertisedDevice advertisedDevice)` was also by value) and still
  binds fine as a `std::function<void(const BLEAdvertisedDevice&)>` for 4.x's `onResult()`.

Detect version with `ESP_ARDUINO_VERSION_MAJOR` (from `esp_arduino_version.h`, pulled in by `Arduino.h`).
Fixed by ifdefing `examples/BTHomeScan/BLEScanner.cpp` on `#if ESP_ARDUINO_VERSION_MAJOR >= 4` at each
divergence point (includes, `Impl::pBLEScan` type, manufacturer/service-data extraction, callback
registration, `scanTask`), keeping the pre-4.x code path for older platform pins. Verified building
against both `61.04.00-RC1` (framework 4.0.0) and `55.03.312` (framework 3.3.12) pins in `platformio.ini`.

## PlatformIO stale `.pio/libdeps` copy of `file://src` local library

`lib_deps` includes `file://src` (this repo's own `src/` as an Arduino library). PlatformIO's Library
Manager copies (not symlinks) it into `.pio/libdeps/<env>/src/` on first install and does **not**
reliably re-sync on later edits to `src/*.h` — a stale copy silently shadows the real source and causes
confusing build errors unrelated to the actual current code (e.g. a `#include "mbedtls/ccm.h"` guard
that had already been fixed in `src/BTHomeDecoder.h` still failed to build because the stale libdeps
copy pre-dated the fix). Fix: `rm -rf .pio/libdeps .pio/build` and rebuild when local-library changes
don't seem to take effect.

## Two `pio` binaries can shadow each other

This project's directory has a `.venv` (`source .venv/bin/activate`) that also contains an old
`platformio` package (Core v6.1.16), separate from the global Homebrew `pio` (Core v6.2.0). The
pioarduino platform requires Core >=6.2.0. Sourcing the venv put the old `pio` first on `PATH` and
build failed with `IncompatiblePlatform`. Use `/opt/homebrew/bin/pio` directly (or deactivate the venv)
for PlatformIO builds in this repo.
