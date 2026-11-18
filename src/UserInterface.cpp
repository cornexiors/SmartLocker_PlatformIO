#include "App.h"

void UserInterface::begin(Hardware& hw, LockerManager& lockers, NetworkService& net) {
    _hw = &hw;
    _lockers = &lockers;
    _net = &net;
    returnHome();
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
    _state = RESULT;
    _resultDeadline = millis() + Cfg::UI_RESULT_MS;

    _hw->drawResult(_net->wifiConnected(), _net->mqttConnected(), title, detail, isError);
}

void UserInterface::returnHome() {
    _pin = "";

    if (_hasPendingNotice) {
        _hasPendingNotice = false;
        showResult(_pendingTitle, _pendingDetail, _pendingError);
        return;
    }

    _state = HOME;
    Serial.println(F("[UI] -> HOME"));
    _hw->drawHome(
        _net->wifiConnected(),
        _net->mqttConnected(),
        _lockers->availableCount(),
        _hw->doorClosed(0)
    );
}

void UserInterface::refreshCurrentScreen() {
    const bool wifiOk = _net->wifiConnected();
    const bool mqttOk = _net->mqttConnected();

    switch (_state) {
        case HOME:
            _hw->drawHome(wifiOk, mqttOk, _lockers->availableCount(), _hw->doorClosed(0));
            break;
        case ASSIGN_CRED:
            _hw->drawCredentialPrompt(wifiOk, mqttOk, true, _pin.length());
            break;
        case UNLOCK_CRED:
            _hw->drawCredentialPrompt(wifiOk, mqttOk, false, _pin.length());
            break;
        case RESULT:
            break;
    }
}

void UserInterface::onRfidScanned(const String& uid) {
    if (_state != ASSIGN_CRED && _state != UNLOCK_CRED) return;

    _lastActivityMs = millis();
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
    } else if (status == ActionStatus::ALREADY_ASSIGNED) {
        showResult("ALREADY ASSIGNED", lockerName, true);
    } else if (status == ActionStatus::NO_LOCKER_FREE) {
        showResult("NO LOCKER FREE", "All lockers full", true);
    } else {
        showResult("Wrong/Unauthorized", "RFID", true);
    }
}

void UserInterface::onKey(char key) {
    static uint32_t lastAcceptedKeyMs = 0;
    const uint32_t now = millis();
    if (now - lastAcceptedKeyMs < 150) return;
    lastAcceptedKeyMs = now;

    _lastActivityMs = now;

    if (_state == RESULT) {
        returnHome();
        return;
    }

    if (key == 'C' && _state != HOME) {
        returnHome();
        return;
    }

    // HOME SCREEN
    if (_state == HOME) {
        if (key == 'A') {
            _pin = "";
            _state = ASSIGN_CRED;
            refreshCurrentScreen();
        } else if (key == 'B') {
            _pin = "";
            _state = UNLOCK_CRED;
            refreshCurrentScreen();
        }
        return;
    }

    // ASSIGN OR UNLOCK CREDENTIAL ENTRY
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
            } else if (status == ActionStatus::ALREADY_ASSIGNED) {
                showResult("ALREADY ASSIGNED", lockerName, true);
            } else if (status == ActionStatus::NO_LOCKER_FREE) {
                showResult("NO LOCKER FREE", "All lockers full", true);
            } else {
                showResult("Wrong/Unauthorized", "PIN Code", true);
            }
        }
    }
}

void UserInterface::tick() {
    const uint32_t now = millis();

    String uid;
    if (_hw->readCard(uid)) {
        Serial.printf("RFID Scanned: %s\n", uid.c_str());
        onRfidScanned(uid);
    }

    if (now - _lastKeyScan >= Cfg::KEYPAD_SCAN_MS) {
        _lastKeyScan = now;
        const char key = _hw->scanKey();
        if (key == _rawKey) {
            if (_keyStable < 3) ++_keyStable;
        } else {
            _rawKey = key;
            _keyStable = 0;
        }

        if (_keyStable >= 3) {
            if (_rawKey == 0) {
                _keyReleased = true;
                _stableKey = 0;
            } else if (_keyReleased && _stableKey != _rawKey) {
                _keyReleased = false;
                _stableKey = _rawKey;
                Serial.printf("[UI] Accepted key '%c' in state %u\n",
                              _stableKey, static_cast<unsigned>(_state));
                onKey(_stableKey);
            }
        }
    }

    if (_state == RESULT && static_cast<int32_t>(millis() - _resultDeadline) >= 0) {
        returnHome();
        return;
    }

    if (_state != HOME && _state != RESULT &&
        (millis() - _lastActivityMs >= Cfg::UI_IDLE_TIMEOUT_MS)) {
        returnHome();
        return;
    }

    // Redraw Home screen immediately if Wi-Fi, MQTT, or Door 1 reed state changes
    static bool lastShownWifi = false;
    static bool lastShownMqtt = false;
    static bool lastShownDoor1 = true;
    const bool curWifi = _net->wifiConnected();
    const bool curMqtt = _net->mqttConnected();
    const bool curDoor1 = _hw->doorClosed(0);

    if (_state == HOME && (curWifi != lastShownWifi || curMqtt != lastShownMqtt || curDoor1 != lastShownDoor1)) {
        lastShownWifi = curWifi;
        lastShownMqtt = curMqtt;
        lastShownDoor1 = curDoor1;
        refreshCurrentScreen();
    }
}