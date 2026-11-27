#include "App.h"

void UserInterface::begin(Hardware& hw, LockerManager& lockers,
                          ClockService& clock, NetworkService& net) {
    _hw = &hw;
    _lockers = &lockers;
    _clock = &clock;
    _net = &net;
    returnHome();
}

bool UserInterface::isAdminActive() const {
    return _state == ADMIN_MENU || _state == ADMIN_PICK_LOCKER ||
           _state == ADMIN_SET_RFID_UID || _state == ADMIN_SET_RFID_TAP ||
           _state == ADMIN_SETUP_PIN || _state == ADMIN_SETUP_RFID ||
           _state == ADMIN_AUTH_RFID;
}

bool UserInterface::isSessionBusy() const {
    return _state != HOME && _state != RESULT;
}

void UserInterface::notifyRemoteCommand(const String& title, const String& detail, bool isError) {
    _pendingTitle = title;
    _pendingDetail = detail;
    _pendingError = isError;
    _hasPendingNotice = true;
}

uint16_t UserInterface::remainingSeconds() const {
    if (_stateTimeoutMs == 0) return 0;
    const uint32_t elapsed = millis() - _stateEnteredMs;
    if (elapsed >= _stateTimeoutMs) return 0;
    return static_cast<uint16_t>((_stateTimeoutMs - elapsed + 999) / 1000);
}

void UserInterface::enterState(State next, uint32_t timeoutMs) {
    _state = next;
    _stateEnteredMs = millis();
    _stateTimeoutMs = timeoutMs;
    refreshCurrentScreen(true);
}

void UserInterface::touchActivity() {
    _stateEnteredMs = millis();
}

void UserInterface::returnHome() {
    _inputBuf = "";
    _activeOtp = "";
    _pendingUserId = "";
    _setupAdminPin = "";
    _adminOp = OP_NONE;
    _verifyingAsync = false;
    enterState(HOME, 0);
}

void UserInterface::showResult(const String& title, const String& detail, bool isError) {
    _pendingTitle = title;
    _pendingDetail = detail;
    _pendingError = isError;
    _verifyingAsync = false;
    _hw->beep(isError ? 180 : 60);
    enterState(RESULT, Cfg::UI_RESULT_MS);
}

void UserInterface::showQrPrompt(const char* subtitle) {
    _pendingDetail = String(subtitle);
    _verifyingAsync = false;
    _hw->beep(120);
    enterState(QR_REGISTER, Cfg::UI_QR_TIMEOUT_MS);
}

void UserInterface::submitAsyncPin(AuthRequestType type, int8_t lockerIdx, const String& pin) {
    if (_verifyingAsync) return;
    _verifyingAsync = true;
    ++_pendingReqId;

    AuthRequest req{};
    req.requestId = _pendingReqId;
    req.type = type;
    req.lockerIndex = lockerIdx;
    strncpy(req.pin, pin.c_str(), sizeof(req.pin) - 1);

    if (xQueueSend(gAuthReqQueue, &req, 0) != pdTRUE) {
        _verifyingAsync = false;
        showResult("BUSY", "Try again shortly", true);
    }
}

void UserInterface::onAsyncAuthResult(const AuthResult& res) {
    if (!_verifyingAsync || res.requestId != _pendingReqId) return;
    _verifyingAsync = false;

    if (res.type == AuthRequestType::USER_ASSIGN_PIN) {
        if (!res.matched) {
            showQrPrompt("Unregistered PIN:");
            return;
        }
        String lockerName;
        const ActionStatus st = _lockers->assignByUserSlot(res.userSlot, "PIN", lockerName);
        if (st == ActionStatus::OK) {
            showResult("ASSIGNED", lockerName + " Opened", false);
        } else if (st == ActionStatus::ALREADY_ASSIGNED) {
            showResult("DENIED", "Already has " + lockerName, true);
        } else if (st == ActionStatus::NO_LOCKER_FREE) {
            showResult("FULL", "No free locker", true);
        } else if (st == ActionStatus::OFFLINE_BLOCKED) {
            showQrPrompt("Offline! Web Reg:");
        } else {
            showResult("ERROR", "Actuator fault", true);
        }
        return;
    }

    if (res.type == AuthRequestType::USER_UNLOCK_PIN) {
        if (!res.matched) {
            showQrPrompt("Invalid PIN / Reg:");
            return;
        }
        _pendingUserId = String(res.userId);
        _hw->beep(60);
        enterState(UNLOCK_ACTION, Cfg::UI_IDLE_TIMEOUT_MS);
        return;
    }

    if (res.type == AuthRequestType::ADMIN_DMENU_PIN) {
        if (!res.matched) {
            showQrPrompt("Invalid OTP/Admin:");
            return;
        }
        _hw->beep(60);
        enterState(ADMIN_AUTH_RFID, Cfg::UI_IDLE_TIMEOUT_MS);
    }
}

void UserInterface::tick() {
    const uint32_t now = millis();

    if (_hasPendingNotice && _state == HOME) {
        _hasPendingNotice = false;
        showResult(_pendingTitle, _pendingDetail, _pendingError);
    }

    // 1. Debounced Keypad Scan (25 ms)
    if (now - _lastKeyScan >= Cfg::KEYPAD_SCAN_MS) {
        _lastKeyScan = now;
        const char k = _hw->scanKey();
        if (k != _rawKey) {
            _rawKey = k;
            _keyStable = 1;
        } else if (k != 0) {
            if (_keyStable < 3) {
                ++_keyStable;
                if (_keyStable == 3 && _stableKey != k) {
                    _stableKey = k;
                    onKey(k);
                }
            }
        } else {
            _stableKey = 0;
            _keyStable = 0;
        }
    }

    // 2. RFID Card Scan (100 ms)
    String uid;
    if (_hw->readCard(uid)) {
        onRfidScanned(uid);
    }

    // 3. Timeout & 1 Hz LCD Countdown Refresh
    if (_stateTimeoutMs > 0 && (now - _stateEnteredMs >= _stateTimeoutMs)) {
        if (_state == ADMIN_PICK_LOCKER || _state == ADMIN_SET_RFID_UID || _state == ADMIN_SET_RFID_TAP) {
            _inputBuf = "";
            enterState(ADMIN_MENU, Cfg::ADMIN_MENU_TIMEOUT_MS);
        } else {
            returnHome();
        }
        return;
    }

    if (now - _lastUiRefresh >= Cfg::UI_REFRESH_MS) {
        _lastUiRefresh = now;
        refreshCurrentScreen(false);
    }
}

void UserInterface::onKey(char key) {
    if (_verifyingAsync) return;
    _hw->beep(25);
    touchActivity();

    // Global cancel on non-admin screens
    if (key == 'C' && _state != HOME && !isAdminActive()) {
        returnHome();
        return;
    }

    switch (_state) {
        case HOME: {
            if (key == 'A') {
                if (!_net->online()) {
                    showQrPrompt("Offline! Reg Web:");
                    return;
                }
                if (_lockers->availableCount() == 0) {
                    showResult("FULL", "No free locker", true);
                    return;
                }
                _inputBuf = "";
                enterState(ASSIGN_CRED, Cfg::UI_IDLE_TIMEOUT_MS);
            } else if (key == 'B') {
                _selectedLocker = 0;
                enterState(UNLOCK_SELECT, Cfg::UI_IDLE_TIMEOUT_MS);
            } else if (key == 'D') {
                _inputBuf = "";
                enterState(D_MENU_PIN, Cfg::UI_IDLE_TIMEOUT_MS);
            }
            break;
        }

        case QR_REGISTER:
        case RESULT: {
            if (key == 'C' || key == '#') returnHome();
            break;
        }

        case ASSIGN_CRED: {
            if (key >= '0' && key <= '9') {
                if (_inputBuf.length() < Cfg::PIN_MAX_LENGTH) {
                    _inputBuf += key;
                    _hw->updatePinBoxes(_inputBuf.length());
                }
            } else if (key == '*') {
                if (!_inputBuf.isEmpty()) {
                    _inputBuf.remove(_inputBuf.length() - 1);
                    _hw->updatePinBoxes(_inputBuf.length());
                }
            } else if (key == '#') {
                if (_inputBuf.length() >= Cfg::PIN_MIN_LENGTH) {
                    submitAsyncPin(AuthRequestType::USER_ASSIGN_PIN, -1, _inputBuf);
                }
            }
            break;
        }

        case UNLOCK_SELECT: {
            if (key >= '1' && key <= ('0' + Cfg::LOCKER_COUNT)) {
                _selectedLocker = static_cast<uint8_t>(key - '1');
                if (!isAssigned(_lockers->record(_selectedLocker))) {
                    showResult("EMPTY", String(Cfg::LOCKER_IDS[_selectedLocker]) + " is free", true);
                    return;
                }
                _inputBuf = "";
                enterState(UNLOCK_CRED, Cfg::UI_IDLE_TIMEOUT_MS);
            } else if (key == 'A') {
                _selectedLocker = (_selectedLocker + Cfg::LOCKER_COUNT - 1) % Cfg::LOCKER_COUNT;
                refreshCurrentScreen(true);
            } else if (key == 'B') {
                _selectedLocker = (_selectedLocker + 1) % Cfg::LOCKER_COUNT;
                refreshCurrentScreen(true);
            } else if (key == '#') {
                if (!isAssigned(_lockers->record(_selectedLocker))) {
                    showResult("EMPTY", String(Cfg::LOCKER_IDS[_selectedLocker]) + " is free", true);
                    return;
                }
                _inputBuf = "";
                enterState(UNLOCK_CRED, Cfg::UI_IDLE_TIMEOUT_MS);
            }
            break;
        }

        case UNLOCK_CRED: {
            if (key >= '0' && key <= '9') {
                if (_inputBuf.length() < Cfg::PIN_MAX_LENGTH) {
                    _inputBuf += key;
                    _hw->updatePinBoxes(_inputBuf.length());
                }
            } else if (key == '*') {
                if (!_inputBuf.isEmpty()) {
                    _inputBuf.remove(_inputBuf.length() - 1);
                    _hw->updatePinBoxes(_inputBuf.length());
                }
            } else if (key == '#') {
                if (_inputBuf.length() >= Cfg::PIN_MIN_LENGTH) {
                    submitAsyncPin(AuthRequestType::USER_UNLOCK_PIN, _selectedLocker, _inputBuf);
                }
            }
            break;
        }

        case UNLOCK_ACTION: {
            String lockerName;
            if (key == '1') {
                const ActionStatus st = _lockers->executeUnlockFinish(_selectedLocker, "USER_FINISH", lockerName);
                showResult(st == ActionStatus::OK ? "FINISHED" : "ERROR",
                           st == ActionStatus::OK ? (lockerName + " Released") : "Actuator fault",
                           st != ActionStatus::OK);
            } else if (key == '2') {
                const ActionStatus st = _lockers->executeUnlockExtend(_selectedLocker, "USER_EXTEND", lockerName);
                showResult(st == ActionStatus::OK ? "EXTENDED" : "ERROR",
                           st == ActionStatus::OK ? (lockerName + " Kept Open") : "Actuator fault",
                           st != ActionStatus::OK);
            }
            break;
        }

        case D_MENU_PIN: {
            if (key >= '0' && key <= '9') {
                if (_inputBuf.length() < Cfg::PIN_MAX_LENGTH) {
                    _inputBuf += key;
                    refreshCurrentScreen(true);
                }
            } else if (key == '*') {
                if (!_inputBuf.isEmpty()) {
                    _inputBuf.remove(_inputBuf.length() - 1);
                    refreshCurrentScreen(true);
                }
            } else if (key == '#') {
                if (_inputBuf.length() < Cfg::PIN_MIN_LENGTH) return;

                // 1. Check if first-boot admin setup is required after fresh flash
                if (!_lockers->isAdminProvisioned()) {
                    _setupAdminPin = _inputBuf;
                    _inputBuf = "";
                    enterState(ADMIN_SETUP_RFID, Cfg::UI_IDLE_TIMEOUT_MS);
                    return;
                }

                // 2. Check if 8-digit code matches an active RFID Registration OTP
                String otpUser;
                if (_inputBuf.length() == Cfg::RFID_OTP_LENGTH &&
                    _lockers->verifyOtp(_inputBuf, otpUser)) {
                    _activeOtp = _inputBuf;
                    _pendingUserId = otpUser;
                    _inputBuf = "";
                    enterState(RFID_OTP_TAP, Cfg::UI_IDLE_TIMEOUT_MS);
                    return;
                }

                // 3. Otherwise verify against Admin PIN on Core 0
                submitAsyncPin(AuthRequestType::ADMIN_DMENU_PIN, -1, _inputBuf);
            }
            break;
        }

        case ADMIN_MENU: {
            if (key == '0' || key == 'C') {
                returnHome();
            } else if (key == '1') {
                _adminOp = OP_ASSIGN_UNREG;
                enterState(ADMIN_PICK_LOCKER, Cfg::ADMIN_MENU_TIMEOUT_MS);
            } else if (key == '2') {
                _adminOp = OP_TOGGLE_LOCK;
                enterState(ADMIN_PICK_LOCKER, Cfg::ADMIN_MENU_TIMEOUT_MS);
            } else if (key == '3') {
                _adminOp = OP_RELEASE;
                enterState(ADMIN_PICK_LOCKER, Cfg::ADMIN_MENU_TIMEOUT_MS);
            } else if (key == '4') {
                _inputBuf = "";
                enterState(ADMIN_SET_RFID_UID, Cfg::ADMIN_MENU_TIMEOUT_MS);
            } else if (key == '5') {
                _lockers->overwriteClearExpired(-1, "ADMIN_OVERWRITE");
                _hw->beep(80);
                refreshCurrentScreen(true);
            } else if (key == '6') {
                _net->requestUserSync();
                _hw->beep(80);
                refreshCurrentScreen(true);
            }
            break;
        }

        case ADMIN_PICK_LOCKER: {
            if (key == 'C') {
                enterState(ADMIN_MENU, Cfg::ADMIN_MENU_TIMEOUT_MS);
                return;
            }
            if (key >= '1' && key <= ('0' + Cfg::LOCKER_COUNT)) {
                const uint8_t idx = static_cast<uint8_t>(key - '1');
                if (_adminOp == OP_ASSIGN_UNREG) {
                    _lockers->adminAssignUnregistered(idx, "ADMIN_GUEST");
                } else if (_adminOp == OP_TOGGLE_LOCK) {
                    _lockers->remoteSetLock(idx, !_lockers->isLocked(idx), "ADMIN_MENU");
                } else if (_adminOp == OP_RELEASE) {
                    _lockers->release(idx, "ADMIN_MENU");
                }
                _hw->beep(70);
                enterState(ADMIN_MENU, Cfg::ADMIN_MENU_TIMEOUT_MS);
            }
            break;
        }

        case ADMIN_SET_RFID_UID: {
            if (key == 'C') {
                _inputBuf = "";
                enterState(ADMIN_MENU, Cfg::ADMIN_MENU_TIMEOUT_MS);
                return;
            }
            if (key >= '0' && key <= '9') {
                if (_inputBuf.length() < 20) {
                    _inputBuf += key;
                    refreshCurrentScreen(true);
                }
            } else if (key == '*') {
                if (!_inputBuf.isEmpty()) {
                    _inputBuf.remove(_inputBuf.length() - 1);
                    refreshCurrentScreen(true);
                }
            } else if (key == '#') {
                if (!_inputBuf.isEmpty()) {
                    _pendingUserId = _inputBuf;
                    _inputBuf = "";
                    enterState(ADMIN_SET_RFID_TAP, Cfg::ADMIN_MENU_TIMEOUT_MS);
                }
            }
            break;
        }

        case ADMIN_SET_RFID_TAP: {
            if (key == 'C') {
                enterState(ADMIN_MENU, Cfg::ADMIN_MENU_TIMEOUT_MS);
            }
            break;
        }

        default:
            break;
    }
}

void UserInterface::onRfidScanned(const String& uid) {
    if (_verifyingAsync) return;
    touchActivity();

    if (_state == ASSIGN_CRED) {
        String lockerName;
        const ActionStatus st = _lockers->assignByUid(uid, lockerName);
        if (st == ActionStatus::OK) {
            showResult("ASSIGNED", lockerName + " Opened", false);
        } else if (st == ActionStatus::UNAUTHORIZED) {
            showQrPrompt("Unregistered Card:");
        } else if (st == ActionStatus::ALREADY_ASSIGNED) {
            showResult("DENIED", "Already has " + lockerName, true);
        } else if (st == ActionStatus::NO_LOCKER_FREE) {
            showResult("FULL", "No free locker", true);
        } else if (st == ActionStatus::OFFLINE_BLOCKED) {
            showQrPrompt("Offline! Web Reg:");
        } else {
            showResult("ERROR", "Actuator fault", true);
        }
        return;
    }

    if (_state == UNLOCK_CRED) {
        String userId;
        const ActionStatus st = _lockers->verifyUnlockByUid(_selectedLocker, uid, userId);
        if (st == ActionStatus::OK || st == ActionStatus::EXPIRED) {
            _pendingUserId = userId;
            _hw->beep(60);
            enterState(UNLOCK_ACTION, Cfg::UI_IDLE_TIMEOUT_MS);
        } else {
            showQrPrompt("Card Mismatch/Reg:");
        }
        return;
    }

    if (_state == RFID_OTP_TAP) {
        String boundUser;
        const ActionStatus st = _lockers->completeOtpRfidRegistration(_activeOtp, uid, boundUser);
        if (st == ActionStatus::OK) {
            showResult("RFID SAVED", "User " + boundUser + " synced", false);
        } else if (st == ActionStatus::ALREADY_ASSIGNED) {
            showResult("CARD IN USE", "Already registered", true);
        } else {
            showResult("OTP EXPIRED", "Request new OTP", true);
        }
        return;
    }

    if (_state == ADMIN_SETUP_RFID) {
        if (_lockers->provisionAdmin(_setupAdminPin, uid)) {
            _setupAdminPin = "";
            _hw->beep(100);
            enterState(ADMIN_MENU, Cfg::ADMIN_MENU_TIMEOUT_MS);
        } else {
            showResult("SETUP FAIL", "Invalid Admin PIN", true);
        }
        return;
    }

    if (_state == ADMIN_AUTH_RFID) {
        if (_lockers->verifyAdminRfid(uid)) {
            _hw->beep(90);
            enterState(ADMIN_MENU, Cfg::ADMIN_MENU_TIMEOUT_MS);
        } else {
            showResult("DENIED", "Invalid Admin Card", true);
        }
        return;
    }

    if (_state == ADMIN_SET_RFID_TAP) {
        const ActionStatus st = _lockers->adminSetUserRfid(_pendingUserId, uid);
        _hw->beep(st == ActionStatus::OK ? 80 : 180);
        enterState(ADMIN_MENU, Cfg::ADMIN_MENU_TIMEOUT_MS);
    }
}

void UserInterface::refreshCurrentScreen(bool fullRedraw) {
    char shortTime[16];
    _clock->formatShortLocal(shortTime, sizeof(shortTime));
    const bool wifiOk = _net->wifiConnected();
    const bool mqttOk = _net->mqttConnected();
    const uint16_t secLeft = remainingSeconds();

    if (!fullRedraw) {
        _hw->updateTopClock(wifiOk, mqttOk, shortTime);
        if (_stateTimeoutMs > 0) {
            uint16_t totalSec = static_cast<uint16_t>(_stateTimeoutMs / 1000);
            _hw->drawCountdownBadge(secLeft, totalSec);
        }
        return;
    }

    LockerRecord recs[Cfg::LOCKER_COUNT];
    bool locked[Cfg::LOCKER_COUNT];
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        recs[i] = _lockers->record(i);
        locked[i] = _lockers->isLocked(i);
    }

    switch (_state) {
        case HOME:
            _hw->drawHome(wifiOk, mqttOk, shortTime,
                          _lockers->availableCount(), _lockers->problematicCount());
            break;
        case QR_REGISTER:
            _hw->drawQrRegister(wifiOk, mqttOk, shortTime, Cfg::APP_REGISTER_URL,
                                _pendingDetail.c_str(), secLeft);
            break;
        case ASSIGN_CRED:
            _hw->drawCredentialPrompt(wifiOk, mqttOk, shortTime,
                                      "ASSIGN LOCKER", "Tap RFID or PIN+#:",
                                      _inputBuf.length(), secLeft);
            break;
        case UNLOCK_SELECT:
            _hw->drawLockerSelector(wifiOk, mqttOk, shortTime,
                                    recs, locked, _selectedLocker, secLeft);
            break;
        case UNLOCK_CRED:
            _hw->drawCredentialPrompt(wifiOk, mqttOk, shortTime,
                                      Cfg::LOCKER_IDS[_selectedLocker], "Tap RFID or PIN+#:",
                                      _inputBuf.length(), secLeft);
            break;
        case UNLOCK_ACTION:
            _hw->drawUnlockActionPrompt(wifiOk, mqttOk, shortTime, _selectedLocker, secLeft);
            break;
        case D_MENU_PIN:
            _hw->drawDMenuFirstPage(wifiOk, mqttOk, shortTime,
                                    Cfg::APP_REGISTER_URL, _inputBuf.length(), secLeft);
            break;
        case RFID_OTP_TAP:
            _hw->drawTapCardPrompt(wifiOk, mqttOk, shortTime,
                                   "BIND USER RFID", ("User: " + _pendingUserId).c_str(), secLeft);
            break;
        case ADMIN_SETUP_PIN:
            _hw->drawCredentialPrompt(wifiOk, mqttOk, shortTime,
                                      "INIT ADMIN PIN", "Set 4-8 digit PIN:",
                                      _inputBuf.length(), secLeft);
            break;
        case ADMIN_SETUP_RFID:
            _hw->drawTapCardPrompt(wifiOk, mqttOk, shortTime,
                                   "INIT ADMIN CARD", "Tap master RFID card", secLeft);
            break;
        case ADMIN_AUTH_RFID:
            _hw->drawTapCardPrompt(wifiOk, mqttOk, shortTime,
                                   "ADMIN 2FA", "Tap Admin RFID card", secLeft);
            break;
        case ADMIN_MENU:
            _hw->drawAdminMenu(wifiOk, mqttOk, shortTime,
                               recs, locked, _lockers->registeredUserCount(), secLeft);
            break;
        case ADMIN_PICK_LOCKER:
            _hw->drawAdminTargetPrompt(wifiOk, mqttOk, shortTime,
                                       "SELECT LOCKER", "Press 1-4 (C:Back):",
                                       "", secLeft);
            break;
        case ADMIN_SET_RFID_UID:
            _hw->drawAdminTargetPrompt(wifiOk, mqttOk, shortTime,
                                       "SET USER RFID", "Enter User ID + #:",
                                       _inputBuf, secLeft);
            break;
        case ADMIN_SET_RFID_TAP:
            _hw->drawTapCardPrompt(wifiOk, mqttOk, shortTime,
                                   "SET USER RFID", ("User: " + _pendingUserId).c_str(), secLeft);
            break;
        case RESULT:
            _hw->drawResult(wifiOk, mqttOk, shortTime,
                            _pendingTitle, _pendingDetail, _pendingError, secLeft);
            break;
    }
}
