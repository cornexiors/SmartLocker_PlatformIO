# Smart Locker Controller (ESP32-S3) — v2.0

An offline-first, cryptographically authenticated smart locker controller built on the **ESP32-S3-DevKitC-1 (N16R8)** using PlatformIO and the Arduino framework.

This document serves as the technical handoff and reference guide for developers maintaining or extending the **physical hardware**, **embedded firmware**, and **cloud backend**.

---

## 1. System Overview & Core Design Rules

The v2.0 architecture enforces a **single-use locker lifecycle** tied to registered mobile app accounts, combining local offline resilience (NVS storage + internal RTC) with encrypted MQTT synchronization:

1. **Default State:** All compartments default to **Locked (`180°`)** and **Available**.
2. **Assignment (`A` Key):** Only users registered in the companion app (and synced to the ESP32's local NVS database) can claim a locker. Each `user_id` is limited to **one active locker** at a time. Claiming a locker unlocks it (`0°`) and marks it **Assigned**.
3. **Auto-Lock on Door Close:** Whenever a reed switch transitions from `OPEN` to `CLOSED`, the servo automatically rotates to `180°` (Locked)—both after initial item drop-off and after item retrieval.
4. **Single-Use Unlock & Auto-Release (`B` Key):** When an assigned user unlocks their compartment via RFID or PIN, the servo unlocks (`0°`) and the firmware **immediately clears their assignment**, returning the locker to **Available**. Once the user closes the empty door, it relocks automatically (`180°`) for the next user.
5. **Overdue (`>24h`) Hold & Backend Warnings:** Daily reminder notifications (15:00 and 17:00 WIB) are managed by the backend. If a locker remains assigned past **24 hours**, the ESP32 locks the compartment, blocks regular user access (`LOCKER EXPIRED - CONTACT ADMIN`), increments the home screen's `OVERDUE (>24H)` badge, and holds the locker until an operator inspects it and issues a `RELEASE` (via MQTT or local Admin Mode).
6. **Door-Open Safety Alert:** Leaving any locker door open for more than **60 seconds** triggers a repeating local buzzer alert and emits a `DOOR_OPEN_TIMEOUT` event to the backend.

---

## 2. Bill of Materials (Component List)

| Qty | Component | Specification / Notes |
| :-- | :--- | :--- |
| **1** | **Microcontroller** | **ESP32-S3-DevKitC-1 (N16R8)** — 16 MB Flash, 8 MB PSRAM, dual USB-C ports (`USB` native + `COM` UART). |
| **4** | **Lock Actuators** | **180° Positional Servo Motors** (e.g., **MG90S 180°** metal-gear recommended, or **SG90 180°**). *Do not use 360° continuous-rotation servos.* |
| **1** | **RFID Reader** | **MFRC522** 13.56 MHz SPI module (3.3V logic) + MIFARE Classic / compatible RFID tags or cards. |
| **1** | **Display + SD Module** | **ST7735 TFT LCD** (`128×140` / 1.44"–1.8" SPI, 3.3V logic) with integrated microSD card socket. |
| **1** | **microSD Card** *(Optional)* | FAT32 formatted microSD card for append-only `/locker.log` JSON audit logs. |
| **1** | **I/O Expander** | **PCF8575** 16-bit I2C I/O expander (`0x20`–`0x27`, 3.3V logic) for keypad scanning & future expansion. |
| **1** | **Keypad** | **4×4 Matrix Membrane Keypad** (`1–9`, `0`, `A–D`, `*`, `#`). |
| **1–4** | **Door Sensors** | **3-Pin Digital Reed Switch Modules** (with `DO` output, active `LOW`) + door magnets. |
| **1** | **Buzzer** | **2-Pin Passive Buzzer** (driven via 2 kHz PWM `tone()`). |
| **1** | **Status LED** | 3 mm / 5 mm LED + `220 Ω`–`330 Ω` current-limiting resistor (network online indicator). |
| **1** | **External RTC** *(Optional)* | **3.3V-native DS3231** I2C RTC module (`0x68`). *Optional; ESP32 internal RTC + NTP is used by default.* |
| **1** | **Servo Power Supply** | Regulated **5V 2A–3A DC** supply (or DC-DC buck converter stepping 12V down to 5.0V) dedicated to servos. |
| **1** | **Decoupling Capacitor** | **100 µF – 470 µF Electrolytic Capacitor** (10V or 16V rated) across the servo 5V/GND rail. |
| **1** | **Enclosure** | 4-compartment locker chassis (`30 × 30 × 40 cm` compartment dimensions). |

---

## 3. GPIO Pinout & Bus Allocation Table

All pin assignments are centralized in `include/Config.h` under `namespace Cfg`.

### 3.1 ESP32-S3 Direct GPIOs

| Subsystem | Signal / Function | ESP32-S3 GPIO | Direction / Mode | Notes |
| :--- | :--- | :---: | :--- | :--- |
| **I2C Bus** | `I2C_SDA` | `GPIO 1` | Bidirectional (Open-Drain) | Shared by PCF8575 & optional 3.3V RTC (`100 kHz`). |
| **I2C Bus** | `I2C_SCL` | `GPIO 2` | Output (Open-Drain) | Shared by PCF8575 & optional 3.3V RTC (`100 kHz`). |
| **SPI Bus** | `SPI_MOSI` | `GPIO 11` | Output | Shared by ST7735 TFT, MFRC522 RFID, and microSD. |
| **SPI Bus** | `SPI_SCK` | `GPIO 12` | Output | Shared SPI clock. |
| **SPI Bus** | `SPI_MISO` | `GPIO 13` | Input | Shared by MFRC522 RFID and microSD. |
| **TFT Display** | `TFT_CS` | `GPIO 10` | Output (Active `LOW`) | ST7735 Chip Select. |
| **TFT Display** | `TFT_DC` | `GPIO 9` | Output | ST7735 Data/Command select. |
| **TFT Display** | `TFT_RST` | `GPIO 8` | Output | ST7735 Hardware Reset. |
| **RFID Reader** | `RFID_CS` | `GPIO 7` | Output (Active `LOW`) | MFRC522 Chip Select. |
| **RFID Reader** | `RFID_RST` | `GPIO 6` | Output | MFRC522 Hardware Reset. |
| **microSD** | `SD_CS` | `GPIO 5` | Output (Active `LOW`) | microSD Chip Select (`4 MHz` SPI). |
| **Actuators** | `SERVO_0` (`LOCKER_001`) | `GPIO 4` | PWM Output (50 Hz) | `0°` = `1000 µs` (Unlock), `180°` = `2000 µs` (Lock). |
| **Actuators** | `SERVO_1` (`LOCKER_002`) | `GPIO 21` | PWM Output (50 Hz) | `0°` = `1000 µs` (Unlock), `180°` = `2000 µs` (Lock). |
| **Actuators** | `SERVO_2` (`LOCKER_003`) | `GPIO 18` | PWM Output (50 Hz) | `0°` = `1000 µs` (Unlock), `180°` = `2000 µs` (Lock). |
| **Actuators** | `SERVO_3` (`LOCKER_004`) | `GPIO 16` | PWM Output (50 Hz) | `0°` = `1000 µs` (Unlock), `180°` = `2000 µs` (Lock). |
| **Door Sensors** | `REED_0` (`LOCKER_001`) | `GPIO 14` | `INPUT_PULLUP` | `LOW` = Door Closed, `HIGH` = Door Open (`45 ms` debounce). |
| **Door Sensors** | `REED_1..3` (`LOCKER_002..004`) | `0xFF` (`UNUSED`) | — | Unassigned pins default to `CLOSED` until wired & configured. |
| **Indicators** | `LED_PIN` | `GPIO 17` | Output | `HIGH` when Wi-Fi + MQTT are both online. |
| **Indicators** | `BUZZER_PIN` | `GPIO 38` | PWM Output (`2000 Hz`) | Passive buzzer feedback & 60s door-open alarm. |

### 3.2 PCF8575 I2C Expander Pinout (`0x20`)

| PCF8575 Pins | Function | Direction | Notes |
| :--- | :--- | :--- | :--- |
| **`P0` – `P3`** | 4×4 Keypad Rows (`R1`–`R4`) | Quasi-Bidirectional Output | Driven `LOW` sequentially during matrix scan. |
| **`P4` – `P7`** | 4×4 Keypad Columns (`C1`–`C4`) | Quasi-Bidirectional Input | Pulled `HIGH`; reads `LOW` when key is pressed. |
| **`P8` – `P15`** | Reserved Digital Outputs | Output (Default `HIGH`) | Preserved via bitmask (`_pcfOutputState = 0xFF00`) during scans. |

---

## 4. Recommended Wiring & Power Management

Follow these hardware rules strictly to prevent voltage spikes, brownouts, and permanent damage to the ESP32-S3:

### 4.1 Power Rail Separation & Common Ground
* **Isolate the Servo 5V Rail:** **Never** power SG90/MG90S servos from the ESP32-S3's `5V` pin, and **never** connect an external 5V supply to the ESP32-S3's `5V` pin while a USB cable is plugged into a computer. A stalled micro-servo draws `650–850 mA`, which trips USB over-current protection and can burn out the onboard USB-to-UART bridge IC.
* **Dedicated Regulated 5V Supply for Servos:** Power all servo `VCC` (red) wires from a dedicated **5V 2A–3A regulated supply** or a step-down buck converter set to `5.0V`.
  * *Avoid feeding 12V directly into an MB-102 breadboard supply when driving motors*—the 7V drop overheats its linear regulator and can cause it to fail shorted, passing 12V spikes onto the 5V rail. Use a `7V–9V` adapter or a switching buck converter instead.
* **Mandatory Shared Ground (`GND`):** Tie the `GND` of the external 5V servo supply directly to a `GND` pin on the ESP32-S3. Without a common ground reference, servo PWM signals will jitter or fail to reach target angles.
* **Decoupling Capacitor:** Solder or insert a **100 µF to 470 µF electrolytic capacitor** directly across the `+5V` and `GND` rails next to the servo connectors (observe polarity: stripe = `GND`) to absorb inductive back-EMF spikes when servos start or stop.

### 4.2 3.3V Logic & Bus Hygiene
* **Strictly 3.3V Peripheral Logic:** The ESP32-S3 GPIOs are **not 5V tolerant**. Power the MFRC522, ST7735 TFT, PCF8575, reed switch modules, and any optional RTC strictly from the **`3V3`** rail.
  * *Warning on RTC Modules:* Do not use 5V-only DS1307 modules that have onboard pull-up resistors connected to 5V—they back-feed 5V into `GPIO 1` and `GPIO 2`. Either omit the external RTC (the firmware automatically syncs the ESP32's internal RTC via NTP) or use a native **3.3V DS3231** module.
* **SPI Bus Lead Lengths:** Because the ST7735 TFT, MFRC522 RFID, and microSD share `GPIO 11/12/13`, keep SPI jumper wires under **15 cm** and use solid-core or crimped Dupont connectors to avoid signal ringing.
* **Power-Down Before Rewiring:** Always disconnect both USB power and the external servo supply before moving servo signal pins (`GPIO 4, 21, 18, 16`) or adjusting breadboard jumpers.

### 4.3 USB Port Usage (`USB` vs `COM`)
* Connect your development PC to the port labeled **`USB`** (native ESP32-S3 USB OTG on `GPIO 19/20`) using a **USB-A to USB-C data cable**.
* *Note:* USB-C-to-USB-C laptop PD chargers may fail to power DevKit clones that omit 5.1 kΩ CC pulldown resistors; always use a USB-A power source or USB-A-to-C cable.

---

## 5. Firmware Architecture & Build Setup

### 5.1 `platformio.ini` Configuration
Ensure your `platformio.ini` includes the native USB CDC build flags so firmware uploads and `Serial` logs route through the native `USB` port:

```ini
[env:esp32-s3-n16r8]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200

build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1

lib_deps =
    knolleary/PubSubClient
    madhephaestus/ESP32Servo
    adafruit/RTClib
    adafruit/Adafruit GFX Library
    adafruit/Adafruit ST7735 and ST7789 Library
    miguelbalboa/MFRC522
```

### 5.2 Module Overview

| File | Responsibility |
| :--- | :--- |
| `include/Config.h` | Hardware pinout, Wi-Fi/MQTT credentials, TLS flags, WIB offset (`UTC+7`), admin PIN (`999999`), and app registration URL. |
| `include/App.h` | Core data structs (`LockerRecord`, `UserRecord`), `ActionStatus` enum, and class declarations. |
| `src/Hardware.cpp` | Low-level I2C/SPI drivers, keypad matrix scanner, MFRC522 reader, reed switch debouncer, servo PWM, and `128×140` graphic TFT renderer (including `PROGMEM` QR bitmap). |
| `src/Storage.cpp` | Three NVS namespaces (`lockerdata`, `lockerusers`, `lockersync` FIFO queue) + optional microSD `/locker.log` writer. |
| `src/ClockService.cpp` | Manages ESP32 internal RTC (`settimeofday`) synced via NTP on boot, after power loss, and every 2 hours (`NTP_RESYNC_MS`). |
| `src/LockerManager.cpp` | Enforces single-use rental rules, PBKDF2-SHA256 PIN verification, 24h expiry holds, >60s door-open alerts, and user database CRUD. |
| `src/UserInterface.cpp` | Non-blocking FSM (`HOME`, `ASSIGN_ASK`, `QR_CODE`, `ASSIGN_CRED`, `UNLOCK_CRED`, `RESULT`, `ADMIN_PIN`, `ADMIN_MENU`) with an 18s idle timeout. |
| `src/Network.cpp` | `WiFiClientSecure` TLS / `PubSubClient` MQTT client with automatic reconnection, retained state publishing, and topic routing. |
| `src/main.cpp` | Main cooperative scheduler, HMAC-SHA256 packet verifier (`verifySignedPacket`), nonce replay cache, and USB Serial debug console. |

### 5.3 On-Device Keypad Controls & Hidden Admin Mode
* **Home Screen:**
  * Press **`A`** → **Assign Locker**: Prompts *"Registered in app?"* (`A: Yes` proceeds to credential input; `B: No` displays the app registration QR code and URL).
  * Press **`B`** → **Unlock Locker**: Prompts *"Input PIN or tap RFID"* with an 8-slot underlined PIN box.
  * Press **`D`** → **Admin Mode**: Prompts for `ADMIN_MASTER_PIN` (`999999` by default). Inside Admin Mode, staff can inspect live states (`AVL`/`USE`/`EXP`, `LCK`/`UNL`, door `C`/`O`), press **`1`–`4`** to manually `RELEASE` any locker offline, or press **`A`** to trigger a `user_database_sync`.
* **Navigation Keys:** `C` = Back / Cancel, `*` = Delete digit, `#` = Submit PIN.

### 5.4 Updating the Registration QR Code
To update the QR code displayed to unregistered users (`Hardware::drawQrCode`):
1. Update `Cfg::APP_REGISTER_URL` in `include/Config.h`.
2. Generate a **64×64 pixel** monochrome QR code for your registration URL, convert it to a 1-bit C byte array (`512 bytes`, `PROGMEM`) using a tool like *image2cpp*, and replace `QR_BITMAP` in `src/Hardware.cpp`.

---

## 6. Backend Integration & MQTT Specification

### 6.1 Transport & Cryptographic Security
* **TLS Encryption:** The ESP32 connects via `WiFiClientSecure` on **port `8883`** (`Cfg::MQTT_USE_TLS = true`). For production brokers with a private CA, paste the PEM Root CA certificate into `Cfg::MQTT_ROOT_CA` in `include/Config.h`.
* **HMAC-SHA256 Payload Authentication:** Every message sent *from* the backend to the ESP32 (`command` and `users/sync`) must be pipe-delimited (`|`) and signed using **HMAC-SHA256** with `Cfg::HMAC_SECRET`:
  * **Canonical String:** All fields except the final signature joined by `|` (e.g., `FIELD_0|FIELD_1|...|TIMESTAMP|NONCE`).
  * **Signature:** 64-character lowercase hexadecimal HMAC-SHA256 digest of the canonical string.
  * **Replay Protection:** Each message must include a unique `NONCE` (the ESP32 caches the last 24 nonces and rejects duplicates with `REPLAY`) and a Unix UTC `TIMESTAMP`.

### 6.2 MQTT Topics & Payload Schemas (`MQTT_BASE = "smartlocker/demo/"`)

| Topic | Direction | Retained | Payload Format & Backend Handling |
| :--- | :--- | :---: | :--- |
| `device/status` | ESP32 → Backend | Yes | `"ONLINE"` on connect; `"OFFLINE"` via MQTT Last Will & Testament (LWT). |
| `LOCKER_00X/state` | ESP32 → Backend | Yes | Published on every state change and 30s heartbeat:<br>`{"locker_id":"LOCKER_001","locked":true,"assigned":true,"expired":false,"user_id":"U101","method":"RFID"}` |
| `device/event` | ESP32 → Backend | No | Buffered in NVS when offline and drained at 1 msg / 250 ms:<br>`{"event_id":"...","device_id":"LOCKER_CONTROLLER_001","event":"LOCKER_UNLOCKED","locker_id":"LOCKER_001","user_id":"U101","locked":false,"assigned":false,"method":"PIN","timestamp":1794883680}` |
| `users/request` | ESP32 → Backend | No | Sent on MQTT connect or Admin sync trigger:<br>`{"device_id":"LOCKER_CONTROLLER_001","action":"SYNC_REQUEST","user_count":12}` |
| `users/sync` | Backend → ESP32 | No | Signed user database mutations (see formats in Section 6.3). |
| `LOCKER_00X/command` | Backend → ESP32 | No | Signed remote actuator commands (see format in Section 6.3). |
| `LOCKER_00X/ack` | ESP32 → Backend | No | Command response: `ACCEPTED\|LOCKER_001\|CMD_ID\|ACCEPTED` or `REJECTED\|LOCKER_001\|CMD_ID\|REASON`. |

### 6.3 Backend-to-ESP32 Signed Message Formats

1. **Remote Locker Command (6 fields):**
   ```text
   LOCKER_ID|ACTION|TIMESTAMP|NONCE|COMMAND_ID|SIGNATURE
   ```
   * `ACTION`: `LOCK`, `UNLOCK`, or `RELEASE` (clears assignment & expired hold).
   * Duplicate `COMMAND_ID` retries return the cached ACK idempotently.

2. **User Database Upsert (8 fields):**
   ```text
   UPSERT|USER_ID|RFID_UID|SALT|PIN_HASH|TIMESTAMP|NONCE|SIGNATURE
   ```
   * `RFID_UID`: Uppercase hex UID (or empty string if user has no RFID registered).
   * `SALT` & `PIN_HASH`: **Must match the ESP32's PBKDF2 configuration** so app-created PINs verify offline on the keypad:
     * **Algorithm:** `PBKDF2-HMAC-SHA256`
     * **Iterations:** `10,000` (`Cfg::PIN_PBKDF2_ITERATIONS`)
     * **Salt:** `1–16` ASCII/hex characters
     * **Derived Key Length:** `32 bytes` formatted as a **64-character lowercase hex string**

3. **User Delete (5 fields) & Clear All (4 fields):**
   ```text
   DELETE|USER_ID|TIMESTAMP|NONCE|SIGNATURE
   CLEAR|TIMESTAMP|NONCE|SIGNATURE
   ```

### 6.4 Required Backend State Synchronization Rules
* **Immediate Release on `LOCKER_UNLOCKED`:** When the backend receives `event == "LOCKER_UNLOCKED"` on `device/event` (or `assigned: false` on `LOCKER_00X/state`), it must **immediately mark the locker as `AVAILABLE`** in the backend database and notify the user that their rental session has ended.
* **Scheduled Expiry Warnings (15:00 & 17:00 WIB):** The ESP32 no longer tracks daily warning hours. The backend must run a scheduled job at `15:00` and `17:00` WIB to query active locker rentals and send push/in-app reminders.
* **Overdue (`>24h`) Resolution:** When `event == "LOCKER_EXPIRED"` is received, flag the compartment as overdue in the admin dashboard. After staff physically inspect and empty the compartment, send a signed `RELEASE` command (or use on-site Admin Mode, which emits `LOCKER_RELEASED`) to return the locker to `AVAILABLE`.