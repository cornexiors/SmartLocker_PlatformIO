#include "App.h"
#include <cstring>

void LockerManager::begin(Storage& storage, Hardware& hardware, NetworkService& network) {
    _storage = &storage;
    _hardware = &hardware;
    _network = &network;

    _storage->loadLockers(_records);

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        _locked[i] = true;
        if (!_hardware->setLockerActuator(i, true)) {
            Serial.printf("WARN: Servo init failed for %s\n", Cfg::LOCKER_IDS[i]);
        }
    }
}

void LockerManager::saveLocker(uint8_t index) {
    if (_storage) _storage->saveLocker(index, _records[index]);
}

void LockerManager::publishFullLockerState(uint8_t index, const char* method) {
    if (!_network || index >= Cfg::LOCKER_COUNT) return;
    _network->publishLockerState(
        index,
        _locked[index],
        _records[index].assigned != 0,
        method
    );
}

void LockerManager::broadcastAllStates(const char* method) {
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        publishFullLockerState(i, method);
    }
}

void LockerManager::emitEvent(const char* name, uint8_t index, const char* method) {
    char json[220];
    const char* locker = index < Cfg::LOCKER_COUNT ? Cfg::LOCKER_IDS[index] : "SYSTEM";
    const bool locked = index < Cfg::LOCKER_COUNT ? _locked[index] : true;
    const bool assigned = index < Cfg::LOCKER_COUNT ? (_records[index].assigned != 0) : false;

    snprintf(json, sizeof(json),
             "{\"device_id\":\"%s\",\"event\":\"%s\",\"locker_id\":\"%s\","
             "\"locked\":%s,\"assigned\":%s,\"method\":\"%s\"}",
             Cfg::DEVICE_ID, name, locker,
             locked ? "true" : "false",
             assigned ? "true" : "false",
             method);

    Serial.println(json);
    if (_network) {
        _network->publishEvent(String(json));
    }
}

bool LockerManager::setLock(uint8_t index, bool locked, const char* method) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    if (!_hardware->setLockerActuator(index, locked)) {
        Serial.printf("ERR: Actuator failed for %s\n", Cfg::LOCKER_IDS[index]);
        return false;
    }
    _locked[index] = locked;
    publishFullLockerState(index, method);
    return true;
}

// ============================================================
// QUERIES
// ============================================================

int LockerManager::findLockerId(const String& id) const {
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (id.equalsIgnoreCase(Cfg::LOCKER_IDS[i])) return i;
    }
    return -1;
}

uint8_t LockerManager::availableCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (_records[i].assigned == 0) ++count;
    }
    return count;
}

int LockerManager::findLockerByUid(const String& uid) const {
    if (uid.isEmpty()) return -1;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (_records[i].assigned && _records[i].uid[0] && uid.equalsIgnoreCase(_records[i].uid)) {
            return i;
        }
    }
    return -1;
}

int LockerManager::findLockerByPin(const String& pin) const {
    if (pin.isEmpty()) return -1;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (_records[i].assigned && _records[i].pin[0] && pin == _records[i].pin) {
            return i;
        }
    }
    return -1;
}

// ============================================================
// OFFLINE ASSIGN & SINGLE-USE UNLOCK-RELEASE FLOWS
// ============================================================

ActionStatus LockerManager::assignByUid(const String& uid, String& lockerName) {
    if (uid.isEmpty()) return ActionStatus::UNAUTHORIZED;

    // Enforce one active locker per RFID card at a time
    const int existing = findLockerByUid(uid);
    if (existing >= 0) {
        lockerName = Cfg::LOCKER_IDS[existing];
        return ActionStatus::ALREADY_ASSIGNED;
    }

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (_records[i].assigned != 0) continue;

        LockerRecord r{};
        r.assigned = 1;
        strlcpy(r.uid, uid.c_str(), sizeof(r.uid));
        _records[i] = r;
        saveLocker(i);

        if (!setLock(i, false, "RFID")) {
            _records[i] = LockerRecord{};
            saveLocker(i);
            return ActionStatus::HARDWARE_ERROR;
        }

        emitEvent("LOCKER_ASSIGNED", i, "RFID");
        lockerName = Cfg::LOCKER_IDS[i];
        return ActionStatus::OK;
    }

    return ActionStatus::NO_LOCKER_FREE;
}

ActionStatus LockerManager::assignByPin(const String& pin, String& lockerName) {
    if (pin.length() < Cfg::PIN_MIN_LENGTH || pin.length() > Cfg::PIN_MAX_LENGTH) {
        return ActionStatus::UNAUTHORIZED;
    }

    // Enforce one active locker per PIN at a time
    const int existing = findLockerByPin(pin);
    if (existing >= 0) {
        lockerName = Cfg::LOCKER_IDS[existing];
        return ActionStatus::ALREADY_ASSIGNED;
    }

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (_records[i].assigned != 0) continue;

        LockerRecord r{};
        r.assigned = 1;
        strlcpy(r.pin, pin.c_str(), sizeof(r.pin));
        _records[i] = r;
        saveLocker(i);

        if (!setLock(i, false, "PIN")) {
            _records[i] = LockerRecord{};
            saveLocker(i);
            return ActionStatus::HARDWARE_ERROR;
        }

        emitEvent("LOCKER_ASSIGNED", i, "PIN");
        lockerName = Cfg::LOCKER_IDS[i];
        return ActionStatus::OK;
    }

    return ActionStatus::NO_LOCKER_FREE;
}

ActionStatus LockerManager::unlockAndReleaseByUid(const String& uid, String& lockerName) {
    const int idx = findLockerByUid(uid);
    if (idx < 0) return ActionStatus::UNAUTHORIZED;

    lockerName = Cfg::LOCKER_IDS[idx];

    // Single-use assignment: clear record immediately on unlock
    _records[idx] = LockerRecord{};
    saveLocker(idx);

    if (!setLock(idx, false, "RFID")) {
        return ActionStatus::HARDWARE_ERROR;
    }

    emitEvent("LOCKER_UNLOCKED", idx, "RFID");
    return ActionStatus::OK;
}

ActionStatus LockerManager::unlockAndReleaseByPin(const String& pin, String& lockerName) {
    const int idx = findLockerByPin(pin);
    if (idx < 0) return ActionStatus::UNAUTHORIZED;

    lockerName = Cfg::LOCKER_IDS[idx];

    // Single-use assignment: clear record immediately on unlock
    _records[idx] = LockerRecord{};
    saveLocker(idx);

    if (!setLock(idx, false, "PIN")) {
        return ActionStatus::HARDWARE_ERROR;
    }

    emitEvent("LOCKER_UNLOCKED", idx, "PIN");
    return ActionStatus::OK;
}

// ============================================================
// REMOTE / SERIAL CONTROL & DOOR REED AUTO-LOCK
// ============================================================

bool LockerManager::remoteSetLock(uint8_t index, bool locked, const char* source) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    if (!setLock(index, locked, source)) return false;
    emitEvent(locked ? "REMOTE_LOCK" : "REMOTE_UNLOCK", index, source);
    return true;
}

bool LockerManager::release(uint8_t index, const char* source) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    _records[index] = LockerRecord{};
    saveLocker(index);
    if (!setLock(index, false, source)) return false;
    emitEvent("LOCKER_RELEASED", index, source);
    return true;
}

void LockerManager::resetAllLockers() {
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        _records[i] = LockerRecord{};
        setLock(i, true, "RESET");
    }
    _storage->clearAllLockers();
    emitEvent("ALL_LOCKERS_RESET", 0xFF, "SERIAL");
}

void LockerManager::tick() {
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        bool closed = false;
        if (_hardware->doorChanged(i, closed)) {
            Serial.printf("[REED] %s (GPIO %u) -> %s\n",
                          Cfg::LOCKER_IDS[i], Cfg::REED_PINS[i],
                          closed ? "CLOSED" : "OPEN");
            _hardware->beep(40);
            if (closed) {
                emitEvent("DOOR_CLOSED", i, "REED");
                if (setLock(i, true, "REED")) {
                    emitEvent("DOOR_CLOSED_AUTO_LOCKED", i, "REED");
                }
            } else {
                emitEvent("DOOR_OPENED", i, "REED");
            }
        }
    }
}