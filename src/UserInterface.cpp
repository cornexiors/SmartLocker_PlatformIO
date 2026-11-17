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
        // Queue toast so we do not interrupt active user input
        _hasPendingNotice = true;
        _pendingTitle = title;
        _pendingDetail = detail;
        _pendingError = isError;
    }
}

void UserInterface::showResult(const String& title, const String& detail, bool isError) {
    _pin = "";
    _state = RESULT;
    _resultDeadline = millis() + Cfg::UI_RESULT_MS;

    char shortTime[12];
    _clock->formatShortLocal(shortTime, sizeof(shortTime));
    _hw->drawResult(_net->wifiConnected(), _net->mqttConnected(), shortTime, title, detail, isError);
}

void UserInterface::returnHome() {
    _pin = "";
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
        case ADMIN_PIN:
            _hw->drawAdminPin(wifiOk, mqttOk, shortTime, _pin.length());
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
    // Only accept fresh RFID taps when actively on a credential prompt screen
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
            _state = ADMIN_PIN;
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

    // HIDDEN ADMIN PIN ENTRY
    if (_state == ADMIN_PIN) {
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
            } else {
                showResult("ADMIN DENIED", "Wrong Admin PIN", true);
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

    // Poll RFID reader continuously; only acts if on credential prompt screen
    String uid;
    if (_hw->readCard(uid)) {
        Serial.printf("RFID Scanned: %s\n", uid.c_str());
        onRfidScanned(uid);
    }

    // Debounced 4x4 keypad scan
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

    // 3-second result banner auto-dismiss
    if (_state == RESULT && static_cast<int32_t>(now - _resultDeadline) >= 0) {
        returnHome();
        return;
    }

    // 18-second idle auto-timeout for all interactive sub-screens
    if (_state != HOME && _state != RESULT &&
        (now - _lastActivityMs >= Cfg::UI_IDLE_TIMEOUT_MS)) {
        returnHome();
        return;
    }

    // 1-second top status bar clock & connectivity update
    if (now - _lastUiRefresh >= Cfg::UI_REFRESH_MS) {
        _lastUiRefresh = now;
        char shortTime[12];
        _clock->formatShortLocal(shortTime, sizeof(shortTime));
        _hw->updateHomeClock(_net->wifiConnected(), _net->mqttConnected(), shortTime);
    }
}