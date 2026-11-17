#include "App.h"

void UserInterface::begin(Hardware& hw, LockerManager& lockers,
                          ClockService& clock, NetworkService& net) {
    _hw = &hw;
    _lockers = &lockers;
    _clock = &clock;
    _net = &net;
    returnHome();
}

void UserInterface::touchActivity() {
    _lastActivityMs = millis();
}

void UserInterface::notifyRemoteCommand(const String& title, const String& detail, bool isError) {
    if (_state == HOME) {
        showResult(title, detail, isError);
    } else {
        _hasPendingNotice = true;
        _pendingTitle = title;
        _pendingDetail = detail;
        _pendingError = isError;
    }
}

void UserInterface::showResult(const String& title, const String& detail, bool isError) {
    _pin = "";
    _activeOtp = "";
    _state = RESULT;
    _resultDeadline = millis() + Cfg::UI_RESULT_MS;

    char shortTime[12];
    _clock->formatShortLocal(shortTime, sizeof(shortTime));
    _hw->drawResult(_net->wifiConnected(), _net->mqttConnected(), shortTime, title, detail, isError);
}

void UserInterface::returnHome() {
    _pin = "";
    _activeOtp = "";
    _otpUserId = "";

    if (_hasPendingNotice) {
        _hasPendingNotice = false;
        showResult(_pendingTitle, _pendingDetail, _pendingError);
        return;
    }

    _state = HOME;
    char shortTime[12];
    _clock->formatShortLocal(shortTime, sizeof(shortTime));
    _hw->drawHome(
        _net->wifiConnected(),
        _net->mqttConnected(),
        shortTime,
        _lockers->availableCount(),
        _lockers->problematicCount()
    );
    _lastUiRefresh = millis();
}

void UserInterface::refreshCurrentScreen() {
    char shortTime[12];
    _clock->formatShortLocal(shortTime, sizeof(shortTime));
    const bool wifiOk = _net->wifiConnected();
    const bool mqttOk = _net->mqttConnected();

    switch (_state) {
        case HOME:
            _hw->drawHome(wifiOk, mqttOk, shortTime,
                          _lockers->availableCount(), _lockers->problematicCount());
            break;
        case ASSIGN_ASK:
            _hw->drawRegisterPrompt(wifiOk, mqttOk, shortTime);
            break;
        case QR_CODE:
            _hw->drawQrCode(wifiOk, mqttOk, shortTime, Cfg::APP_REGISTER_URL);
            break;
        case ASSIGN_CRED:
            _hw->drawCredentialPrompt(wifiOk, mqttOk, shortTime, true, _pin.length());
            break;
        case UNLOCK_CRED:
            _hw->drawCredentialPrompt(wifiOk, mqttOk, shortTime, false, _pin.length());
            break;
        case OTP_ENTRY:
            _hw->drawOtpPrompt(wifiOk, mqttOk, shortTime, _pin.length());
            break;
        case OTP_TAP_RFID:
            _hw->drawOtpTapRfid(wifiOk, mqttOk, shortTime, _otpUserId.c_str());
            break;
        case ADMIN_MENU:
            {
                LockerRecord recs[Cfg::LOCKER_COUNT];
                bool lcks[Cfg::LOCKER_COUNT];
                for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
                    recs[i] = _lockers->record(i);
                    lcks[i] = _lockers->isLocked(i);
                }
                _hw->drawAdminMenu(wifiOk, mqttOk, shortTime, recs, lcks,
                                   _lockers->registeredUserCount());
            }
            break;
        case RESULT:
            break;
    }
}

void UserInterface::onRfidScanned(const String& uid) {
    // Case 1: D-Mode OTP RFID registration tap
    if (_state == OTP_TAP_RFID) {
        touchActivity();
        String registeredUser;
        const ActionStatus st = _lockers->completeOtpRfidRegistration(_activeOtp, uid, registeredUser);
        if (st == ActionStatus::OK) {
            showResult("RFID REGISTERED", uid, false);
        } else if (st == ActionStatus::ALREADY_ASSIGNED) {
            showResult("RFID IN USE", "Already registered", true);
        } else {
            showResult("REG FAILED", "OTP expired/invalid", true);
        }
        return;
    }

    // Case 2: Assign or Unlock credential tap
    if (_state != ASSIGN_CRED && _state != UNLOCK_CRED) return;

    touchActivity();
    const bool assigning = (_state == ASSIGN_CRED);
    String lockerName;
    const ActionStatus status = assigning
        ? _lockers->assignByUid(uid, lockerName)
        : _lockers->unlockAndReleaseByUid(uid, lockerName);

    if (status == ActionStatus::OK) {
        showResult(
            assigning ? "ASSIGNED LOCKER" : "UNLOCKING LOCKER",
            lockerName,
            false
        );
    } else if (status == ActionStatus::EXPIRED) {
        showResult("LOCKER EXPIRED", "Contact Admin", true);
    } else if (status == ActionStatus::ALREADY_ASSIGNED) {
        showResult("ALREADY ASSIGNED", lockerName, true);
    } else if (status == ActionStatus::NO_LOCKER_FREE) {
        showResult("NO LOCKER FREE", "All lockers full", true);
    } else {
        showResult("Wrong/Unauthorized", "RFID", true);
    }
}

void UserInterface::onKey(char key) {
    // 250ms cooldown prevents Column 3 (A/B/C/D) membrane bounce into 'C' (Back)
    static uint32_t lastAcceptedKeyMs = 0;
    const uint32_t now = millis();
    if (now - lastAcceptedKeyMs < 250) return;
    lastAcceptedKeyMs = now;

    touchActivity();

    if (_state == RESULT) {
        returnHome();
        return;
    }

    // Global Back/Cancel key
    if (key == 'C' && _state != HOME) {
        if (_state == QR_CODE || _state == ASSIGN_CRED) {
            _pin = "";
            _state = ASSIGN_ASK;
            refreshCurrentScreen();
        } else {
            returnHome();
        }
        return;
    }

    // HOME SCREEN
    if (_state == HOME) {
        if (key == 'A') {
            _state = ASSIGN_ASK;
            refreshCurrentScreen();
        } else if (key == 'B') {
            _pin = "";
            _state = UNLOCK_CRED;
            refreshCurrentScreen();
        } else if (key == 'D') {
            _pin = "";
            _state = OTP_ENTRY;
            refreshCurrentScreen();
        }
        return;
    }

    // ASSIGN: REGISTERED IN APP?
    if (_state == ASSIGN_ASK) {
        if (key == 'A') {
            _pin = "";
            _state = ASSIGN_CRED;
            refreshCurrentScreen();
        } else if (key == 'B') {
            _state = QR_CODE;
            refreshCurrentScreen();
        }
        return;
    }

    // CREDENTIAL INPUT (ASSIGN OR UNLOCK)
    if (_state == ASSIGN_CRED || _state == UNLOCK_CRED) {
        if (key >= '0' && key <= '9' && _pin.length() < Cfg::PIN_MAX_LENGTH) {
            _pin += key;
            _hw->updateCredentialPin(_pin.length());
        } else if (key == '*') {
            if (!_pin.isEmpty()) {
                _pin.remove(_pin.length() - 1);
                _hw->updateCredentialPin(_pin.length());
            }
        } else if (key == '#') {
            if (_pin.length() < Cfg::PIN_MIN_LENGTH) {
                showResult("PIN TOO SHORT", "Use 4-8 digits", true);
                return;
            }
            const bool assigning = (_state == ASSIGN_CRED);
            String lockerName;
            const ActionStatus status = assigning
                ? _lockers->assignByPin(_pin, lockerName)
                : _lockers->unlockAndReleaseByPin(_pin, lockerName);

            if (status == ActionStatus::OK) {
                showResult(
                    assigning ? "ASSIGNED LOCKER" : "UNLOCKING LOCKER",
                    lockerName,
                    false
                );
            } else if (status == ActionStatus::EXPIRED) {
                showResult("LOCKER EXPIRED", "Contact Admin", true);
            } else if (status == ActionStatus::ALREADY_ASSIGNED) {
                showResult("ALREADY ASSIGNED", lockerName, true);
            } else if (status == ActionStatus::NO_LOCKER_FREE) {
                showResult("NO LOCKER FREE", "All lockers full", true);
            } else {
                showResult("Wrong/Unauthorized", "PIN Code", true);
            }
        }
        return;
    }

    // D-MODE: OTP FOR RFID REGISTRATION (ALSO ACCEPTS ADMIN MASTER PIN)
    if (_state == OTP_ENTRY) {
        if (key >= '0' && key <= '9' && _pin.length() < Cfg::PIN_MAX_LENGTH) {
            _pin += key;
            _hw->updateCredentialPin(_pin.length());
        } else if (key == '*') {
            if (!_pin.isEmpty()) {
                _pin.remove(_pin.length() - 1);
                _hw->updateCredentialPin(_pin.length());
            }
        } else if (key == '#') {
            if (_pin == Cfg::ADMIN_MASTER_PIN) {
                _pin = "";
                _state = ADMIN_MENU;
                refreshCurrentScreen();
                return;
            }
            String userId;
            if (_lockers->verifyOtp(_pin, userId)) {
                _activeOtp = _pin;
                _otpUserId = userId;
                _pin = "";
                _state = OTP_TAP_RFID;
                refreshCurrentScreen();
            } else {
                showResult("INVALID / EXPIRED", "Check App OTP", true);
            }
        }
        return;
    }

    // HIDDEN ADMIN OPERATOR MENU
    if (_state == ADMIN_MENU) {
        if (key >= '1' && key <= '4') {
            const uint8_t idx = static_cast<uint8_t>(key - '1');
            if (idx < Cfg::LOCKER_COUNT) {
                _lockers->release(idx, "ADMIN");
                refreshCurrentScreen();
            }
        } else if (key == 'A') {
            _net->publishUserSyncRequest(_lockers->registeredUserCount());
            showResult("SYNC REQUESTED", "Pulling users...", false);
        }
    }
}

void UserInterface::tick() {
    const uint32_t now = millis();

    String uid;
    if (_hw->readCard(uid)) {
        appLog("INFO", "RFID Scanned: %s", uid.c_str());
        onRfidScanned(uid);
    }

    if (now - _lastKeyScan >= Cfg::KEYPAD_SCAN_MS) {
        _lastKeyScan = now;
        const char key = _hw->scanKey();
        if (key == _rawKey) {
            if (_keyStable < 2) ++_keyStable;
        } else {
            _rawKey = key;
            _keyStable = 0;
        }
        if (_keyStable >= 2 && _stableKey != _rawKey) {
            _stableKey = _rawKey;
            if (_stableKey) onKey(_stableKey);
        }
    }

    if (_state == RESULT && static_cast<int32_t>(now - _resultDeadline) >= 0) {
        returnHome();
        return;
    }

    if (_state != HOME && _state != RESULT &&
        (now - _lastActivityMs >= Cfg::UI_IDLE_TIMEOUT_MS)) {
        returnHome();
        return;
    }

    // Redraw Home body immediately if Wi-Fi or MQTT connection status changes
    static bool lastShownWifi = false;
    static bool lastShownMqtt = false;
    const bool curWifi = _net->wifiConnected();
    const bool curMqtt = _net->mqttConnected();

    if (_state == HOME && (curWifi != lastShownWifi || curMqtt != lastShownMqtt)) {
        lastShownWifi = curWifi;
        lastShownMqtt = curMqtt;
        refreshCurrentScreen();
        _lastUiRefresh = now;
    } else if (now - _lastUiRefresh >= Cfg::UI_REFRESH_MS) {
        _lastUiRefresh = now;
        char shortTime[12];
        _clock->formatShortLocal(shortTime, sizeof(shortTime));
        _hw->updateHomeClock(curWifi, curMqtt, shortTime);
    }
}