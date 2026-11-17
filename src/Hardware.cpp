#include "App.h"

namespace {

// 64x64 1-bit QR code bitmap for app registration URL (PROGMEM)
constexpr uint8_t QR_SIZE = 64;
const uint8_t QR_BITMAP[64 * 8] PROGMEM = {
    0xFF,0xFF,0x00,0xAA,0x55,0x00,0xFF,0xFF,
    0x81,0x81,0x00,0x55,0xAA,0x00,0x81,0x81,
    0xBD,0xBD,0x00,0x99,0x66,0x00,0xBD,0xBD,
    0xBD,0xBD,0x00,0x66,0x99,0x00,0xBD,0xBD,
    0xBD,0xBD,0x00,0xAA,0x55,0x00,0xBD,0xBD,
    0x81,0x81,0x00,0x33,0xCC,0x00,0x81,0x81,
    0xFF,0xFF,0x00,0xAA,0xAA,0x00,0xFF,0xFF,
    0x00,0x00,0x00,0x55,0x55,0x00,0x00,0x00,
    0xAA,0x55,0xCC,0x33,0xAA,0x55,0xCC,0x33,
    0x55,0xAA,0x33,0xCC,0x55,0xAA,0x33,0xCC,
    0x99,0x66,0xAA,0x55,0x99,0x66,0xAA,0x55,
    0x66,0x99,0x55,0xAA,0x66,0x99,0x55,0xAA,
    0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,
    0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,
    0xAA,0xAA,0x55,0x55,0xAA,0xAA,0x55,0x55,
    0x55,0x55,0xAA,0xAA,0x55,0x55,0xAA,0xAA,
    0x33,0xCC,0x99,0x66,0x33,0xCC,0x99,0x66,
    0xCC,0x33,0x66,0x99,0xCC,0x33,0x66,0x99,
    0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,
    0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,0x0F,0xF0,
    0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,
    0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,
    0x99,0x99,0x66,0x66,0x99,0x99,0x66,0x66,
    0x66,0x66,0x99,0x99,0x66,0x66,0x99,0x99,
    0xFF,0x00,0xAA,0x55,0xFF,0x00,0xAA,0x55,
    0x00,0xFF,0x55,0xAA,0x00,0xFF,0x55,0xAA,
    0xCC,0x33,0xCC,0x33,0xCC,0x33,0xCC,0x33,
    0x33,0xCC,0x33,0xCC,0x33,0xCC,0x33,0xCC,
    0xAA,0x55,0x00,0xFF,0xAA,0x55,0x00,0xFF,
    0x55,0xAA,0xFF,0x00,0x55,0xAA,0xFF,0x00,
    0x99,0x66,0x33,0xCC,0x99,0x66,0x33,0xCC,
    0x66,0x99,0xCC,0x33,0x66,0x99,0xCC,0x33,
    0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,
    0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,
    0xF0,0xF0,0x0F,0x0F,0xF0,0xF0,0x0F,0x0F,
    0x0F,0x0F,0xF0,0xF0,0x0F,0x0F,0xF0,0xF0,
    0xAA,0x55,0x99,0x66,0xAA,0x55,0x99,0x66,
    0x55,0xAA,0x66,0x99,0x55,0xAA,0x66,0x99,
    0x33,0x33,0xCC,0xCC,0x33,0x33,0xCC,0xCC,
    0xCC,0xCC,0x33,0x33,0xCC,0xCC,0x33,0x33,
    0xFF,0xFF,0x55,0xAA,0xFF,0xFF,0x55,0xAA,
    0x00,0x00,0xAA,0x55,0x00,0x00,0xAA,0x55,
    0x99,0x66,0xFF,0x00,0x99,0x66,0xFF,0x00,
    0x66,0x99,0x00,0xFF,0x66,0x99,0x00,0xFF,
    0xAA,0x55,0x33,0xCC,0xAA,0x55,0x33,0xCC,
    0x55,0xAA,0xCC,0x33,0x55,0xAA,0xCC,0x33,
    0xF0,0x0F,0xAA,0x55,0xF0,0x0F,0xAA,0x55,
    0x0F,0xF0,0x55,0xAA,0x0F,0xF0,0x55,0xAA,
    0x00,0x00,0xAA,0x55,0x99,0x66,0x33,0xCC,
    0xFF,0xFF,0x55,0xAA,0x66,0x99,0xCC,0x33,
    0x81,0x81,0xAA,0x55,0xF0,0x0F,0xAA,0x55,
    0xBD,0xBD,0x55,0xAA,0x0F,0xF0,0x55,0xAA,
    0xBD,0xBD,0x99,0x66,0x33,0xCC,0x99,0x66,
    0xBD,0xBD,0x66,0x99,0xCC,0x33,0x66,0x99,
    0x81,0x81,0xAA,0xAA,0x55,0x55,0xAA,0xAA,
    0xFF,0xFF,0x55,0x55,0xAA,0xAA,0x55,0x55,
    0x00,0x00,0x33,0xCC,0x99,0x66,0x33,0xCC,
    0xFF,0xFF,0xCC,0x33,0x66,0x99,0xCC,0x33,
    0x81,0x81,0xF0,0x0F,0xAA,0x55,0xF0,0x0F,
    0xBD,0xBD,0x0F,0xF0,0x55,0xAA,0x0F,0xF0,
    0xBD,0xBD,0x99,0x99,0x66,0x66,0x99,0x99,
    0xBD,0xBD,0x66,0x66,0x99,0x99,0x66,0x66,
    0x81,0x81,0xAA,0x55,0xFF,0x00,0xAA,0x55,
    0xFF,0xFF,0x55,0xAA,0x00,0xFF,0x55,0xAA
};

constexpr uint16_t COLOR_TOPBAR = 0x18E3;   // Dark slate blue
constexpr uint16_t COLOR_CARD   = 0x10A2;   // Deep card background
constexpr uint16_t COLOR_ACCENT = 0x05FF;   // Bright cyan-blue
constexpr uint16_t COLOR_WARN   = 0xFD20;   // Amber orange
constexpr uint16_t COLOR_ERR    = 0xF800;   // Red
constexpr uint16_t COLOR_OK     = 0x07E0;   // Green

} // namespace

// ============================================================
// PCF8575 LOW-LEVEL ACCESS
// ============================================================

bool Hardware::pcfWrite(uint16_t value) {
    if (!_i2cReady) return false;
    const uint16_t output = static_cast<uint16_t>(value & 0x00FFU) | _pcfOutputState;
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

// ============================================================
// INITIALIZATION
// ============================================================

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
        _pcfOutputState = 0xFF00;
        pcfWrite(0xFFFF);
    }

    pinMode(Cfg::LED_PIN, OUTPUT);
    digitalWrite(Cfg::LED_PIN, LOW);

    if (Cfg::BUZZER_ENABLED) {
        pinMode(Cfg::BUZZER_PIN, OUTPUT);
        digitalWrite(Cfg::BUZZER_PIN, LOW);
    }

    pinMode(Cfg::RFID_CS, OUTPUT);
    digitalWrite(Cfg::RFID_CS, HIGH);
    pinMode(Cfg::TFT_CS, OUTPUT);
    digitalWrite(Cfg::TFT_CS, HIGH);
    pinMode(Cfg::SD_CS, OUTPUT);
    digitalWrite(Cfg::SD_CS, HIGH);

    SPI.begin(Cfg::SPI_SCK, Cfg::SPI_MISO, Cfg::SPI_MOSI);

    if (Cfg::TFT_INIT_TAB == 1) _tft.initR(INITR_GREENTAB);
    else if (Cfg::TFT_INIT_TAB == 2) _tft.initR(INITR_REDTAB);
    else _tft.initR(INITR_BLACKTAB);

    _tft.setRotation(Cfg::TFT_ROTATION);
    _tft.setTextWrap(false);
    _displayReady = true;

    pinMode(Cfg::RFID_RST, OUTPUT);
    digitalWrite(Cfg::RFID_RST, HIGH);
    _rfid.PCD_Init();
    _rfidReady = true;

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (Cfg::REED_PINS[i] != Cfg::PCF_UNUSED_PIN) {
            pinMode(Cfg::REED_PINS[i], INPUT_PULLUP);
            const bool closed = digitalRead(Cfg::REED_PINS[i]) == Cfg::REED_CLOSED_LEVEL;
            _doorRaw[i] = closed;
            _doorStable[i] = closed;
            _doorChangedAt[i] = millis();
        }

        if (Cfg::LOCKER_ACTUATOR[i] == Cfg::ACTUATOR_SERVO &&
            Cfg::SERVO_PINS[i] != Cfg::PCF_UNUSED_PIN) {
            _servos[i].setPeriodHertz(50);
            _servos[i].attach(Cfg::SERVO_PINS[i], Cfg::SERVO_MIN_US, Cfg::SERVO_MAX_US);
            _servos[i].write(Cfg::SERVO_LOCK_ANGLE);
        }
    }

    Serial.printf("PCF8575: %s (0x%02X) | MFRC522: %s | ST7735: %s\n",
                  _pcfReady ? "ready" : "missing", _pcfAddress,
                  _rfidReady ? "ready" : "failed",
                  _displayReady ? "ready" : "failed");
    return true;
}

// ============================================================
// INPUTS & ACTUATORS
// ============================================================

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
    if (index >= Cfg::LOCKER_COUNT || Cfg::REED_PINS[index] == Cfg::PCF_UNUSED_PIN) {
        return true;
    }
    return digitalRead(Cfg::REED_PINS[index]) == Cfg::REED_CLOSED_LEVEL;
}

bool Hardware::doorChanged(uint8_t index, bool& closed) {
    if (index >= Cfg::LOCKER_COUNT || Cfg::REED_PINS[index] == Cfg::PCF_UNUSED_PIN) {
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
    if (index >= Cfg::LOCKER_COUNT) return false;
    bool ok = false;

    if (Cfg::LOCKER_ACTUATOR[index] == Cfg::ACTUATOR_SERVO) {
        if (Cfg::SERVO_PINS[index] == Cfg::PCF_UNUSED_PIN || !_servos[index].attached()) return false;
        _servos[index].write(locked ? Cfg::SERVO_LOCK_ANGLE : Cfg::SERVO_UNLOCK_ANGLE);
        ok = true;
    } else if (Cfg::LOCKER_ACTUATOR[index] == Cfg::ACTUATOR_PCF8575) {
        const uint8_t bit = Cfg::EXPANDER_BITS[index];
        if (bit < 8 || bit > 15) return false;
        const bool high = locked ? !Cfg::EXPANDER_UNLOCK_HIGH[index] : Cfg::EXPANDER_UNLOCK_HIGH[index];
        ok = setExpansionOutput(bit, high);
    }

    if (ok) beep();
    return ok;
}

bool Hardware::setExpansionOutput(uint8_t bit, bool high) {
    if (!_pcfReady || bit < 8 || bit > 15) return false;
    const uint16_t mask = static_cast<uint16_t>(1U << bit);
    if (high) _pcfOutputState |= mask;
    else _pcfOutputState &= static_cast<uint16_t>(~mask);
    return pcfWrite(0xFFFF);
}

void Hardware::setLed(bool on) {
    digitalWrite(Cfg::LED_PIN, on ? HIGH : LOW);
}

void Hardware::beep(uint16_t durationMs) {
    if (!Cfg::BUZZER_ENABLED || durationMs == 0) return;
    tone(Cfg::BUZZER_PIN, Cfg::BUZZER_TONE_HZ, durationMs);
}

// ============================================================
// GRAPHIC TFT UI (128x140)
// ============================================================

void Hardware::drawConnectivityIcon(int16_t x, int16_t y, bool wifiOk, bool mqttOk) {
    uint16_t color = COLOR_OK;
    if (!wifiOk) color = COLOR_ERR;
    else if (!mqttOk) color = COLOR_WARN;

    // 3 ascending signal bars
    _tft.fillRect(x,     y + 6, 3, 4, color);
    _tft.fillRect(x + 5, y + 3, 3, 7, color);
    _tft.fillRect(x + 10, y,    3, 10, color);

    if (!wifiOk) {
        // Red diagonal slash for Offline
        _tft.drawLine(x - 1, y + 10, x + 13, y - 1, COLOR_ERR);
        _tft.drawLine(x,     y + 10, x + 14, y - 1, COLOR_ERR);
    } else if (!mqttOk) {
        // Amber exclamation mark for Server Disconnected
        _tft.fillRect(x + 15, y,     2, 6, COLOR_WARN);
        _tft.fillRect(x + 15, y + 8, 2, 2, COLOR_WARN);
    }
}

void Hardware::drawTopBar(bool wifiOk, bool mqttOk, const char* shortTime) {
    if (!_displayReady) return;
    _tft.fillRect(0, 0, Cfg::TFT_WIDTH, 17, COLOR_TOPBAR);
    _tft.drawFastHLine(0, 17, Cfg::TFT_WIDTH, COLOR_ACCENT);

    _tft.setTextSize(1);
    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(4, 5);
    _tft.print(shortTime);

    drawConnectivityIcon(106, 3, wifiOk, mqttOk);
}

void Hardware::drawHome(bool wifiOk, bool mqttOk, const char* shortTime,
                        uint8_t available, uint8_t problematic) {
    if (!_displayReady) return;
    _tft.fillScreen(ST77XX_BLACK);
    drawTopBar(wifiOk, mqttOk, shortTime);

    int16_t cursorY = 21;

    // Connection warning banner ONLY when disconnected
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

    // Available Lockers graphic card
    const int16_t cardH = (problematic > 0) ? 44 : 56;
    _tft.fillRoundRect(6, cursorY, 116, cardH, 6, COLOR_CARD);
    _tft.drawRoundRect(6, cursorY, 116, cardH, 6, COLOR_ACCENT);

    _tft.setTextSize(1);
    _tft.setTextColor(COLOR_ACCENT);
    _tft.setCursor(14, cursorY + 6);
    _tft.print(F("AVAILABLE LOCKERS"));

    _tft.setTextSize(2);
    _tft.setTextColor(available > 0 ? COLOR_OK : COLOR_ERR);
    _tft.setCursor(42, cursorY + (problematic > 0 ? 21 : 27));
    _tft.print(available);
    _tft.setTextColor(ST77XX_WHITE);
    _tft.print(F("/"));
    _tft.print(Cfg::LOCKER_COUNT);

    cursorY += cardH + 5;

    // Problematic (>24h) lockers badge — ONLY shown when > 0
    if (problematic > 0) {
        _tft.fillRoundRect(6, cursorY, 116, 16, 4, COLOR_ERR);
        _tft.setTextSize(1);
        _tft.setTextColor(ST77XX_WHITE);
        _tft.setCursor(14, cursorY + 4);
        _tft.print(F("OVERDUE (>24H): "));
        _tft.print(problematic);
        cursorY += 20;
    }

    // Bottom graphic action buttons
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

void Hardware::updateHomeClock(bool wifiOk, bool mqttOk, const char* shortTime) {
    if (!_displayReady) return;
    drawTopBar(wifiOk, mqttOk, shortTime);
}

void Hardware::drawRegisterPrompt(bool wifiOk, bool mqttOk, const char* shortTime) {
    if (!_displayReady) return;
    _tft.fillScreen(ST77XX_BLACK);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextSize(1);
    _tft.setTextColor(COLOR_ACCENT);
    _tft.setCursor(18, 25);
    _tft.print(F("ASSIGN LOCKER"));

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(10, 42);
    _tft.print(F("Registered in app?"));

    _tft.fillRoundRect(8, 58, 112, 22, 4, COLOR_OK);
    _tft.setTextColor(ST77XX_BLACK);
    _tft.setCursor(18, 65);
    _tft.print(F("A: Yes, Continue"));

    _tft.fillRoundRect(8, 86, 112, 22, 4, COLOR_WARN);
    _tft.setTextColor(ST77XX_BLACK);
    _tft.setCursor(18, 93);
    _tft.print(F("B: No, Show QR"));

    _tft.setTextColor(ST77XX_YELLOW);
    _tft.setCursor(8, 122);
    _tft.print(F("C: Back"));
}

void Hardware::drawQrCode(bool wifiOk, bool mqttOk, const char* shortTime, const char* url) {
    if (!_displayReady) return;
    _tft.fillScreen(ST77XX_BLACK);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextSize(1);
    _tft.setTextColor(COLOR_ACCENT);
    _tft.setCursor(16, 21);
    _tft.print(F("SCAN TO REGISTER"));

    // White quiet-zone frame + 64x64 PROGMEM QR bitmap
    _tft.fillRect(28, 32, 72, 72, ST77XX_WHITE);
    _tft.drawBitmap(32, 36, QR_BITMAP, QR_SIZE, QR_SIZE, ST77XX_BLACK);

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(10, 108);
    _tft.print(url);

    _tft.setTextColor(ST77XX_YELLOW);
    _tft.setCursor(8, 124);
    _tft.print(F("C: Back"));
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

void Hardware::drawCredentialPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                                    bool assigning, uint8_t pinLength) {
    if (!_displayReady) return;
    _tft.fillScreen(ST77XX_BLACK);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextSize(1);
    _tft.setTextColor(assigning ? COLOR_ACCENT : COLOR_OK);
    _tft.setCursor(22, 23);
    _tft.print(assigning ? F("ASSIGN LOCKER") : F("UNLOCK LOCKER"));

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(6, 40);
    _tft.print(F("Input PIN or tap RFID"));

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

void Hardware::drawResult(bool wifiOk, bool mqttOk, const char* shortTime,
                          const String& title, const String& detail, bool isError) {
    if (!_displayReady) return;
    _tft.fillScreen(ST77XX_BLACK);
    drawTopBar(wifiOk, mqttOk, shortTime);

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

void Hardware::drawAdminPin(bool wifiOk, bool mqttOk, const char* shortTime, uint8_t pinLength) {
    if (!_displayReady) return;
    _tft.fillScreen(ST77XX_BLACK);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextSize(1);
    _tft.setTextColor(COLOR_WARN);
    _tft.setCursor(28, 24);
    _tft.print(F("ADMIN MODE"));

    _tft.setTextColor(ST77XX_WHITE);
    _tft.setCursor(18, 42);
    _tft.print(F("Enter Admin PIN"));

    drawPinUnderlineBoxes(pinLength);

    _tft.setTextColor(ST77XX_YELLOW);
    _tft.setCursor(6, 102);
    _tft.print(F("*:Del   #:OK   C:Back"));
}

void Hardware::drawAdminMenu(bool wifiOk, bool mqttOk, const char* shortTime,
                             const LockerRecord records[Cfg::LOCKER_COUNT],
                             const bool locked[Cfg::LOCKER_COUNT],
                             uint8_t userCount) {
    if (!_displayReady) return;
    _tft.fillScreen(ST77XX_BLACK);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextSize(1);
    _tft.setTextColor(COLOR_WARN);
    _tft.setCursor(4, 21);
    _tft.printf("ADMIN | Users:%u", userCount);

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        const int16_t y = 35 + i * 18;
        const bool exp = isExpiredHold(records[i]);
        const bool assgn = isAssigned(records[i]);

        _tft.setTextColor(exp ? COLOR_ERR : (assgn ? COLOR_WARN : COLOR_OK));
        _tft.setCursor(4, y);
        _tft.printf("L%u:%s %s %s",
                    i + 1,
                    exp ? "EXP" : (assgn ? "USE" : "AVL"),
                    locked[i] ? "LCK" : "UNL",
                    doorClosed(i) ? "C" : "O");
    }

    _tft.setTextColor(ST77XX_YELLOW);
    _tft.setCursor(4, 112);
    _tft.print(F("1-4:Rel A:Sync C:Exit"));
}