#include "App.h"

void UserInterface::begin(Hardware& hw, LockerManager& lockers, ClockService& clock, NetworkService& net) {
  _hw = &hw; _lockers = &lockers; _clock = &clock; _net = &net;
  returnHome();
}

void UserInterface::finishResult(const String& a, const String& b) {
  _resultA = a; _resultB = b; _state = RESULT;
  _resultDeadline = millis() + 2800;
  _hw->drawResult(_resultA, _resultB);
}

void UserInterface::returnHome() {
  _state = HOME; _pin = ""; _assigning = false; _useRfid = false;
  char timeText[24]; _clock->formatLocal(timeText, sizeof(timeText));
  _hw->drawHome(_net->online(), static_cast<uint8_t>(_lockers->availableCount()), timeText);
  _lastUiRefresh = millis();
}

void UserInterface::onKey(char key) {
  if (_state == HOME) { _state = MENU; _hw->drawMenu(); return; }
  if (_state == RESULT) { returnHome(); return; }
  if (key == 'C') {
    if (_state == MENU) returnHome();
    else if (_state == AUTH_METHOD) { _state = MENU; _hw->drawMenu(); }
    else if (_state == WAIT_RFID || _state == PIN_ENTRY) { _pin = ""; _state = AUTH_METHOD; _hw->drawAuth(_assigning, false); }
    return;
  }
  if (_state == MENU) {
    if (key == 'A') _assigning = true;
    else if (key == 'B') _assigning = false;
    else return;
    _state = AUTH_METHOD; _hw->drawAuth(_assigning, false); return;
  }
  if (_state == AUTH_METHOD) {
    if (key == 'A') {
      _useRfid = true; _state = WAIT_RFID; _hw->drawWaitRfid(_assigning);
      if (!_lastUid.isEmpty() && millis() - _lastUidAt < 60000) {
        String locker;
        const bool ok = _assigning ? _lockers->assignByUid(_lastUid, locker) : _lockers->unlockByUid(_lastUid, locker);
        finishResult(ok ? (_assigning ? "LOCKER ASSIGNED" : "LOCKER OPENED") : "ACCESS DENIED", ok ? locker : "Card not available");
      }
    } else if (key == 'B') { _useRfid = false; _pin = ""; _state = PIN_ENTRY; _hw->drawPin(_assigning, _pin); }
    return;
  }
  if (_state == PIN_ENTRY) {
    if (key >= '0' && key <= '9' && _pin.length() < Cfg::PIN_MAX_LENGTH) {
      _pin += key;
      String mask; for (size_t i = 0; i < _pin.length(); ++i) mask += '*';
      _hw->drawPin(_assigning, mask);
    } else if (key == '*') {
      if (!_pin.isEmpty()) _pin.remove(_pin.length() - 1);
      String mask; for (size_t i = 0; i < _pin.length(); ++i) mask += '*';
      _hw->drawPin(_assigning, mask);
    } else if (key == '#') {
      if (_pin.length() < Cfg::PIN_MIN_LENGTH) { finishResult("PIN TOO SHORT", "Use 4-8 digits"); return; }
      String locker;
      const bool ok = _assigning ? _lockers->assignByPin(_pin, locker) : _lockers->unlockByPin(_pin, locker);
      _pin = "";
      finishResult(ok ? (_assigning ? "LOCKER ASSIGNED" : "LOCKER OPENED") : "ACCESS DENIED", ok ? locker : "PIN invalid / expired");
    }
  }
}

void UserInterface::tick() {
  String uid;
  if (_hw->readCard(uid)) {
    _lastUid = uid; _lastUidAt = millis();
    Serial.printf("RFID UID: %s\n", uid.c_str());
    if (_state == WAIT_RFID) {
      String locker;
      const bool ok = _assigning ? _lockers->assignByUid(uid, locker) : _lockers->unlockByUid(uid, locker);
      finishResult(ok ? (_assigning ? "LOCKER ASSIGNED" : "LOCKER OPENED") : "ACCESS DENIED", ok ? locker : "Card invalid / expired");
    }
  }

  const uint32_t now = millis();
  if (now - _lastKeyScan >= Cfg::KEYPAD_SCAN_MS) {
    _lastKeyScan = now;
    const char key = _hw->scanKey();
    if (key == _rawKey) { if (_keyStable < 2) ++_keyStable; }
    else { _rawKey = key; _keyStable = 0; }
    if (_keyStable >= 2 && _stableKey != _rawKey) {
      _stableKey = _rawKey;
      if (_stableKey) onKey(_stableKey);
    }
  }

  if (_state == RESULT && static_cast<int32_t>(now - _resultDeadline) >= 0) returnHome();
  if (_state == HOME && now - _lastUiRefresh >= Cfg::UI_REFRESH_MS) {
    char timeText[24]; _clock->formatLocal(timeText, sizeof(timeText));
    _hw->updateHome(_net->online(), static_cast<uint8_t>(_lockers->availableCount()), timeText);
    _lastUiRefresh = now;
  }
}
