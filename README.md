# Smart Locker — ESP32-S3 PlatformIO Firmware

Modular Arduino-framework firmware for ESP32-S3-DevKitC-1 N16R8.

## Current hardware profile

| Function | ESP32-S3 connection |
|---|---:|
| I²C SDA / PCF8575 | GPIO 1 |
| I²C SCL / PCF8575 | GPIO 2 |
| Servo output for LOCKER_001 | GPIO 4 |
| Servo test output for LOCKER_002 | GPIO 21 |
| Integrated TFT SD chip-select | GPIO 5 |
| MFRC522 reset | GPIO 6 |
| MFRC522 NSS / SS | GPIO 7 |
| TFT reset | GPIO 8 |
| TFT DC / A0 | GPIO 9 |
| TFT chip-select | GPIO 10 |
| Shared SPI MOSI | GPIO 11 |
| Shared SPI SCK | GPIO 12 |
| Shared SPI MISO | GPIO 13 |
| 3-pin reed module DO | GPIO 14 (LOCKER_001 only) |
| LED (with 220–330 Ω resistor) | GPIO 17 |
| Passive buzzer positive | GPIO 38 (use this instead of GPIO37) |

**Pin correction required:** GPIO37 is connected to the octal PSRAM interface on the ESP32-S3 N16R8 and is not available as a normal GPIO. Move the passive buzzer positive wire from GPIO37 to GPIO38.

The RTC is intentionally disabled. Time-based rental functions use SNTP/NTP after Wi-Fi connects. Before NTP time becomes valid, offline assignments are retained and their rental deadline is initialized when valid time becomes available.

The SD card is the slot integrated into the ST7735 display module. It shares SPI with the TFT and MFRC522 and uses GPIO 5 as SD_CS. No separate SD module is expected.

## Multi-locker configuration

Edit `include/Config.h`:

- Increase `LOCKER_COUNT` when adding logical lockers.
- Add one unique ID to `LOCKER_IDS` for each locker.
- Add the matching actuator output to `SERVO_PINS`.
- Add a reed GPIO to `REED_PINS`, or use `PCF_UNUSED_PIN` if that locker has no sensor yet.

Current test map:

```cpp
constexpr uint8_t LOCKER_COUNT = 2;
constexpr char LOCKER_IDS[LOCKER_COUNT][12] = {"LOCKER_001", "LOCKER_002"};
constexpr uint8_t SERVO_PINS[LOCKER_COUNT] = {4, 21};
constexpr uint8_t REED_PINS[LOCKER_COUNT] = {14, PCF_UNUSED_PIN};
```

Only one SG90 is currently installed. For a one-servo GPIO test, physically move its signal wire between GPIO 4 and GPIO 21 before testing the corresponding locker. The firmware maintains separate logical locker states, but it cannot move one physical servo on two pins simultaneously. For a real two-servo installation, connect each servo signal to its configured GPIO and power the servos from a suitable external 5 V supply with common ground.

**Do not use GPIO 20 for this test while native USB CDC is enabled.** GPIO20 is the ESP32-S3 USB D+ pin and may interfere with USB serial/programming.

The second PCF8575 is detected as an optional digital-I/O expansion. Each logical locker can be configured as either a servo actuator or a PCF8575 digital output in `Config.h` using `LOCKER_ACTUATOR[]` and `EXPANDER_BITS[]`. The expander output is intended to drive a transistor/relay/driver input, not the actuator directly. PCF8575 pins are not servo PWM outputs; do not connect an SG90 signal directly to them. Set `EXPANDER_UNLOCK_HIGH[]` to match the driver input polarity and provide a suitable driver stage before enabling this mode.

## Current PCF8575 keypad map

- P0–P3: keypad rows 1–4
- P4–P7: keypad columns 1–4
- P8–P15: unused on the keypad board
- The optional second PCF8575 is detected at the next available address in 0x20–0x27.

The keypad scanner releases all lines high between scans. Set the PCF8575 address jumpers so the keypad board and expansion board have different I²C addresses.

## Backend/MQTT compatibility

The firmware retains the existing demo broker and topic convention used by the integrated firmware:

- Command: `smartlocker/demo/LOCKER_001/command`
- Acknowledgment: `smartlocker/demo/LOCKER_001/ack`
- Status: `smartlocker/demo/LOCKER_001/status`
- Device presence: `smartlocker/demo/device/status`

Command payload format is unchanged:

`LOCKER_ID|ACTION|TIMESTAMP|NONCE|COMMAND_ID|HMAC_SHA256`

The HMAC canonical string is the first five fields joined with `|`. Supported remote actions in this firmware are `LOCK`, `UNLOCK`, `EXTEND`, and `RELEASE`. The first two match the existing integrated backend command contract. Keep the same HMAC secret on the backend and device; the checked-in value is demo-only.

The firmware does not introduce new REST endpoints or change the backend API. The existing backend should continue publishing the signed MQTT command to the per-locker command topic.

## Software behavior

- Offline-first local RFID/PIN assignment and access.
- Local NVS persistence for locker records and queued events.
- Optional SD append-only event log.
- MQTT command authentication with HMAC-SHA256, a 32-command duplicate cache, and nonce replay protection.
- Event queue removes an item after PubSubClient reports a successful QoS 0 publish to the broker; this is broker acceptance, not an application-level backend acknowledgment. No new backend ACK endpoint/topic was introduced.
- Per-locker status and command acknowledgment topics.
- Rental duration: 24 hours; warning windows at 15:00 and 17:00 WIB; maximum two extensions.
- Expired rentals remain assigned and require operator release.
- Door-close auto-lock applies only to lockers with a configured reed sensor.
- PIN hashes use PBKDF2-HMAC-SHA256 (10,000 iterations); plaintext PINs are not stored or published.

## Build

Open this folder in PlatformIO and build environment `esp32-s3-n16r8`.

This package has been statically revised but **has not been compiled in this environment** because PlatformIO is not installed here. Confirm the build in PlatformIO before flashing. Also verify the exact ST7735 tab/offset for the 128×140 display module; the library's common ST7735 init profiles are not guaranteed to match every 128×140 board.

## Electrical notes

- MFRC522 must use 3.3 V.
- Confirm the TFT module supports 3.3 V logic and power before connecting VCC/BL.
- Servo power should come from the external 5 V rail, not the ESP32 3.3 V output. Join supply and ESP32 grounds.
- The DS1307/24C32 RTC is not initialized or used in this firmware revision. Keep it disconnected from the I²C bus for now, as requested.
- HiveMQ public broker on port 1883 is unencrypted and demo-only; do not use it for production credentials or locker control.
