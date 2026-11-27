#include "App.h"
#include <qrcode.h>

namespace {
constexpr uint16_t C_BG      = 0x0861;
constexpr uint16_t C_PANEL   = 0x10A2;
constexpr uint16_t C_ACCENT  = 0x05FF;
constexpr uint16_t C_GREEN   = 0x2668;
constexpr uint16_t C_AMBER   = 0xFD20;
constexpr uint16_t C_RED     = 0xF9A6;
constexpr uint16_t C_TEXT    = 0xFFFF;
constexpr uint16_t C_MUTED   = 0xAD75;
}

bool Hardware::begin() {
    pinMode(Cfg::LED_PIN, OUTPUT);
    digitalWrite(Cfg::LED_PIN, LOW);

    // Initialize onboard WS2812 RGB LED (GPIO 48)
    rgbLedWrite(Cfg::RGB_LED_PIN, Cfg::RGB_BRIGHTNESS, Cfg::RGB_BRIGHTNESS / 2, 0);

    if (Cfg::BUZZER_ENABLED) {
        pinMode(Cfg::BUZZER_PIN, OUTPUT);
        digitalWrite(Cfg::BUZZER_PIN, LOW);
    }

    // 1. I2C Bus & PCF8575 (P00-P07 keypad + P10-P13 reed switches)
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
        pcfWrite(0xFFFF); // Set all pins high (weak pull-up input state)
        uint16_t word = 0xFFFF;
        if (pcfRead(word)) {
            _pcfLastWord = word;
            for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
                const bool rawClosed = ((word >> Cfg::REED_PCF_BITS[i]) & 0x01) == (Cfg::REED_CLOSED_IS_LOW ? 0 : 1);
                _doorRaw[i] = rawClosed;
                _doorStable[i] = rawClosed;
                _doorChangedAt[i] = millis();
            }
        }
    }

    // 2. Shared SPI Bus (ST7735 TFT + MFRC522 RFID + SD)
    pinMode(Cfg::TFT_CS, OUTPUT);
    digitalWrite(Cfg::TFT_CS, HIGH);
    pinMode(Cfg::RFID_CS, OUTPUT);
    digitalWrite(Cfg::RFID_CS, HIGH);
    pinMode(Cfg::SD_CS, OUTPUT);
    digitalWrite(Cfg::SD_CS, HIGH);

    SPI.begin(Cfg::SPI_SCK, Cfg::SPI_MISO, Cfg::SPI_MOSI);

    _tft.initR(INITR_BLACKTAB);
    _tft.setRotation(Cfg::TFT_ROTATION);
    _tft.fillScreen(C_BG);
    _displayReady = true;

    _rfid.PCD_Init();
    const uint8_t ver = _rfid.PCD_GetVersion();
    _rfidReady = (ver != 0x00 && ver != 0xFF);

    // 3. 4 Adjacent Positional Servos (GPIO 15..18)
    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        _servos[i].setPeriodHertz(50);
        _servos[i].attach(Cfg::SERVO_PINS[i], Cfg::SERVO_MIN_US, Cfg::SERVO_MAX_US);
        _servos[i].write(Cfg::SERVO_LOCK_ANGLE);
    }

    return _pcfReady && _displayReady;
}

bool Hardware::pcfWrite(uint16_t mask) {
    if (!_pcfReady) return false;
    // CRITICAL RULE: Always force bits 8..15 HIGH (0xFF00) so P10..P13 reed pull-ups never drive LOW
    const uint16_t safeWord = 0xFF00 | (mask & 0x00FF);
    Wire.beginTransmission(_pcfAddress);
    Wire.write(static_cast<uint8_t>(safeWord & 0xFF));
    Wire.write(static_cast<uint8_t>((safeWord >> 8) & 0xFF));
    return Wire.endTransmission() == 0;
}

bool Hardware::pcfRead(uint16_t& value) {
    if (!_pcfReady) return false;
    if (Wire.requestFrom(_pcfAddress, static_cast<uint8_t>(2)) != 2) return false;
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    value = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
    _pcfLastWord = value;
    return true;
}

char Hardware::scanKey() {
    if (!_pcfReady) return 0;

    char detected = 0;
    for (uint8_t row = 0; row < 4; ++row) {
        const uint8_t rowMask = static_cast<uint8_t>(0xFF & ~(1U << row));
        if (!pcfWrite(rowMask)) continue;
        delayMicroseconds(60);

        uint16_t word = 0xFFFF;
        if (!pcfRead(word)) continue;

        for (uint8_t col = 0; col < 4; ++col) {
            if ((word & (1U << (4 + col))) == 0) {
                detected = Cfg::KEYMAP[row][col];
                break;
            }
        }
        if (detected) break;
    }

    // Restore idle all-high mask so P10..P13 reed switches remain readable anytime
    pcfWrite(0x00FF);
    uint16_t idleWord = 0xFFFF;
    pcfRead(idleWord);

    return detected;
}

bool Hardware::doorClosed(uint8_t index) {
    if (index >= Cfg::LOCKER_COUNT) return true;
    const uint8_t bit = Cfg::REED_PCF_BITS[index];
    const bool pinLow = ((_pcfLastWord >> bit) & 0x01) == 0;
    const bool rawClosed = Cfg::REED_CLOSED_IS_LOW ? pinLow : !pinLow;

    const uint32_t now = millis();
    if (rawClosed != _doorRaw[index]) {
        _doorRaw[index] = rawClosed;
        _doorChangedAt[index] = now;
    } else if (rawClosed != _doorStable[index] && (now - _doorChangedAt[index] >= Cfg::REED_DEBOUNCE_MS)) {
        _doorStable[index] = rawClosed;
    }
    return _doorStable[index];
}

bool Hardware::doorChanged(uint8_t index, bool& closed) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    const bool prev = _doorStable[index];
    const bool cur = doorClosed(index);
    closed = cur;
    return prev != cur;
}

bool Hardware::readCard(String& uid) {
    if (!_rfidReady) return false;
    const uint32_t now = millis();
    if (now - _lastRfidMs < Cfg::RFID_SCAN_MS) return false;
    _lastRfidMs = now;

    if (!_rfid.PICC_IsNewCardPresent() || !_rfid.PICC_ReadCardSerial()) {
        return false;
    }

    char hexBuf[24];
    uint8_t pos = 0;
    for (byte i = 0; i < _rfid.uid.size && pos + 2 < sizeof(hexBuf); ++i) {
        snprintf(&hexBuf[pos], 3, "%02X", _rfid.uid.uidByte[i]);
        pos += 2;
    }
    hexBuf[pos] = '\0';
    uid = String(hexBuf);

    _rfid.PICC_HaltA();
    _rfid.PCD_StopCrypto1();
    return !uid.isEmpty();
}

uint8_t Hardware::rfidVersionReg() {
    if (!_rfidReady) return 0;
    return _rfid.PCD_GetVersion();
}

bool Hardware::setLockerActuator(uint8_t index, bool locked) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    _servos[index].write(locked ? Cfg::SERVO_LOCK_ANGLE : Cfg::SERVO_UNLOCK_ANGLE);
    return true;
}

bool Hardware::setServoAngleDirect(uint8_t index, int angle) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    const int clamped = constrain(angle, 0, 180);
    _servos[index].write(clamped);
    return true;
}

void Hardware::setOnlineLed(bool on) {
    digitalWrite(Cfg::LED_PIN, on ? HIGH : LOW);
}

void Hardware::setRgbState(RgbState state) {
    if (state == _lastRgb) return;
    _lastRgb = state;
    const uint8_t b = Cfg::RGB_BRIGHTNESS;
    switch (state) {
        case RgbState::ONLINE_IDLE:     rgbLedWrite(Cfg::RGB_LED_PIN, 0, b, 0); break;
        case RgbState::MQTT_CONNECTING: rgbLedWrite(Cfg::RGB_LED_PIN, 0, 0, b); break;
        case RgbState::OFFLINE_LOCAL:   rgbLedWrite(Cfg::RGB_LED_PIN, b, b / 2, 0); break;
        case RgbState::ACTIVE_SESSION:  rgbLedWrite(Cfg::RGB_LED_PIN, 0, b, b); break;
        case RgbState::ADMIN_ACTIVE:    rgbLedWrite(Cfg::RGB_LED_PIN, b, 0, b); break;
        case RgbState::ALERT_ERROR:     rgbLedWrite(Cfg::RGB_LED_PIN, b, 0, 0); break;
    }
}

void Hardware::testRgbCycle() {
    const uint8_t b = Cfg::RGB_BRIGHTNESS;
    rgbLedWrite(Cfg::RGB_LED_PIN, b, 0, 0); delay(120);
    rgbLedWrite(Cfg::RGB_LED_PIN, 0, b, 0); delay(120);
    rgbLedWrite(Cfg::RGB_LED_PIN, 0, 0, b); delay(120);
    RgbState restore = _lastRgb;
    _lastRgb = static_cast<RgbState>(255);
    setRgbState(restore);
}

void Hardware::beep(uint16_t durationMs) {
    if (!Cfg::BUZZER_ENABLED) return;
    tone(Cfg::BUZZER_PIN, Cfg::BUZZER_TONE_HZ, durationMs);
}

// ============================================================
// ST7735 GRAPHIC UI RENDERERS (128x140)
// ============================================================

void Hardware::drawConnectivityIcon(int16_t x, int16_t y, bool wifiOk, bool mqttOk) {
    uint16_t col = C_RED;
    const char* tag = "OFF";
    if (wifiOk && mqttOk) {
        col = C_GREEN;
        tag = "ON";
    } else if (wifiOk) {
        col = C_AMBER;
        tag = "NET";
    }
    _tft.fillRoundRect(x, y, 26, 12, 2, col);
    _tft.setTextColor(C_BG);
    _tft.setTextSize(1);
    _tft.setCursor(x + 4, y + 2);
    _tft.print(tag);
}

void Hardware::drawTopBar(bool wifiOk, bool mqttOk, const char* shortTime) {
    _tft.fillRect(0, 0, Cfg::TFT_WIDTH, 16, C_PANEL);
    _tft.setTextColor(C_TEXT);
    _tft.setTextSize(1);
    _tft.setCursor(4, 4);
    _tft.print(shortTime);
    drawConnectivityIcon(98, 2, wifiOk, mqttOk);
}

void Hardware::updateTopClock(bool wifiOk, bool mqttOk, const char* shortTime) {
    _tft.fillRect(0, 0, 96, 16, C_PANEL);
    _tft.setTextColor(C_TEXT);
    _tft.setTextSize(1);
    _tft.setCursor(4, 4);
    _tft.print(shortTime);
    drawConnectivityIcon(98, 2, wifiOk, mqttOk);
}

void Hardware::drawCountdownBadge(uint16_t secondsLeft, uint16_t totalSeconds) {
    _tft.fillRect(0, 126, Cfg::TFT_WIDTH, 14, C_PANEL);
    _tft.setTextColor(C_MUTED);
    _tft.setTextSize(1);
    _tft.setCursor(4, 129);
    _tft.printf("Time: %us / %us", secondsLeft, totalSeconds);
}

void Hardware::drawHome(bool wifiOk, bool mqttOk, const char* shortTime,
                        uint8_t available, uint8_t problematic) {
    _tft.fillScreen(C_BG);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextColor(C_ACCENT);
    _tft.setTextSize(1);
    _tft.setCursor(6, 22);
    _tft.print("SMART LOCKER v2.1");

    _tft.fillRoundRect(6, 36, 116, 34, 4, C_PANEL);
    _tft.setTextColor(C_TEXT);
    _tft.setCursor(12, 42);
    _tft.printf("Free: %u / %u", available, Cfg::LOCKER_COUNT);
    _tft.setCursor(12, 55);
    if (problematic > 0) {
        _tft.setTextColor(C_RED);
        _tft.printf("Overdue: %u", problematic);
    } else {
        _tft.setTextColor(mqttOk ? C_GREEN : C_AMBER);
        _tft.print(mqttOk ? "System Ready" : "Offline Mode");
    }

    _tft.setTextColor(C_TEXT);
    _tft.setCursor(6, 78);
    _tft.print("[A] Assign Locker");
    _tft.setCursor(6, 92);
    _tft.print("[B] Unlock Locker");
    _tft.setCursor(6, 106);
    _tft.print("[D] OTP / Admin");
}

void Hardware::drawQrMatrix(int16_t x0, int16_t y0, uint8_t scale, const char* url) {
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcodeData, 3, ECC_LOW, url);

    const int16_t pad = 2;
    const int16_t box = qrcode.size * scale + pad * 2;
    _tft.fillRect(x0 - pad, y0 - pad, box, box, C_TEXT);

    for (uint8_t y = 0; y < qrcode.size; ++y) {
        for (uint8_t x = 0; x < qrcode.size; ++x) {
            if (qrcode_getModule(&qrcode, x, y)) {
                _tft.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, 0x0000);
            }
        }
    }
}

void Hardware::drawQrRegister(bool wifiOk, bool mqttOk, const char* shortTime,
                              const char* url, const char* subtitle, uint16_t secLeft) {
    _tft.fillScreen(C_BG);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextColor(C_AMBER);
    _tft.setTextSize(1);
    _tft.setCursor(4, 20);
    _tft.print(subtitle);

    drawQrMatrix(35, 34, 2, url);

    _tft.setTextColor(C_ACCENT);
    _tft.setCursor(4, 98);
    _tft.print("app.smartlocker.id");
    _tft.setTextColor(C_MUTED);
    _tft.setCursor(4, 111);
    _tft.print("[C] Back to Home");
    drawCountdownBadge(secLeft, Cfg::UI_QR_TIMEOUT_MS / 1000);
}

void Hardware::drawCredentialPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                                    const char* header, const char* hint,
                                    uint8_t pinLength, uint16_t secLeft) {
    _tft.fillScreen(C_BG);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextColor(C_ACCENT);
    _tft.setTextSize(1);
    _tft.setCursor(6, 22);
    _tft.print(header);

    _tft.setTextColor(C_TEXT);
    _tft.setCursor(6, 36);
    _tft.print(hint);

    updatePinBoxes(pinLength);

    _tft.setTextColor(C_MUTED);
    _tft.setCursor(6, 86);
    _tft.print("[#] Confirm  [*] Del");
    _tft.setCursor(6, 100);
    _tft.print("[C] Cancel");
    drawCountdownBadge(secLeft, Cfg::UI_IDLE_TIMEOUT_MS / 1000);
}

void Hardware::updatePinBoxes(uint8_t pinLength) {
    _tft.fillRect(6, 54, 116, 22, C_BG);
    for (uint8_t i = 0; i < Cfg::PIN_MAX_LENGTH; ++i) {
        const int16_t x = 6 + i * 14;
        _tft.drawRect(x, 54, 12, 18, i < pinLength ? C_ACCENT : C_MUTED);
        if (i < pinLength) {
            _tft.fillCircle(x + 6, 63, 2, C_TEXT);
        }
    }
}

void Hardware::drawLockerSelector(bool wifiOk, bool mqttOk, const char* shortTime,
                                  const LockerRecord records[Cfg::LOCKER_COUNT],
                                  const bool locked[Cfg::LOCKER_COUNT],
                                  uint8_t selectedIndex, uint16_t secLeft) {
    _tft.fillScreen(C_BG);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextColor(C_ACCENT);
    _tft.setTextSize(1);
    _tft.setCursor(4, 20);
    _tft.print("SELECT LOCKER (1-4):");

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        const int16_t y = 34 + i * 18;
        const bool sel = (i == selectedIndex);
        if (sel) {
            _tft.fillRoundRect(4, y - 2, 120, 15, 2, C_ACCENT);
            _tft.setTextColor(C_BG);
        } else {
            _tft.setTextColor(C_TEXT);
        }
        _tft.setCursor(8, y + 1);
        const char* st = isExpiredHold(records[i]) ? "OVERDUE" :
                         (isAssigned(records[i]) ? (locked[i] ? "OCC" : "OPEN") : "FREE");
        _tft.printf("[%u] L%02u : %s", i + 1, i + 1, st);
    }

    _tft.setTextColor(C_MUTED);
    _tft.setCursor(4, 110);
    _tft.print("1-4 / A-B+# / C:Home");
    drawCountdownBadge(secLeft, Cfg::UI_IDLE_TIMEOUT_MS / 1000);
}

void Hardware::drawUnlockActionPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                                      uint8_t lockerIndex, uint16_t secLeft) {
    _tft.fillScreen(C_BG);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextColor(C_GREEN);
    _tft.setTextSize(1);
    _tft.setCursor(6, 22);
    _tft.printf("VERIFIED: %s", Cfg::LOCKER_IDS[lockerIndex]);

    _tft.fillRoundRect(6, 38, 116, 28, 3, C_PANEL);
    _tft.setTextColor(C_TEXT);
    _tft.setCursor(10, 44);
    _tft.print("[1] UNLOCK + FINISH");
    _tft.setTextColor(C_MUTED);
    _tft.setCursor(10, 55);
    _tft.print("    End & release");

    _tft.fillRoundRect(6, 72, 116, 28, 3, C_PANEL);
    _tft.setTextColor(C_ACCENT);
    _tft.setCursor(10, 78);
    _tft.print("[2] UNLOCK + EXTEND");
    _tft.setTextColor(C_MUTED);
    _tft.setCursor(10, 89);
    _tft.print("    Keep & re-lock");

    _tft.setCursor(6, 108);
    _tft.print("[C] Cancel");
    drawCountdownBadge(secLeft, Cfg::UI_IDLE_TIMEOUT_MS / 1000);
}

void Hardware::drawDMenuFirstPage(bool wifiOk, bool mqttOk, const char* shortTime,
                                  const char* url, uint8_t pinLength, uint16_t secLeft) {
    _tft.fillScreen(C_BG);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextColor(C_ACCENT);
    _tft.setTextSize(1);
    _tft.setCursor(4, 19);
    _tft.print("OTP / ADMIN PIN:");

    // Compact QR on left, PIN status & URL on right
    drawQrMatrix(6, 32, 2, url);

    _tft.setTextColor(C_TEXT);
    _tft.setCursor(68, 34);
    _tft.print("Web Reg:");
    _tft.setTextColor(C_ACCENT);
    _tft.setCursor(68, 45);
    _tft.print("smartlocker");
    _tft.setCursor(68, 55);
    _tft.print(".id");

    _tft.setTextColor(C_TEXT);
    _tft.setCursor(68, 70);
    _tft.printf("PIN: %u/8", pinLength);

    _tft.setTextColor(C_MUTED);
    _tft.setCursor(4, 96);
    _tft.print("[#] Submit  [*] Del");
    _tft.setCursor(4, 110);
    _tft.print("[C] Return Home");
    drawCountdownBadge(secLeft, Cfg::UI_IDLE_TIMEOUT_MS / 1000);
}

void Hardware::drawTapCardPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                                 const char* title, const char* subtitle, uint16_t secLeft) {
    _tft.fillScreen(C_BG);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextColor(C_ACCENT);
    _tft.setTextSize(1);
    _tft.setCursor(6, 24);
    _tft.print(title);

    _tft.fillRoundRect(6, 42, 116, 44, 4, C_PANEL);
    _tft.setTextColor(C_TEXT);
    _tft.setCursor(12, 52);
    _tft.print("TAP RFID CARD NOW");
    _tft.setTextColor(C_MUTED);
    _tft.setCursor(12, 68);
    _tft.print(subtitle);

    _tft.setCursor(6, 102);
    _tft.print("[C] Cancel");
    drawCountdownBadge(secLeft, Cfg::UI_IDLE_TIMEOUT_MS / 1000);
}

void Hardware::drawResult(bool wifiOk, bool mqttOk, const char* shortTime,
                          const String& title, const String& detail,
                          bool isError, uint16_t secLeft) {
    _tft.fillScreen(C_BG);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.fillRoundRect(6, 24, 116, 84, 4, C_PANEL);
    _tft.setTextColor(isError ? C_RED : C_GREEN);
    _tft.setTextSize(1);
    _tft.setCursor(12, 34);
    _tft.print(title);

    _tft.setTextColor(C_TEXT);
    _tft.setCursor(12, 54);
    _tft.print(detail);

    drawCountdownBadge(secLeft, Cfg::UI_RESULT_MS / 1000);
}

void Hardware::drawAdminMenu(bool wifiOk, bool mqttOk, const char* shortTime,
                             const LockerRecord records[Cfg::LOCKER_COUNT],
                             const bool locked[Cfg::LOCKER_COUNT],
                             uint8_t userCount, uint16_t secLeft) {
    _tft.fillScreen(C_BG);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextColor(C_AMBER);
    _tft.setTextSize(1);
    _tft.setCursor(4, 18);
    _tft.printf("ADMIN (Users:%u)", userCount);

    _tft.setTextColor(C_TEXT);
    _tft.setCursor(4, 30);
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        const char c = isExpiredHold(records[i]) ? '!' : (isAssigned(records[i]) ? 'O' : 'F');
        _tft.printf("L%u:%c%c ", i + 1, c, locked[i] ? 'L' : 'U');
    }

    _tft.setCursor(4, 44);  _tft.print("[1] Assign Unreg");
    _tft.setCursor(4, 56);  _tft.print("[2] Lock/Unlock");
    _tft.setCursor(4, 68);  _tft.print("[3] Release Locker");
    _tft.setCursor(4, 80);  _tft.print("[4] Set User RFID");
    _tft.setCursor(4, 92);  _tft.print("[5] SyncPush/Clear");
    _tft.setCursor(4, 104); _tft.print("[6] SyncPull [0]Home");

    drawCountdownBadge(secLeft, Cfg::ADMIN_MENU_TIMEOUT_MS / 1000);
}

void Hardware::drawAdminTargetPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                                     const char* title, const char* prompt,
                                     const String& value, uint16_t secLeft) {
    _tft.fillScreen(C_BG);
    drawTopBar(wifiOk, mqttOk, shortTime);

    _tft.setTextColor(C_AMBER);
    _tft.setTextSize(1);
    _tft.setCursor(6, 22);
    _tft.print(title);

    _tft.setTextColor(C_TEXT);
    _tft.setCursor(6, 40);
    _tft.print(prompt);

    _tft.fillRoundRect(6, 56, 116, 22, 3, C_PANEL);
    _tft.setTextColor(C_ACCENT);
    _tft.setCursor(12, 63);
    _tft.print(value.isEmpty() ? "_" : value);

    _tft.setTextColor(C_MUTED);
    _tft.setCursor(6, 90);
    _tft.print("[#] Confirm  [*] Del");
    _tft.setCursor(6, 104);
    _tft.print("[C] Back to Admin");
    drawCountdownBadge(secLeft, Cfg::ADMIN_MENU_TIMEOUT_MS / 1000);
}
