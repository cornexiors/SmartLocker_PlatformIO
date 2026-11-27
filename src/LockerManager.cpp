#include "App.h"
#include <cstring>

void LockerManager::begin(Storage& storage, Hardware& hardware,
                          ClockService& clock, NetworkService& network) {
    _storage = &storage;
    _hardware = &hardware;
    _clock = &clock;
    _network = &network;
    _stateMutex = xSemaphoreCreateMutex();

    _storage->loadLockers(_records);
    _storage->loadUsers(_users);
    _storage->loadAdminConfig(_adminCfg);

    // Clear D-menu admin credentials automatically on fresh firmware binary flash
    const char* currentBuild = __DATE__ " " __TIME__;
    if (_adminCfg.provisioned && strcmp(_adminCfg.buildSig, currentBuild) != 0) {
        _storage->clearAdminConfig();
        memset(&_adminCfg, 0, sizeof(AdminConfig));
        appLog("INFO", "New firmware build detected; D-menu admin provisioning reset");
    }

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        const bool shouldLock = isAssigned(_records[i]);
        _locked[i] = shouldLock;
        _hardware->setLockerActuator(i, shouldLock);
    }
}

void LockerManager::tick() {
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        bool closed = true;
        if (_hardware->doorChanged(i, closed)) {
            if (closed) {
                _doorOpenedAt[i] = 0;
                _doorTimeoutReported[i] = false;
                if (!_locked[i] && isAssigned(_records[i])) {
                    setLock(i, true, "REED_AUTO_LOCK");
                    emitEvent("DOOR_CLOSED_AUTO_LOCKED", i, "REED_SENSOR", _records[i].userId);
                    publishFullLockerState(i, "REED_AUTO_LOCK");
                }
            } else {
                _doorOpenedAt[i] = millis();
                _lastDoorAlertAt[i] = 0;
                _doorTimeoutReported[i] = false;
            }
        }
    }

    const uint32_t now = millis();
    if (now - _lastTimerCheck >= 1000) {
        _lastTimerCheck = now;
        checkExpirationsAndDoorAlerts();
    }
}

void LockerManager::checkExpirationsAndDoorAlerts() {
    const uint32_t nowMs = millis();
    const uint64_t nowEpoch = _clock->valid() ? _clock->nowUtc() : 0;

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (!_hardware->doorClosed(i) && _doorOpenedAt[i] > 0 &&
            (nowMs - _doorOpenedAt[i] >= Cfg::DOOR_OPEN_ALERT_MS)) {
            if (_lastDoorAlertAt[i] == 0 || (nowMs - _lastDoorAlertAt[i] >= Cfg::DOOR_ALERT_REPEAT_MS)) {
                _lastDoorAlertAt[i] = nowMs;
                _hardware->beep(150);
                if (!_doorTimeoutReported[i]) {
                    _doorTimeoutReported[i] = true;
                    emitEvent("DOOR_OPEN_TIMEOUT", i, "REED_SENSOR", _records[i].userId);
                }
            }
        }

        if (nowEpoch > 0 && isAssigned(_records[i]) && !isExpiredHold(_records[i]) &&
            _records[i].expiresAt > 0 && nowEpoch >= _records[i].expiresAt) {
            xSemaphoreTake(_stateMutex, portMAX_DELAY);
            _records[i].flags |= 0x02;
            saveLocker(i);
            xSemaphoreGive(_stateMutex);

            emitEvent("LOCKER_EXPIRED", i, "TIMER", _records[i].userId);
            publishFullLockerState(i, "TIMER_EXPIRED");
        }
    }
}

// ============================================================
// USER ASSIGNMENT & UNLOCK FLOWS (SYMMETRIC ONLINE/OFFLINE)
// ============================================================

ActionStatus LockerManager::assignByUid(const String& uid, String& lockerName) {
    if (!_network->online()) return ActionStatus::OFFLINE_BLOCKED;
    const int userSlot = findRegisteredUserByUid(uid);
    if (userSlot < 0) return ActionStatus::UNAUTHORIZED;
    return assignByUserSlot(userSlot, "RFID", lockerName);
}

ActionStatus LockerManager::assignByUserSlot(int userSlot, const char* method, String& lockerName) {
    if (!_network->online()) return ActionStatus::OFFLINE_BLOCKED;

    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    if (userSlot < 0 || userSlot >= Cfg::MAX_USERS || !_users[userSlot].active) {
        xSemaphoreGive(_stateMutex);
        return ActionStatus::UNAUTHORIZED;
    }

    const UserRecord u = _users[userSlot];
    const int existing = findAssignedLockerByUserId(String(u.userId));
    if (existing >= 0) {
        lockerName = Cfg::LOCKER_IDS[existing];
        xSemaphoreGive(_stateMutex);
        return ActionStatus::ALREADY_ASSIGNED;
    }

    int freeIdx = -1;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (!isAssigned(_records[i])) {
            freeIdx = i;
            break;
        }
    }
    if (freeIdx < 0) {
        xSemaphoreGive(_stateMutex);
        return ActionStatus::NO_LOCKER_FREE;
    }

    LockerRecord& rec = _records[freeIdx];
    memset(&rec, 0, sizeof(LockerRecord));
    rec.flags = 0x01;
    strncpy(rec.userId, u.userId, sizeof(rec.userId) - 1);
    strncpy(rec.uid, u.uid, sizeof(rec.uid) - 1);
    strncpy(rec.salt, u.salt, sizeof(rec.salt) - 1);
    strncpy(rec.pinHash, u.pinHash, sizeof(rec.pinHash) - 1);
    rec.assignedAt = _clock->valid() ? _clock->nowUtc() : 0;
    rec.expiresAt = rec.assignedAt > 0 ? (rec.assignedAt + Cfg::RENTAL_SECONDS) : 0;
    saveLocker(freeIdx);
    xSemaphoreGive(_stateMutex);

    if (!setLock(freeIdx, false, method)) return ActionStatus::HARDWARE_ERROR;
    lockerName = Cfg::LOCKER_IDS[freeIdx];

    emitEvent("LOCKER_ASSIGNED", freeIdx, method, u.userId);
    publishFullLockerState(freeIdx, method);
    return ActionStatus::OK;
}

ActionStatus LockerManager::verifyUnlockByUid(uint8_t lockerIdx, const String& uid, String& outUserId) {
    if (lockerIdx >= Cfg::LOCKER_COUNT) return ActionStatus::UNAUTHORIZED;
    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    const LockerRecord rec = _records[lockerIdx];
    if (!isAssigned(rec) || rec.userId[0] == '\0') {
        xSemaphoreGive(_stateMutex);
        return ActionStatus::UNAUTHORIZED;
    }

    String targetUid = String(rec.uid);
    const int userSlot = findRegisteredUserByUserId(String(rec.userId));
    if (userSlot >= 0 && _users[userSlot].uid[0] != '\0') {
        targetUid = String(_users[userSlot].uid);
    }

    if (targetUid.isEmpty() || !targetUid.equalsIgnoreCase(uid)) {
        xSemaphoreGive(_stateMutex);
        return ActionStatus::UNAUTHORIZED;
    }

    outUserId = String(rec.userId);
    const bool expired = isExpiredHold(rec);
    xSemaphoreGive(_stateMutex);
    return expired ? ActionStatus::EXPIRED : ActionStatus::OK;
}

bool LockerManager::verifyUnlockByUserSlot(uint8_t lockerIdx, int userSlot, String& outUserId) {
    if (lockerIdx >= Cfg::LOCKER_COUNT) return false;
    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    const LockerRecord rec = _records[lockerIdx];
    bool ok = false;
    if (isAssigned(rec) && userSlot >= 0 && userSlot < Cfg::MAX_USERS && _users[userSlot].active) {
        if (strcmp(rec.userId, _users[userSlot].userId) == 0) {
            outUserId = String(rec.userId);
            ok = true;
        }
    }
    xSemaphoreGive(_stateMutex);
    return ok;
}

ActionStatus LockerManager::executeUnlockFinish(uint8_t lockerIdx, const char* method, String& lockerName) {
    if (lockerIdx >= Cfg::LOCKER_COUNT) return ActionStatus::UNAUTHORIZED;
    if (!setLock(lockerIdx, false, method)) return ActionStatus::HARDWARE_ERROR;

    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    char prevUser[25] = {};
    strncpy(prevUser, _records[lockerIdx].userId, sizeof(prevUser) - 1);
    memset(&_records[lockerIdx], 0, sizeof(LockerRecord));
    saveLocker(lockerIdx);
    xSemaphoreGive(_stateMutex);

    lockerName = Cfg::LOCKER_IDS[lockerIdx];
    emitEvent("LOCKER_RELEASED", lockerIdx, method, prevUser);
    publishFullLockerState(lockerIdx, method);
    return ActionStatus::OK;
}

ActionStatus LockerManager::executeUnlockExtend(uint8_t lockerIdx, const char* method, String& lockerName) {
    if (lockerIdx >= Cfg::LOCKER_COUNT) return ActionStatus::UNAUTHORIZED;
    if (!setLock(lockerIdx, false, method)) return ActionStatus::HARDWARE_ERROR;

    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    LockerRecord& rec = _records[lockerIdx];
    rec.flags &= ~0x02; // Clear any overdue flag on explicit extension
    if (_clock->valid()) {
        rec.expiresAt = _clock->nowUtc() + Cfg::RENTAL_SECONDS;
    }
    saveLocker(lockerIdx);
    char uidCopy[25] = {};
    strncpy(uidCopy, rec.userId, sizeof(uidCopy) - 1);
    xSemaphoreGive(_stateMutex);

    lockerName = Cfg::LOCKER_IDS[lockerIdx];
    // Emit LOCKER_ASSIGNED so backend keeps ownership in DEPOSIT_OPEN until door re-locks
    emitEvent("LOCKER_ASSIGNED", lockerIdx, method, uidCopy);
    publishFullLockerState(lockerIdx, method);
    return ActionStatus::OK;
}

// ============================================================
// D-MENU RFID OTP & ADMIN RFID REGISTRATION
// ============================================================

bool LockerManager::registerPendingOtp(const String& userId, const String& otp) {
    if (userId.isEmpty() || otp.length() != Cfg::RFID_OTP_LENGTH) return false;

    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    int slot = -1;
    for (uint8_t i = 0; i < Cfg::MAX_PENDING_OTPS; ++i) {
        if (_otps[i].active && strcmp(_otps[i].userId, userId.c_str()) == 0) {
            slot = i;
            break;
        }
        if (!_otps[i].active && slot < 0) slot = i;
    }
    if (slot < 0) slot = 0;

    _otps[slot].active = true;
    strncpy(_otps[slot].userId, userId.c_str(), sizeof(_otps[slot].userId) - 1);
    strncpy(_otps[slot].otp, otp.c_str(), sizeof(_otps[slot].otp) - 1);
    _otps[slot].expiresAtMs = millis() + Cfg::OTP_TTL_MS;
    xSemaphoreGive(_stateMutex);
    return true;
}

bool LockerManager::verifyOtp(const String& otp, String& outUserId) {
    if (otp.length() != Cfg::RFID_OTP_LENGTH) return false;

    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    // Guard: never treat Admin PIN as a user RFID OTP
    if (_adminCfg.provisioned &&
        Security::verifyPinHash(_adminCfg.pinSalt, otp, _adminCfg.pinHash)) {
        xSemaphoreGive(_stateMutex);
        return false;
    }

    const uint32_t now = millis();
    bool found = false;
    for (uint8_t i = 0; i < Cfg::MAX_PENDING_OTPS; ++i) {
        if (_otps[i].active) {
            if (static_cast<int32_t>(now - _otps[i].expiresAtMs) >= 0) {
                _otps[i].active = false;
                continue;
            }
            if (otp == _otps[i].otp) {
                outUserId = String(_otps[i].userId);
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(_stateMutex);
    return found;
}

ActionStatus LockerManager::completeOtpRfidRegistration(const String& otp, const String& uid, String& outUserId) {
    if (!verifyOtp(otp, outUserId)) return ActionStatus::UNAUTHORIZED;
    const ActionStatus st = adminSetUserRfid(outUserId, uid);
    if (st == ActionStatus::OK) {
        xSemaphoreTake(_stateMutex, portMAX_DELAY);
        for (uint8_t i = 0; i < Cfg::MAX_PENDING_OTPS; ++i) {
            if (_otps[i].active && outUserId == _otps[i].userId) {
                _otps[i].active = false;
            }
        }
        xSemaphoreGive(_stateMutex);
    }
    return st;
}

ActionStatus LockerManager::adminSetUserRfid(const String& userId, const String& uid) {
    String cleanUid = uid;
    cleanUid.trim();
    cleanUid.toUpperCase();
    if (userId.isEmpty() || cleanUid.isEmpty()) return ActionStatus::UNAUTHORIZED;

    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    // Reject if RFID UID is already bound to a different user
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        if (_users[i].active && cleanUid.equalsIgnoreCase(_users[i].uid) &&
            userId != _users[i].userId) {
            xSemaphoreGive(_stateMutex);
            return ActionStatus::ALREADY_ASSIGNED;
        }
    }

    int slot = findRegisteredUserByUserId(userId);
    if (slot < 0) {
        for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
            if (!_users[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        xSemaphoreGive(_stateMutex);
        return ActionStatus::NO_LOCKER_FREE;
    }

    _users[slot].active = 1;
    strncpy(_users[slot].userId, userId.c_str(), sizeof(_users[slot].userId) - 1);
    strncpy(_users[slot].uid, cleanUid.c_str(), sizeof(_users[slot].uid) - 1);
    _storage->saveUser(slot, _users[slot]);

    // Also update active locker record if this user currently occupies a compartment
    const int lockerIdx = findAssignedLockerByUserId(userId);
    if (lockerIdx >= 0) {
        strncpy(_records[lockerIdx].uid, cleanUid.c_str(), sizeof(_records[lockerIdx].uid) - 1);
        saveLocker(lockerIdx);
    }
    xSemaphoreGive(_stateMutex);

    if (!_network->publishRfidRegAck(userId, cleanUid, true, "REGISTERED")) {
        char queued[128];
        snprintf(queued, sizeof(queued),
                 "{\"location_id\":\"%s\",\"device_id\":\"%s\",\"event\":\"RFID_REGISTERED\","
                 "\"user_id\":\"%s\",\"rfid_uid\":\"%s\"}",
                 Cfg::LOCATION_ID, Cfg::DEVICE_ID, userId.c_str(), cleanUid.c_str());
        _storage->enqueue(String(queued));
    }
    return ActionStatus::OK;
}

// ============================================================
// ADMIN & REMOTE CONTROLS
// ============================================================

bool LockerManager::adminAssignUnregistered(uint8_t index, const String& label) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    LockerRecord& rec = _records[index];
    memset(&rec, 0, sizeof(LockerRecord));
    rec.flags = 0x01;
    strncpy(rec.userId, label.c_str(), sizeof(rec.userId) - 1);
    rec.assignedAt = _clock->valid() ? _clock->nowUtc() : 0;
    rec.expiresAt = rec.assignedAt > 0 ? (rec.assignedAt + Cfg::RENTAL_SECONDS) : 0;
    saveLocker(index);
    xSemaphoreGive(_stateMutex);

    setLock(index, false, "ADMIN_ASSIGN");
    emitEvent("LOCKER_ASSIGNED", index, "ADMIN", label.c_str());
    publishFullLockerState(index, "ADMIN_ASSIGN");
    return true;
}

bool LockerManager::remoteSetLock(uint8_t index, bool locked, const char* source) {
    if (!setLock(index, locked, source)) return false;
    emitEvent(locked ? "LOCKER_LOCKED" : "LOCKER_UNLOCKED", index, source, _records[index].userId);
    publishFullLockerState(index, source);
    return true;
}

bool LockerManager::release(uint8_t index, const char* source) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    setLock(index, false, source);

    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    char prevUser[25] = {};
    strncpy(prevUser, _records[index].userId, sizeof(prevUser) - 1);
    memset(&_records[index], 0, sizeof(LockerRecord));
    saveLocker(index);
    xSemaphoreGive(_stateMutex);

    emitEvent("LOCKER_RELEASED", index, source, prevUser);
    publishFullLockerState(index, source);
    return true;
}

void LockerManager::overwriteClearExpired(int indexOrAll, const char* source) {
    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (indexOrAll < 0 || indexOrAll == i) {
            if (isExpiredHold(_records[i])) {
                _records[i].flags &= ~0x02;
                if (_clock->valid()) {
                    _records[i].expiresAt = _clock->nowUtc() + Cfg::RENTAL_SECONDS;
                }
                saveLocker(i);
            }
        }
    }
    xSemaphoreGive(_stateMutex);
    broadcastAllStates(source);
}

void LockerManager::broadcastAllStates(const char* method) {
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        publishFullLockerState(i, method);
    }
}

// ============================================================
// HARDWARE ADMIN PROVISIONING & 2FA
// ============================================================

bool LockerManager::isAdminProvisioned() const {
    return _adminCfg.provisioned == 1;
}

bool LockerManager::provisionAdmin(const String& pin, const String& rfidUid) {
    if (pin.length() < Cfg::PIN_MIN_LENGTH || pin.length() > Cfg::PIN_MAX_LENGTH || rfidUid.isEmpty()) {
        return false;
    }
    const String salt = Security::generateSaltHex();
    const String hash = Security::makePinHash(salt.c_str(), pin);
    if (hash.isEmpty()) return false;

    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    memset(&_adminCfg, 0, sizeof(AdminConfig));
    _adminCfg.provisioned = 1;
    strncpy(_adminCfg.buildSig, __DATE__ " " __TIME__, sizeof(_adminCfg.buildSig) - 1);
    String cleanUid = rfidUid;
    cleanUid.toUpperCase();
    strncpy(_adminCfg.rfidUid, cleanUid.c_str(), sizeof(_adminCfg.rfidUid) - 1);
    strncpy(_adminCfg.pinSalt, salt.c_str(), sizeof(_adminCfg.pinSalt) - 1);
    strncpy(_adminCfg.pinHash, hash.c_str(), sizeof(_adminCfg.pinHash) - 1);
    _storage->saveAdminConfig(_adminCfg);
    xSemaphoreGive(_stateMutex);
    return true;
}

bool LockerManager::verifyAdminRfid(const String& rfidUid) const {
    if (!_adminCfg.provisioned || rfidUid.isEmpty()) return false;
    return rfidUid.equalsIgnoreCase(_adminCfg.rfidUid);
}

void LockerManager::resetAdminProvisioning() {
    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    memset(&_adminCfg, 0, sizeof(AdminConfig));
    _storage->clearAdminConfig();
    xSemaphoreGive(_stateMutex);
}

// ============================================================
// USER DATABASE SYNC & ASYNC CORE 0 AUTH WORKER
// ============================================================

bool LockerManager::upsertUser(const String& userId, const String& uid,
                               const String& salt, const String& pinHash) {
    if (userId.isEmpty() || salt.isEmpty() || pinHash.length() != 64) return false;

    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    int slot = findRegisteredUserByUserId(userId);
    if (slot < 0) {
        for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
            if (!_users[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        xSemaphoreGive(_stateMutex);
        return false;
    }

    _users[slot].active = 1;
    strncpy(_users[slot].userId, userId.c_str(), sizeof(_users[slot].userId) - 1);
    String cleanUid = uid;
    cleanUid.toUpperCase();
    strncpy(_users[slot].uid, cleanUid.c_str(), sizeof(_users[slot].uid) - 1);
    strncpy(_users[slot].salt, salt.c_str(), sizeof(_users[slot].salt) - 1);
    strncpy(_users[slot].pinHash, pinHash.c_str(), sizeof(_users[slot].pinHash) - 1);
    _storage->saveUser(slot, _users[slot]);
    xSemaphoreGive(_stateMutex);
    return true;
}

bool LockerManager::deleteUser(const String& userId) {
    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    const int slot = findRegisteredUserByUserId(userId);
    if (slot < 0) {
        xSemaphoreGive(_stateMutex);
        return false;
    }
    memset(&_users[slot], 0, sizeof(UserRecord));
    _storage->saveUser(slot, _users[slot]);
    xSemaphoreGive(_stateMutex);
    return true;
}

void LockerManager::clearUsers() {
    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    memset(_users, 0, sizeof(_users));
    _storage->clearAllUsers();
    xSemaphoreGive(_stateMutex);
}

bool LockerManager::assignFromBackend(const String& lockerId, const String& userId, bool open) {
    const int idx = findLockerId(lockerId);
    if (idx < 0 || userId.isEmpty()) return false;

    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    const int uSlot = findRegisteredUserByUserId(userId);
    LockerRecord& rec = _records[idx];
    memset(&rec, 0, sizeof(LockerRecord));
    rec.flags = 0x01;
    strncpy(rec.userId, userId.c_str(), sizeof(rec.userId) - 1);
    if (uSlot >= 0) {
        strncpy(rec.uid, _users[uSlot].uid, sizeof(rec.uid) - 1);
        strncpy(rec.salt, _users[uSlot].salt, sizeof(rec.salt) - 1);
        strncpy(rec.pinHash, _users[uSlot].pinHash, sizeof(rec.pinHash) - 1);
    }
    rec.assignedAt = _clock->valid() ? _clock->nowUtc() : 0;
    rec.expiresAt = rec.assignedAt > 0 ? (rec.assignedAt + Cfg::RENTAL_SECONDS) : 0;
    saveLocker(idx);
    xSemaphoreGive(_stateMutex);

    if (open) setLock(idx, false, "BACKEND_ASSIGN");
    publishFullLockerState(idx, "BACKEND_ASSIGN");
    return true;
}

bool LockerManager::unassignFromBackend(const String& lockerId, const String& userId) {
    const int idx = findLockerId(lockerId);
    if (idx < 0) return false;
    return release(idx, "BACKEND_UNASSIGN");
}

void LockerManager::clearAssignments() {
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (isAssigned(_records[i])) release(i, "BACKEND_UNASSIGN_ALL");
    }
}

uint8_t LockerManager::registeredUserCount() const {
    uint8_t cnt = 0;
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        if (_users[i].active) ++cnt;
    }
    return cnt;
}

void LockerManager::listUsersToSerial(Stream& out) const {
    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    out.printf("--- SYNCED USERS (%u/%u) ---\n", registeredUserCount(), Cfg::MAX_USERS);
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        if (_users[i].active) {
            out.printf("  [%u] user_id=%s | rfid=%s | has_pin=%s\n",
                       i,
                       _users[i].userId,
                       _users[i].uid[0] ? _users[i].uid : "-",
                       _users[i].pinHash[0] ? "YES" : "NO");
        }
    }
    xSemaphoreGive(_stateMutex);
}

void LockerManager::processAuthRequestOnCore0(const AuthRequest& req, AuthResult& res) {
    res.requestId = req.requestId;
    res.type = req.type;
    res.lockerIndex = req.lockerIndex;
    res.matched = false;
    res.userSlot = -1;

    const String pinStr(req.pin);

    if (req.type == AuthRequestType::ADMIN_SERIAL_LOGIN) {
        res.matched = Security::verifySerialAdmin(String(req.username), pinStr);
        return;
    }

    if (req.type == AuthRequestType::ADMIN_DMENU_PIN) {
        xSemaphoreTake(_stateMutex, portMAX_DELAY);
        const AdminConfig cfgCopy = _adminCfg;
        xSemaphoreGive(_stateMutex);
        if (cfgCopy.provisioned) {
            res.matched = Security::verifyPinHash(cfgCopy.pinSalt, pinStr, cfgCopy.pinHash);
        }
        return;
    }

    if (req.type == AuthRequestType::USER_UNLOCK_PIN) {
        if (req.lockerIndex < 0 || req.lockerIndex >= Cfg::LOCKER_COUNT) return;
        xSemaphoreTake(_stateMutex, portMAX_DELAY);
        const LockerRecord rec = _records[req.lockerIndex];
        char saltCopy[17] = {};
        char hashCopy[65] = {};
        char userCopy[25] = {};
        if (isAssigned(rec) && rec.userId[0] != '\0') {
            strncpy(userCopy, rec.userId, sizeof(userCopy) - 1);
            strncpy(saltCopy, rec.salt, sizeof(saltCopy) - 1);
            strncpy(hashCopy, rec.pinHash, sizeof(hashCopy) - 1);
            const int uSlot = findRegisteredUserByUserId(String(rec.userId));
            if (uSlot >= 0 && _users[uSlot].pinHash[0] != '\0') {
                strncpy(saltCopy, _users[uSlot].salt, sizeof(saltCopy) - 1);
                strncpy(hashCopy, _users[uSlot].pinHash, sizeof(hashCopy) - 1);
                res.userSlot = uSlot;
            }
        }
        xSemaphoreGive(_stateMutex);

        if (hashCopy[0] != '\0' && Security::verifyPinHash(saltCopy, pinStr, hashCopy)) {
            res.matched = true;
            strncpy(res.userId, userCopy, sizeof(res.userId) - 1);
        }
        return;
    }

    if (req.type == AuthRequestType::USER_ASSIGN_PIN) {
        // Copy active users under mutex, then run 10,000-iteration PBKDF2 outside mutex
        UserRecord snapshot[Cfg::MAX_USERS];
        xSemaphoreTake(_stateMutex, portMAX_DELAY);
        memcpy(snapshot, _users, sizeof(snapshot));
        xSemaphoreGive(_stateMutex);

        for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
            if (snapshot[i].active && snapshot[i].pinHash[0] != '\0') {
                if (Security::verifyPinHash(snapshot[i].salt, pinStr, snapshot[i].pinHash)) {
                    res.matched = true;
                    res.userSlot = i;
                    strncpy(res.userId, snapshot[i].userId, sizeof(res.userId) - 1);
                    return;
                }
            }
        }
    }
}

// ============================================================
// INTERNAL HELPERS
// ============================================================

int LockerManager::findLockerId(const String& id) const {
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (id.equalsIgnoreCase(Cfg::LOCKER_IDS[i])) return i;
    }
    return -1;
}

uint8_t LockerManager::availableCount() const {
    uint8_t cnt = 0;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (!isAssigned(_records[i])) ++cnt;
    }
    return cnt;
}

uint8_t LockerManager::problematicCount() const {
    uint8_t cnt = 0;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (isExpiredHold(_records[i])) ++cnt;
    }
    return cnt;
}

int LockerManager::findRegisteredUserByUid(const String& uid) const {
    if (uid.isEmpty()) return -1;
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        if (_users[i].active && uid.equalsIgnoreCase(_users[i].uid)) return i;
    }
    return -1;
}

int LockerManager::findRegisteredUserByUserId(const String& userId) const {
    if (userId.isEmpty()) return -1;
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        if (_users[i].active && userId == _users[i].userId) return i;
    }
    return -1;
}

int LockerManager::findAssignedLockerByUserId(const String& userId) const {
    if (userId.isEmpty()) return -1;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (isAssigned(_records[i]) && userId == _records[i].userId) return i;
    }
    return -1;
}

void LockerManager::saveLocker(uint8_t index) {
    _storage->saveLocker(index, _records[index]);
}

bool LockerManager::setLock(uint8_t index, bool locked, const char* method) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    if (!_hardware->setLockerActuator(index, locked)) return false;
    _locked[index] = locked;
    appLog("ACT", "%s -> %s (%s)", Cfg::LOCKER_IDS[index], locked ? "LOCKED" : "UNLOCKED", method);
    return true;
}

void LockerManager::emitEvent(const char* name, uint8_t index, const char* method, const char* userId) {
    if (index >= Cfg::LOCKER_COUNT) return;

    char eventId[40];
    snprintf(eventId, sizeof(eventId), "%s-%lu-%04X",
             Cfg::DEVICE_ID,
             static_cast<unsigned long>(millis()),
             static_cast<unsigned int>(esp_random() & 0xFFFF));

    char json[320];
    snprintf(json, sizeof(json),
             "{\"event_id\":\"%s\",\"location_id\":\"%s\",\"device_id\":\"%s\","
             "\"locker_id\":\"%s\",\"user_id\":\"%s\",\"event\":\"%s\","
             "\"locked\":%s,\"method\":\"%s\",\"timestamp\":%llu}",
             eventId,
             Cfg::LOCATION_ID,
             Cfg::DEVICE_ID,
             Cfg::LOCKER_IDS[index],
             userId ? userId : "",
             name,
             _locked[index] ? "true" : "false",
             method ? method : "SYSTEM",
             static_cast<unsigned long long>(_clock->nowUtc()));

    const String payload(json);
    _storage->logEvent(payload);
    if (!_network->publishEvent(payload)) {
        _storage->enqueue(payload);
    }
}

void LockerManager::publishFullLockerState(uint8_t index, const char* method) {
    if (index >= Cfg::LOCKER_COUNT) return;
    _network->publishLockerState(
        index,
        _locked[index],
        isAssigned(_records[index]),
        isExpiredHold(_records[index]),
        _records[index].userId,
        method
    );
}
