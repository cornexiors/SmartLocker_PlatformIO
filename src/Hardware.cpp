
#include "App.h"

// ============================================================
// PCF8575 LOW-LEVEL ACCESS
// ============================================================

bool Hardware::pcfWrite(uint16_t value) {
    if (!_i2cReady) {
        return false;
    }

    // Preserve P8-P15 while the keypad scanner operates P0-P7.
    const uint16_t output =
        static_cast<uint16_t>(value & 0x00FFU) |
        _pcfOutputState;

    Wire.beginTransmission(_pcfAddress);
    Wire.write(static_cast<uint8_t>(output));
    Wire.write(static_cast<uint8_t>(output >> 8));

    return Wire.endTransmission() == 0;
}

bool Hardware::pcfRead(uint16_t& value) {
    if (!_pcfReady) {
        return false;
    }

    if (Wire.requestFrom(static_cast<int>(_pcfAddress), 2) != 2) {
        return false;
    }

    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();

    value =
        static_cast<uint16_t>(lo) |
        (static_cast<uint16_t>(hi) << 8);

    return true;
}

// ============================================================
// HARDWARE INITIALIZATION
// ============================================================

bool Hardware::begin() {
    // --------------------------------------------------------
    // I2C: one PCF8575 and one DS1307 RTC.
    // --------------------------------------------------------

    Wire.begin(Cfg::I2C_SDA, Cfg::I2C_SCL);
    Wire.setClock(100000);

    _i2cReady = true;

    // Find only the first PCF8575.
    // Do not initialize a second expander.
    for (
        uint8_t address = Cfg::PCF8575_ADDRESS_FIRST;
        address <= Cfg::PCF8575_ADDRESS_LAST;
        ++address
    ) {
        Wire.beginTransmission(address);

        if (Wire.endTransmission() == 0) {
            _pcfAddress = address;
            _pcfReady = true;
            break;
        }
    }

    if (_pcfReady) {
        _pcfOutputState = 0xFF00;
        pcfWrite(0xFFFF);
    }

    // --------------------------------------------------------
    // LED
    // --------------------------------------------------------

    pinMode(Cfg::LED_PIN, OUTPUT);
    digitalWrite(Cfg::LED_PIN, LOW);

    // --------------------------------------------------------
    // Passive buzzer
    // --------------------------------------------------------

    if (Cfg::BUZZER_ENABLED) {
        pinMode(Cfg::BUZZER_PIN, OUTPUT);
        digitalWrite(Cfg::BUZZER_PIN, LOW);
    }

    // --------------------------------------------------------
    // SPI CHIP SELECT PINS
    // --------------------------------------------------------

    pinMode(Cfg::RFID_CS, OUTPUT);
    digitalWrite(Cfg::RFID_CS, HIGH);

    pinMode(Cfg::TFT_CS, OUTPUT);
    digitalWrite(Cfg::TFT_CS, HIGH);

    pinMode(Cfg::SD_CS, OUTPUT);
    digitalWrite(Cfg::SD_CS, HIGH);

    SPI.begin(
        Cfg::SPI_SCK,
        Cfg::SPI_MISO,
        Cfg::SPI_MOSI
    );

    // --------------------------------------------------------
    // TFT INITIALIZATION
    // --------------------------------------------------------

    if (Cfg::TFT_INIT_TAB == 1) {
        _tft.initR(INITR_GREENTAB);
    } else if (Cfg::TFT_INIT_TAB == 2) {
        _tft.initR(INITR_REDTAB);
    } else {
        _tft.initR(INITR_BLACKTAB);
    }

    _tft.setRotation(Cfg::TFT_ROTATION);
    _tft.setTextWrap(false);

    _displayReady = true;

    // --------------------------------------------------------
    // MFRC522 INITIALIZATION
    // --------------------------------------------------------

    pinMode(Cfg::RFID_RST, OUTPUT);
    digitalWrite(Cfg::RFID_RST, HIGH);

    _rfid.PCD_Init();
    _rfidReady = true;

    // --------------------------------------------------------
    // REED SWITCHES AND SERVO CHANNELS
    // --------------------------------------------------------

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {

        if (Cfg::REED_PINS[i] != Cfg::PCF_UNUSED_PIN) {
            pinMode(Cfg::REED_PINS[i], INPUT_PULLUP);

            const bool closed =
                digitalRead(Cfg::REED_PINS[i]) ==
                Cfg::REED_CLOSED_LEVEL;

            _doorRaw[i] = closed;
            _doorStable[i] = closed;
            _doorChangedAt[i] = millis();
        }

        if (
            Cfg::LOCKER_ACTUATOR[i] == Cfg::ACTUATOR_SERVO &&
            Cfg::SERVO_PINS[i] != Cfg::PCF_UNUSED_PIN
        ) {
            _servos[i].setPeriodHertz(50);

            _servos[i].attach(
                Cfg::SERVO_PINS[i],
                Cfg::SERVO_MIN_US,
                Cfg::SERVO_MAX_US
            );

            _servos[i].write(Cfg::SERVO_LOCK_ANGLE);
        }
    }

    // --------------------------------------------------------
    // DIAGNOSTICS
    // --------------------------------------------------------

    Serial.printf(
        "PCF8575 keypad/output: %s (0x%02X)\n",
        _pcfReady ? "ready" : "not found",
        _pcfAddress
    );

    Serial.printf(
        "MFRC522: %s | ST7735: %s | SD CS: GPIO%u\n",
        _rfidReady ? "ready" : "failed",
        _displayReady ? "ready" : "failed",
        Cfg::SD_CS
    );

    Serial.printf(
        "Servo channels: GPIO%u, GPIO%u, GPIO%u, GPIO%u\n",
        Cfg::SERVO_PINS[0],
        Cfg::SERVO_PINS[1],
        Cfg::SERVO_PINS[2],
        Cfg::SERVO_PINS[3]
    );

    Serial.printf(
        "Passive buzzer: %s (GPIO%u)\n",
        Cfg::BUZZER_ENABLED ? "enabled" : "disabled",
        Cfg::BUZZER_PIN
    );

    // Peripheral failures should not prevent controller startup.
    return true;
}

// ============================================================
// KEYPAD
// ============================================================

char Hardware::scanKey() {
    if (!_pcfReady) {
        return 0;
    }

    for (uint8_t row = 0; row < 4; ++row) {
        const uint16_t drive =
            static_cast<uint16_t>(0xFFFFU & ~(1U << row));

        if (!pcfWrite(drive)) {
            return 0;
        }

        delayMicroseconds(50);

        uint16_t bits = 0xFFFF;

        if (!pcfRead(bits)) {
            return 0;
        }

        for (uint8_t col = 0; col < 4; ++col) {
            if ((bits & (1U << (col + 4))) == 0) {
                pcfWrite(0xFFFF);
                return Cfg::KEYMAP[row][col];
            }
        }
    }

    pcfWrite(0xFFFF);
    return 0;
}

// ============================================================
// RFID
// ============================================================

bool Hardware::readCard(String& uid) {
    if (
        !_rfidReady ||
        millis() - _lastRfidMs < Cfg::RFID_SCAN_MS
    ) {
        return false;
    }

    _lastRfidMs = millis();

    if (
        !_rfid.PICC_IsNewCardPresent() ||
        !_rfid.PICC_ReadCardSerial()
    ) {
        return false;
    }

    static const char hex[] = "0123456789ABCDEF";

    char uidBuffer[21] = {};

    const byte uidSize = min(
        static_cast<byte>(_rfid.uid.size),
        static_cast<byte>(10)
    );

    for (byte i = 0; i < uidSize; ++i) {
        const byte value = _rfid.uid.uidByte[i];

        uidBuffer[i * 2] = hex[value >> 4];
        uidBuffer[i * 2 + 1] = hex[value & 0x0F];
    }

    uid = uidBuffer;

    _rfid.PICC_HaltA();
    _rfid.PCD_StopCrypto1();

    return true;
}

// ============================================================
// REED SWITCHES
// ============================================================

bool Hardware::doorClosed(uint8_t index) {
    if (
        index >= Cfg::LOCKER_COUNT ||
        Cfg::REED_PINS[index] == Cfg::PCF_UNUSED_PIN
    ) {
        // No sensor is configured for this locker.
        return true;
    }

    return digitalRead(Cfg::REED_PINS[index]) ==
           Cfg::REED_CLOSED_LEVEL;
}

bool Hardware::doorChanged(uint8_t index, bool& closed) {
    if (
        index >= Cfg::LOCKER_COUNT ||
        Cfg::REED_PINS[index] == Cfg::PCF_UNUSED_PIN
    ) {
        return false;
    }

    const uint32_t now = millis();
    const bool raw = doorClosed(index);

    if (raw != _doorRaw[index]) {
        _doorRaw[index] = raw;
        _doorChangedAt[index] = now;
    }

    if (
        raw != _doorStable[index] &&
        now - _doorChangedAt[index] >= Cfg::REED_DEBOUNCE_MS
    ) {
        _doorStable[index] = raw;
        closed = raw;
        return true;
    }

    return false;
}

// ============================================================
// SERVO AND LOCKER ACTUATORS
// ============================================================

void Hardware::setServo(uint8_t index, bool locked) {
    setLockerActuator(index, locked);
}

bool Hardware::setLockerActuator(uint8_t index, bool locked) {
    if (index >= Cfg::LOCKER_COUNT) {
        return false;
    }

    bool ok = false;

    // --------------------------------------------------------
    // SERVO ACTUATOR
    // --------------------------------------------------------

    if (Cfg::LOCKER_ACTUATOR[index] == Cfg::ACTUATOR_SERVO) {

        if (
            Cfg::SERVO_PINS[index] == Cfg::PCF_UNUSED_PIN ||
            !_servos[index].attached()
        ) {
            return false;
        }

        _servos[index].write(
            locked
                ? Cfg::SERVO_LOCK_ANGLE
                : Cfg::SERVO_UNLOCK_ANGLE
        );

        ok = true;
    }

    // --------------------------------------------------------
    // PCF8575 DIGITAL OUTPUT ACTUATOR
    // Reserved for future use.
    // --------------------------------------------------------

    else if (
        Cfg::LOCKER_ACTUATOR[index] == Cfg::ACTUATOR_PCF8575
    ) {
        const uint8_t bit = Cfg::EXPANDER_BITS[index];

        if (bit < 8 || bit > 15) {
            return false;
        }

        const bool high = locked
            ? !Cfg::EXPANDER_UNLOCK_HIGH[index]
            : Cfg::EXPANDER_UNLOCK_HIGH[index];

        ok = setExpansionOutput(bit, high);
    }

    if (ok) {
        beep();
    }

    return ok;
}

// ============================================================
// PCF8575 DIGITAL OUTPUTS
// Uses P8-P15 of the SAME keypad PCF8575.
// ============================================================

bool Hardware::setExpansionOutput(uint8_t bit, bool high) {
    if (!_pcfReady || bit < 8 || bit > 15) {
        return false;
    }

    const uint16_t mask =
        static_cast<uint16_t>(1U << bit);

    if (high) {
        _pcfOutputState |= mask;
    } else {
        _pcfOutputState &= static_cast<uint16_t>(~mask);
    }

    return pcfWrite(0xFFFF);
}

// ============================================================
// LED AND PASSIVE BUZZER
// ============================================================

void Hardware::setLed(bool on) {
    digitalWrite(Cfg::LED_PIN, on ? HIGH : LOW);
}

void Hardware::beep(uint16_t durationMs) {
    if (!Cfg::BUZZER_ENABLED || durationMs == 0) {
        return;
    }

    // Passive buzzer requires an oscillating signal.
    tone(
        Cfg::BUZZER_PIN,
        Cfg::BUZZER_TONE_HZ,
        durationMs
    );
}

// ============================================================
// TFT USER INTERFACE
// ============================================================

void Hardware::drawHome(
    bool online,
    uint8_t available,
    const char* timeText
) {
    if (!_displayReady) {
        return;
    }

    _tft.fillScreen(ST77XX_BLACK);
    _tft.setTextSize(1);

    _tft.setTextColor(ST77XX_CYAN);
    _tft.setCursor(3, 5);
    _tft.print(F("SMART LOCKER"));

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(3, 27);
    _tft.print(timeText);

    _tft.setCursor(3, 52);
    _tft.print(F("POWER: ON"));

    _tft.setCursor(3, 70);
    _tft.print(F("NETWORK: "));

    _tft.setTextColor(
        online ? ST77XX_GREEN : ST77XX_YELLOW
    );

    _tft.print(
        online ? F("ONLINE") : F("OFFLINE")
    );

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(3, 94);
    _tft.print(F("AVAILABLE: "));

    _tft.setTextColor(ST77XX_CYAN);
    _tft.print(available);

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(3, 122);
    _tft.print(F("Press any key"));
}

void Hardware::updateHome(
    bool online,
    uint8_t available,
    const char* timeText
) {
    if (!_displayReady) {
        return;
    }

    _tft.fillRect(0, 25, 128, 18, ST77XX_BLACK);

    _tft.setTextSize(1);
    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(3, 27);
    _tft.print(timeText);

    _tft.fillRect(0, 68, 128, 18, ST77XX_BLACK);

    _tft.setCursor(3, 70);
    _tft.print(F("NETWORK: "));

    _tft.setTextColor(
        online ? ST77XX_GREEN : ST77XX_YELLOW
    );

    _tft.print(
        online ? F("ONLINE") : F("OFFLINE")
    );

    _tft.fillRect(0, 92, 128, 18, ST77XX_BLACK);

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(3, 94);
    _tft.print(F("AVAILABLE: "));

    _tft.setTextColor(ST77XX_CYAN);
    _tft.print(available);
}

void Hardware::drawMenu() {
    if (!_displayReady) {
        return;
    }

    _tft.fillScreen(ST77XX_BLACK);
    _tft.setTextSize(1);

    _tft.setTextColor(ST77XX_CYAN);
    _tft.setCursor(3, 5);
    _tft.print(F("MANUAL ACCESS"));

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(5, 42);
    _tft.print(F("A  Assign locker"));

    _tft.setCursor(5, 68);
    _tft.print(F("B  Unlock locker"));

    _tft.setTextColor(ST77XX_YELLOW);
    _tft.setCursor(5, 122);
    _tft.print(F("C: Back"));
}

void Hardware::drawAuth(bool assigning, bool rfidMode) {
    if (!_displayReady) {
        return;
    }

    (void)rfidMode;

    _tft.fillScreen(ST77XX_BLACK);
    _tft.setTextSize(1);

    _tft.setTextColor(ST77XX_CYAN);
    _tft.setCursor(3, 5);

    _tft.print(
        assigning ? F("ASSIGN LOCKER") : F("UNLOCK LOCKER")
    );

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(5, 48);
    _tft.print(F("A  Use RFID"));

    _tft.setCursor(5, 74);
    _tft.print(F("B  Use PIN"));

    _tft.setTextColor(ST77XX_YELLOW);
    _tft.setCursor(5, 122);
    _tft.print(F("C: Back"));
}

void Hardware::drawPin(
    bool assigning,
    const String& maskedPin
) {
    if (!_displayReady) {
        return;
    }

    _tft.fillScreen(ST77XX_BLACK);
    _tft.setTextSize(1);

    _tft.setTextColor(ST77XX_CYAN);
    _tft.setCursor(3, 5);

    _tft.print(
        assigning ? F("CREATE PIN") : F("ENTER PIN")
    );

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setTextSize(2);
    _tft.setCursor(5, 54);
    _tft.print(maskedPin);

    _tft.setTextSize(1);
    _tft.setCursor(3, 102);
    _tft.print(F("#: OK  *: DEL  C: BACK"));
}

void Hardware::drawWaitRfid(bool assigning) {
    if (!_displayReady) {
        return;
    }

    _tft.fillScreen(ST77XX_BLACK);
    _tft.setTextSize(1);

    _tft.setTextColor(ST77XX_CYAN);
    _tft.setCursor(3, 5);

    _tft.print(
        assigning ? F("ASSIGN RFID") : F("UNLOCK RFID")
    );

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(5, 55);
    _tft.print(F("Present card"));

    _tft.setTextColor(ST77XX_YELLOW);
    _tft.setCursor(5, 122);
    _tft.print(F("C: Cancel"));
}

void Hardware::drawResult(
    const String& first,
    const String& second
) {
    if (!_displayReady) {
        return;
    }

    _tft.fillScreen(ST77XX_BLACK);
    _tft.setTextSize(1);

    _tft.setTextColor(ST77XX_CYAN);
    _tft.setCursor(3, 38);
    _tft.println(first);

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(3, 65);
    _tft.println(second);

    _tft.setTextColor(ST77XX_YELLOW);
    _tft.setCursor(3, 122);
    _tft.print(F("Press any key"));
}

void Hardware::drawDoorState(
    uint8_t index,
    bool closed
) {
    if (!_displayReady) {
        return;
    }

    _tft.fillRect(0, 110, 128, 16, ST77XX_BLACK);

    _tft.setCursor(3, 112);

    _tft.setTextColor(
        closed ? ST77XX_GREEN : ST77XX_YELLOW
    );

    _tft.print(F("L"));
    _tft.print(index + 1);
    _tft.print(F(" DOOR: "));
    _tft.print(closed ? F("CLOSED") : F("OPEN"));
}
