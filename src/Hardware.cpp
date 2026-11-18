#include "App.h"

namespace {

constexpr uint16_t COLOR_TOPBAR = 0x18E3;
constexpr uint16_t COLOR_CARD   = 0x10A2;
constexpr uint16_t COLOR_ACCENT = 0x05FF;
constexpr uint16_t COLOR_WARN   = 0xFD20;
constexpr uint16_t COLOR_ERR    = 0xF800;
constexpr uint16_t COLOR_OK     = 0x07E0;
constexpr uint8_t  BUZZER_LEDC_CH = 7;

} // namespace

bool Hardware::pcfWrite(uint16_t value) {
    if (!_i2cReady) return false;
    const uint16_t output = static_cast<uint16_t>(value & 0x00FFU) | 0xFF00U;
    Wire.beginTransmission(_pcfAddress);
    Wire.write(static_cast<uint8_t>(output));
    Wire.write(static_cast<uint8_t>(output >> 8));
    return Wire.endTransmission() == 0;
}

bool Hardware::pcfRead(uint16_t& value) {
    if (!_pcfReady) return false;
    if (Wire.requestFrom(static_cast<int>(_pcfAddress), 2) != 2) return false;
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    value = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
    return true;
}

bool Hardware::begin() {
    Wire.begin(Cfg::I2C_SDA, Cfg::I2C_SCL);
    Wire.setClock(100000);
    _i2cReady = true;

    for (uint8_t addr = Cfg::PCF8575_ADDRESS_FIRST; addr <= Cfg::PCF8575_ADDRESS_LAST; ++addr) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            _pcfAddress = addr;
            _pcfReady = true;
            break;
        }
    }

    if (_pcfReady) {
        pcfWrite(0xFFFF);
    }

    pinMode(Cfg::LED_PIN, OUTPUT);
    digitalWrite(Cfg::LED_PIN, LOW);

    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);

    if (Cfg::BUZZER_ENABLED) {
        ledcSetup(BUZZER_LEDC_CH, Cfg::BUZZER_TONE_HZ, 8);
        ledcAttachPin(Cfg::BUZZER_PIN, BUZZER_LEDC_CH);
        ledcWriteTone(BUZZER_LEDC_CH, 0);
    }

    pinMode(Cfg::RFID_CS, OUTPUT);
    digitalWrite(Cfg::RFID_CS, HIGH);
    pinMode(Cfg::TFT_CS, OUTPUT);
    digitalWrite(Cfg::TFT_CS, HIGH);

    SPI.begin(Cfg::SPI_SCK, Cfg::SPI_MISO, Cfg::SPI_MOSI);

    if (Cfg::TFT_INIT_TAB == 1) _tft.initR(INITR_GREENTAB);
    else if (Cfg::TFT_INIT_TAB == 2) _tft.initR(INITR_REDTAB);
    else _tft.initR(INITR_BLACKTAB);

    _tft.setRotation(Cfg::TFT_ROTATION);
    _tft.setTextWrap(false);
    _displayReady = true;

    pinMode(5, OUTPUT);
    digitalWrite(5, HIGH);

    pinMode(Cfg::RFID_RST, OUTPUT);
    digitalWrite(Cfg::RFID_RST, HIGH);
    _rfid.PCD_Init();
    _rfid.PCD_SetAntennaGain(MFRC522Constants::RxGain_max);
    const byte rfidVer = static_cast<byte>(_rfid.PCD_GetVersion());
    _rfidReady = (rfidVer != 0x00 && rfidVer != 0xFF);

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (Cfg::REED_PINS[i] != Cfg::NO_PIN) {
            pinMode(Cfg::REED_PINS[i], INPUT_PULLUP);
            const bool closed = digitalRead(Cfg::REED_PINS[i]) == Cfg::REED_CLOSED_LEVEL;
            _doorRaw[i] = closed;
            _doorStable[i] = closed;
            _doorChangedAt[i] = millis();
        }

        if (Cfg::SERVO_PINS[i] != Cfg::NO_PIN) {
            _servos[i].setPeriodHertz(50);
            _servos[i].attach(Cfg::SERVO_PINS[i], Cfg::SERVO_MIN_US, Cfg::SERVO_MAX_US);
            _servos[i].write(Cfg::SERVO_LOCK_ANGLE);
        }
    }

    Serial.printf("PCF8575:%s(0x%02X) | MFRC522:%s(0x%02X) | ST7735:%s\n",
                  _pcfReady ? "ok" : "missing", _pcfAddress,
                  _rfidReady ? "ok" : "fail", rfidVer,
                  _displayReady ? "ok" : "fail");
    return true;
}

char Hardware::scanKey() {
    if (!_pcfReady) return 0;
    for (uint8_t row = 0; row < 4; ++row) {
        const uint16_t drive = static_cast<uint16_t>(0xFFFFU & ~(1U << row));
        if (!pcfWrite(drive)) return 0;
        delayMicroseconds(50);
        uint16_t bits = 0xFFFF;
        if (!pcfRead(bits)) return 0;
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

bool Hardware::readCard(String& uid) {
    if (!_rfidReady || millis() - _lastRfidMs < Cfg::RFID_SCAN_MS) return false;
    _lastRfidMs = millis();

    if (!_rfid.PICC_IsNewCardPresent() || !_rfid.PICC_ReadCardSerial()) return false;

    static const char hex[] = "0123456789ABCDEF";
    char uidBuffer[21] = {};
    const byte uidSize = min(static_cast<byte>(_rfid.uid.size), static_cast<byte>(10));

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

bool Hardware::doorClosed(uint8_t index) {
    if (index >= Cfg::LOCKER_COUNT || Cfg::REED_PINS[index] == Cfg::NO_PIN) {
        return true;
    }
    return digitalRead(Cfg::REED_PINS[index]) == Cfg::REED_CLOSED_LEVEL;
}

bool Hardware::doorChanged(uint8_t index, bool& closed) {
    if (index >= Cfg::LOCKER_COUNT || Cfg::REED_PINS[index] == Cfg::NO_PIN) {
        return false;
    }
    const uint32_t now = millis();
    const bool raw = doorClosed(index);

    if (raw != _doorRaw[index]) {
        _doorRaw[index] = raw;
        _doorChangedAt[index] = now;
    }

    if (raw != _doorStable[index] && now - _doorChangedAt[index] >= Cfg::REED_DEBOUNCE_MS) {
        _doorStable[index] = raw;
        closed = raw;
        return true;
    }
    return false;
}

bool Hardware::setLockerActuator(uint8_t index, bool locked) {
    if (index >= Cfg::LOCKER_COUNT || Cfg::SERVO_PINS[index] == Cfg::NO_PIN) return false;
    if (!_servos[index].attached()) return false;
    _servos[index].write(locked ? Cfg::SERVO_LOCK_ANGLE : Cfg::SERVO_UNLOCK_ANGLE);
    beep();
    return true;
}

bool Hardware::setServoAngle(uint8_t index, int angle) {
    if (index >= Cfg::LOCKER_COUNT || Cfg::SERVO_PINS[index] == Cfg::NO_PIN) return false;
    if (!_servos[index].attached()) return false;
    const int clamped = constrain(angle, 0, 180);
    _servos[index].write(clamped);
    Serial.printf("[SERVO] %s (GPIO %u) -> %d deg\n",
                  Cfg::LOCKER_IDS[index], Cfg::SERVO_PINS[index], clamped);
    return true;
}

void Hardware::setLed(bool on) {
    digitalWrite(Cfg::LED_PIN, on ? HIGH : LOW);
}

void Hardware::beep(uint16_t durationMs) {
    if (!Cfg::BUZZER_ENABLED || durationMs == 0) return;
    ledcWriteTone(BUZZER_LEDC_CH, Cfg::BUZZER_TONE_HZ);
    delay(durationMs);
    ledcWriteTone(BUZZER_LEDC_CH, 0);
}

// ============================================================
// GRAPHIC TFT UI (128x140)
// ============================================================

void Hardware::drawConnectivityIcon(int16_t x, int16_t y, bool wifiOk, bool mqttOk) {
    uint16_t color = COLOR_OK;
    if (!wifiOk) color = COLOR_ERR;
    else if (!mqttOk) color = COLOR_WARN;

    _tft.fillRect(x,      y + 6, 3, 4, color);
    _tft.fillRect(x + 5,  y + 3, 3, 7, color);
    _tft.fillRect(x + 10, y,     3, 10, color);

    if (!wifiOk) {
        _tft.drawLine(x - 1, y + 10, x + 13, y - 1, COLOR_ERR);
        _tft.drawLine(x,     y + 10, x + 14, y - 1, COLOR_ERR);
    } else if (!mqttOk) {
        _tft.fillRect(x + 15, y,     2, 6, COLOR_WARN);
        _tft.fillRect(x + 15, y + 8, 2, 2, COLOR_WARN);
    }
}

void Hardware::drawTopBar(bool wifiOk, bool mqttOk, const char* title) {
    if (!_displayReady) return;
    _tft.fillRect(0, 0, Cfg::TFT_WIDTH, 17, COLOR_TOPBAR);
    _tft.drawFastHLine(0, 17, Cfg::TFT_WIDTH, COLOR_ACCENT);

    _tft.setTextSize(1);
    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(4, 5);
    _tft.print(title);

    drawConnectivityIcon(106, 3, wifiOk, mqttOk);
}

void Hardware::drawHome(bool wifiOk, bool mqttOk, uint8_t available, bool door1Closed) {
    if (!_displayReady) return;
    _tft.fillScreen(ST77XX_BLACK);
    drawTopBar(wifiOk, mqttOk, "SMART LOCKER");

    int16_t cursorY = 22;

    if (!wifiOk) {
        _tft.fillRoundRect(4, cursorY, 120, 14, 3, COLOR_ERR);
        _tft.setTextColor(ST77XX_WHITE);
        _tft.setTextSize(1);
        _tft.setCursor(43, cursorY + 3);
        _tft.print(F("Offline"));
        cursorY += 17;
    } else if (!mqttOk) {
        _tft.fillRoundRect(4, cursorY, 120, 14, 3, COLOR_WARN);
        _tft.setTextColor(ST77XX_BLACK);
        _tft.setTextSize(1);
        _tft.setCursor(10, cursorY + 3);
        _tft.print(F("Server disconnected"));
        cursorY += 17;
    }

    const int16_t cardH = (wifiOk && mqttOk) ? 68 : 54;
    _tft.fillRoundRect(6, cursorY, 116, cardH, 6, COLOR_CARD);
    _tft.drawRoundRect(6, cursorY, 116, cardH, 6, COLOR_ACCENT);

    _tft.setTextSize(1);
    _tft.setTextColor(COLOR_ACCENT);
    _tft.setCursor(14, cursorY + 7);
    _tft.print(F("AVAILABLE LOCKERS"));

    _tft.setTextSize(2);
    _tft.setTextColor(available > 0 ? COLOR_OK : COLOR_ERR);
    _tft.setCursor(42, cursorY + ((wifiOk && mqttOk) ? 24 : 19));
    _tft.print(available);
    _tft.setTextColor(ST77XX_WHITE);
    _tft.print(F("/"));
    _tft.print(Cfg::LOCKER_COUNT);

    // Live Reed Switch Status for Locker 1 (GPIO 14)
    _tft.setTextSize(1);
    _tft.setTextColor(door1Closed ? COLOR_OK : COLOR_WARN);
    _tft.setCursor(34, cursorY + cardH - 12);
    _tft.print(door1Closed ? F("D1:CLOSED") : F("D1:OPEN"));

    const int16_t btnY = 114;
    _tft.fillRoundRect(4, btnY, 58, 22, 4, COLOR_ACCENT);
    _tft.setTextSize(1);
    _tft.setTextColor(ST77XX_BLACK);
    _tft.setCursor(9, btnY + 7);
    _tft.print(F("A:Assign"));

    _tft.fillRoundRect(66, btnY, 58, 22, 4, COLOR_OK);
    _tft.setTextColor(ST77XX_BLACK);
    _tft.setCursor(71, btnY + 7);
    _tft.print(F("B:Unlock"));
}

void Hardware::drawPinUnderlineBoxes(uint8_t pinLength) {
    _tft.fillRect(4, 62, 120, 26, ST77XX_BLACK);
    const int16_t startX = 8;
    const int16_t slotW = 12;
    const int16_t gap = 2;

    for (uint8_t i = 0; i < Cfg::PIN_MAX_LENGTH; ++i) {
        const int16_t x = startX + i * (slotW + gap);
        const uint16_t lineColor = (i < pinLength) ? COLOR_OK : COLOR_ACCENT;
        _tft.drawFastHLine(x, 82, slotW, lineColor);
        _tft.drawFastHLine(x, 83, slotW, lineColor);

        if (i < pinLength) {
            _tft.setTextSize(2);
            _tft.setTextColor(ST77XX_WHITE);
            _tft.setCursor(x + 1, 65);
            _tft.print(F("*"));
        }
    }
}

void Hardware::drawCredentialPrompt(bool wifiOk, bool mqttOk, bool assigning, uint8_t pinLength) {
    if (!_displayReady) return;
    _tft.fillScreen(ST77XX_BLACK);
    drawTopBar(wifiOk, mqttOk, "SMART LOCKER");

    _tft.setTextSize(1);
    _tft.setTextColor(assigning ? COLOR_ACCENT : COLOR_OK);
    _tft.setCursor(22, 23);
    _tft.print(assigning ? F("ASSIGN LOCKER") : F("UNLOCK LOCKER"));

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(6, 40);
    _tft.print(F("Set PIN or tap RFID"));

    drawPinUnderlineBoxes(pinLength);

    _tft.setTextSize(1);
    _tft.setTextColor(ST77XX_YELLOW);
    _tft.setCursor(6, 102);
    _tft.print(F("*:Del   #:OK   C:Back"));
}

void Hardware::updateCredentialPin(uint8_t pinLength) {
    if (!_displayReady) return;
    drawPinUnderlineBoxes(pinLength);
}

void Hardware::drawResult(bool wifiOk, bool mqttOk,
                          const String& title, const String& detail, bool isError) {
    if (!_displayReady) return;
    _tft.fillScreen(ST77XX_BLACK);
    drawTopBar(wifiOk, mqttOk, "SMART LOCKER");

    const uint16_t border = isError ? COLOR_ERR : COLOR_OK;
    _tft.fillRoundRect(6, 30, 116, 82, 6, COLOR_CARD);
    _tft.drawRoundRect(6, 30, 116, 82, 6, border);

    _tft.setTextSize(1);
    _tft.setTextColor(border);
    _tft.setCursor(12, 46);
    _tft.print(title);

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(12, 72);
    _tft.print(detail);
}