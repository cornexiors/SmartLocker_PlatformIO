#include "App.h"
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <cstring>

String makePinHash(const char* salt, const String& pin) {
    if (!salt || !salt[0] || pin.isEmpty()) return String();
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return String();

    uint8_t digest[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, md, 1);
    if (rc == 0) {
        rc = mbedtls_pkcs5_pbkdf2_hmac(
            &ctx,
            reinterpret_cast<const unsigned char*>(pin.c_str()), pin.length(),
            reinterpret_cast<const unsigned char*>(salt), strnlen(salt, 16),
            Cfg::PIN_PBKDF2_ITERATIONS, sizeof(digest), digest
        );
    }
    mbedtls_md_free(&ctx);
    if (rc != 0) return String();

    static const char hex[] = "0123456789abcdef";
    char out[65];
    for (uint8_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[64] = '\0';
    return String(out);
}

static bool constantEqual(const char* a, const String& b) {
    const size_t n = strlen(a);
    if (n != b.length()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

void LockerManager::begin(Storage& storage, Hardware& hardware,
                          ClockService& clock, NetworkService& network) {
    _storage = &storage;
    _hardware = &hardware;
    _clock = &clock;
    _network = &network;

    _storage->loadLockers(_records);
    _storage->loadUsers(_users);

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        _locked[i] = true;
        if (!_hardware->setLockerActuator(i, true)) {
            appLog("WARN", "Actuator init failed for %s", Cfg::LOCKER_IDS[i]);
        }
        if (!_hardware->doorClosed(i)) {
            _doorOpenedAt[i] = millis();
        }
    }
}

void LockerManager::saveLocker(uint8_t index) {
    if (_storage) _storage->saveLocker(index, _records[index]);
}

void LockerManager::publishFullLockerState(uint8_t index, const char* method) {
    if (!_network || index >= Cfg::LOCKER_COUNT) return;
    const LockerRecord& r = _records[index];
    _network->publishLockerState(
        index,
        _locked[index],
        isAssigned(r),
        isExpiredHold(r),
        r.userId,
        method
    );
}

void LockerManager::broadcastAllStates(const char* method) {
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        publishFullLockerState(i, method);
    }
}

void LockerManager::emitEvent(const char* name, uint8_t index, const char* method, const char* userId) {
    char json[320];
    const uint64_t now = _clock ? _clock->nowUtc() : 0;
    const char* locker = index < Cfg::LOCKER_COUNT ? Cfg::LOCKER_IDS[index] : "SYSTEM";
    const bool locked = index < Cfg::LOCKER_COUNT ? _locked[index] : true;
    const bool assigned = index < Cfg::LOCKER_COUNT ? isAssigned(_records[index]) : false;

    snprintf(json, sizeof(json),
             "{\"event_id\":\"%s-%08lX\",\"device_id\":\"%s\",\"event\":\"%s\","
             "\"locker_id\":\"%s\",\"user_id\":\"%s\",\"locked\":%s,\"assigned\":%s,"
             "\"method\":\"%s\",\"timestamp\":%llu}",
             Cfg::DEVICE_ID, static_cast<unsigned long>(esp_random()), Cfg::DEVICE_ID,
             name, locker, userId ? userId : "",
             locked ? "true" : "false",
             assigned ? "true" : "false",
             method, static_cast<unsigned long long>(now));

    const String payload(json);
    appLog("EVENT", "%s", json);
    _storage->logEvent(payload);
    if (!_storage->enqueue(payload)) {
        appLog("WARN", "Local NVS sync queue full");
    }
}

bool LockerManager::setLock(uint8_t index, bool locked, const char* method) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    if (!_hardware->setLockerActuator(index, locked)) {
        appLog("ERR", "Actuator command failed for %s", Cfg::LOCKER_IDS[index]);
        return false;
    }
    _locked[index] = locked;
    publishFullLockerState(index, method);
    return true;
}

// ============================================================
// QUERIES & USER LOOKUPS
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
        if (!isAssigned(_records[i])) ++count;
    }
    return count;
}

uint8_t LockerManager::problematicCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (isAssigned(_records[i]) && isExpiredHold(_records[i])) ++count;
    }
    return count;
}

uint8_t LockerManager::registeredUserCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        if (_users[i].active) ++count;
    }
    return count;
}

void LockerManager::listUsersToSerial(Stream& out) const {
    out.printf("Registered users (%u/%u):\n", registeredUserCount(), Cfg::MAX_USERS);
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        if (_users[i].active) {
            out.printf("  [%02u] user_id=%s rfid=%s has_pin=%s\n",
                       i,
                       _users[i].userId,
                       _users[i].uid[0] ? _users[i].uid : "(none)",
                       _users[i].pinHash[0] ? "yes" : "no");
        }
    }
}

int LockerManager::findRegisteredUserByUid(const String& uid) const {
    if (uid.isEmpty()) return -1;
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        if (_users[i].active && _users[i].uid[0] && uid.equalsIgnoreCase(_users[i].uid)) {
            return i;
        }
    }
    return -1;
}

int LockerManager::findRegisteredUserByPin(const String& pin) const {
    if (pin.length() < Cfg::PIN_MIN_LENGTH || pin.length() > Cfg::PIN_MAX_LENGTH) return -1;
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        const UserRecord& u = _users[i];
        if (!u.active || !u.pinHash[0]) continue;
        const String candidate = makePinHash(u.salt, pin);
        if (!candidate.isEmpty() && constantEqual(u.pinHash, candidate)) {
            return i;
        }
    }
    return -1;
}

int LockerManager::findAssignedLockerByUserId(const String& userId) const {
    if (userId.isEmpty()) return -1;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (isAssigned(_records[i]) && userId.equalsIgnoreCase(_records[i].userId)) {
            return i;
        }
    }
    return -1;
}

int LockerManager::findAssignedLockerByUid(const String& uid) const {
    if (uid.isEmpty()) return -1;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (isAssigned(_records[i]) && _records[i].uid[0] && uid.equalsIgnoreCase(_records[i].uid)) {
            return i;
        }
    }
    return -1;
}

int LockerManager::findAssignedLockerByPin(const String& pin) const {
    if (pin.length() < Cfg::PIN_MIN_LENGTH || pin.length() > Cfg::PIN_MAX_LENGTH) return -1;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        const LockerRecord& r = _records[i];
        if (!isAssigned(r) || !r.pinHash[0]) continue;
        const String candidate = makePinHash(r.salt, pin);
        if (!candidate.isEmpty() && constantEqual(r.pinHash, candidate)) {
            return i;
        }
    }
    return -1;
}

// ============================================================
// ASSIGN & SINGLE-USE UNLOCK-RELEASE FLOWS
// ============================================================

ActionStatus LockerManager::assignToUser(int userIdx, const char* method, String& lockerName) {
    if (userIdx < 0 || userIdx >= Cfg::MAX_USERS || !_users[userIdx].active) {
        return ActionStatus::UNAUTHORIZED;
    }

    const UserRecord& u = _users[userIdx];

    const int existing = findAssignedLockerByUserId(u.userId);
    if (existing >= 0) {
        lockerName = Cfg::LOCKER_IDS[existing];
        return ActionStatus::ALREADY_ASSIGNED;
    }

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (isAssigned(_records[i])) continue;

        LockerRecord r{};
        r.flags = 0x01;
        strlcpy(r.userId, u.userId, sizeof(r.userId));
        strlcpy(r.uid, u.uid, sizeof(r.uid));
        strlcpy(r.salt, u.salt, sizeof(r.salt));
        strlcpy(r.pinHash, u.pinHash, sizeof(r.pinHash));
        r.assignedAt = _clock->nowUtc();
        r.expiresAt = _clock->valid() ? (r.assignedAt + Cfg::RENTAL_SECONDS) : 0;

        _records[i] = r;
        saveLocker(i);

        if (!setLock(i, false, method)) {
            _records[i] = LockerRecord{};
            saveLocker(i);
            return ActionStatus::HARDWARE_ERROR;
        }

        emitEvent("LOCKER_ASSIGNED", i, method, u.userId);
        lockerName = Cfg::LOCKER_IDS[i];
        return ActionStatus::OK;
    }

    return ActionStatus::NO_LOCKER_FREE;
}

ActionStatus LockerManager::assignByUid(const String& uid, String& lockerName) {
    const int userIdx = findRegisteredUserByUid(uid);
    if (userIdx < 0) return ActionStatus::UNAUTHORIZED;
    return assignToUser(userIdx, "RFID", lockerName);
}

ActionStatus LockerManager::assignByPin(const String& pin, String& lockerName) {
    const int userIdx = findRegisteredUserByPin(pin);
    if (userIdx < 0) return ActionStatus::UNAUTHORIZED;
    return assignToUser(userIdx, "PIN", lockerName);
}

ActionStatus LockerManager::releaseUnlockedLocker(int lockerIdx, const char* method, String& lockerName) {
    if (lockerIdx < 0 || lockerIdx >= Cfg::LOCKER_COUNT || !isAssigned(_records[lockerIdx])) {
        return ActionStatus::UNAUTHORIZED;
    }

    LockerRecord& r = _records[lockerIdx];
    lockerName = Cfg::LOCKER_IDS[lockerIdx];

    if (isExpiredHold(r) || (r.expiresAt > 0 && _clock->valid() && _clock->nowUtc() >= r.expiresAt)) {
        if (!isExpiredHold(r)) {
            r.flags |= 0x02;
            saveLocker(lockerIdx);
        }
        return ActionStatus::EXPIRED;
    }

    char userIdCopy[25];
    strlcpy(userIdCopy, r.userId, sizeof(userIdCopy));

    _records[lockerIdx] = LockerRecord{};
    saveLocker(lockerIdx);

    if (!setLock(lockerIdx, false, method)) {
        return ActionStatus::HARDWARE_ERROR;
    }

    emitEvent("LOCKER_UNLOCKED", lockerIdx, method, userIdCopy);
    return ActionStatus::OK;
}

ActionStatus LockerManager::unlockAndReleaseByUid(const String& uid, String& lockerName) {
    int idx = findAssignedLockerByUid(uid);
    if (idx < 0) {
        const int uIdx = findRegisteredUserByUid(uid);
        if (uIdx >= 0) idx = findAssignedLockerByUserId(_users[uIdx].userId);
    }
    if (idx < 0) return ActionStatus::UNAUTHORIZED;
    return releaseUnlockedLocker(idx, "RFID", lockerName);
}

ActionStatus LockerManager::unlockAndReleaseByPin(const String& pin, String& lockerName) {
    int idx = findAssignedLockerByPin(pin);
    if (idx < 0) {
        const int uIdx = findRegisteredUserByPin(pin);
        if (uIdx >= 0) idx = findAssignedLockerByUserId(_users[uIdx].userId);
    }
    if (idx < 0) return ActionStatus::UNAUTHORIZED;
    return releaseUnlockedLocker(idx, "PIN", lockerName);
}

// ============================================================
// D-MODE SELF-SERVICE RFID REGISTRATION VIA APP OTP
// ============================================================

bool LockerManager::registerPendingOtp(const String& userId, const String& otp) {
    if (userId.isEmpty() || otp.length() < Cfg::PIN_MIN_LENGTH || otp.length() > Cfg::PIN_MAX_LENGTH) {
        return false;
    }

    const uint32_t now = millis();
    int slot = -1;

    for (uint8_t i = 0; i < Cfg::MAX_PENDING_OTPS; ++i) {
        if (_otps[i].active && static_cast<int32_t>(now - _otps[i].expiresAtMs) >= 0) {
            _otps[i] = PendingOtp{};
        }
        if (_otps[i].active && userId.equalsIgnoreCase(_otps[i].userId)) {
            slot = i;
            break;
        }
        if (!_otps[i].active && slot < 0) {
            slot = i;
        }
    }

    if (slot < 0) slot = 0;

    PendingOtp entry{};
    entry.active = true;
    strlcpy(entry.userId, userId.c_str(), sizeof(entry.userId));
    strlcpy(entry.otp, otp.c_str(), sizeof(entry.otp));
    entry.expiresAtMs = now + Cfg::OTP_TTL_MS;
    _otps[slot] = entry;
    return true;
}

bool LockerManager::verifyOtp(const String& otp, String& outUserId) {
    if (otp.isEmpty()) return false;
    const uint32_t now = millis();

    for (uint8_t i = 0; i < Cfg::MAX_PENDING_OTPS; ++i) {
        if (!_otps[i].active) continue;
        if (static_cast<int32_t>(now - _otps[i].expiresAtMs) >= 0) {
            _otps[i] = PendingOtp{};
            continue;
        }
        if (otp == _otps[i].otp) {
            outUserId = _otps[i].userId;
            return true;
        }
    }
    return false;
}

ActionStatus LockerManager::completeOtpRfidRegistration(const String& otp, const String& uid,
                                                        String& outUserId) {
    if (uid.isEmpty()) return ActionStatus::UNAUTHORIZED;

    const uint32_t now = millis();
    int otpIdx = -1;
    for (uint8_t i = 0; i < Cfg::MAX_PENDING_OTPS; ++i) {
        if (_otps[i].active && static_cast<int32_t>(now - _otps[i].expiresAtMs) < 0 &&
            otp == _otps[i].otp) {
            otpIdx = i;
            break;
        }
    }
    if (otpIdx < 0) return ActionStatus::EXPIRED;

    outUserId = _otps[otpIdx].userId;

    // Prevent binding a card that already belongs to another registered account
    const int existingOwner = findRegisteredUserByUid(uid);
    if (existingOwner >= 0 && !outUserId.equalsIgnoreCase(_users[existingOwner].userId)) {
        if (_network) _network->publishRfidRegAck(outUserId, uid, false, "RFID_ALREADY_IN_USE");
        return ActionStatus::ALREADY_ASSIGNED;
    }

    // Locate or create user slot in NVS database
    int userSlot = -1;
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        if (_users[i].active && outUserId.equalsIgnoreCase(_users[i].userId)) {
            userSlot = i;
            break;
        }
        if (!_users[i].active && userSlot < 0) {
            userSlot = i;
        }
    }
    if (userSlot < 0) {
        if (_network) _network->publishRfidRegAck(outUserId, uid, false, "USER_DB_FULL");
        return ActionStatus::NO_LOCKER_FREE;
    }

    UserRecord u = _users[userSlot];
    u.active = 1;
    strlcpy(u.userId, outUserId.c_str(), sizeof(u.userId));
    strlcpy(u.uid, uid.c_str(), sizeof(u.uid));
    _users[userSlot] = u;
    _storage->saveUser(userSlot, u);

    // Update active locker if this user currently holds one
    const int lockerIdx = findAssignedLockerByUserId(outUserId);
    if (lockerIdx >= 0) {
        strlcpy(_records[lockerIdx].uid, uid.c_str(), sizeof(_records[lockerIdx].uid));
        saveLocker(lockerIdx);
    }

    // Consume OTP (single-use)
    _otps[otpIdx] = PendingOtp{};

    // Emit durable event to NVS queue + immediate ACK to backend
    emitEvent("RFID_REGISTERED", 0xFF, uid.c_str(), outUserId.c_str());
    if (_network) {
        _network->publishRfidRegAck(outUserId, uid, true, "REGISTERED");
    }

    return ActionStatus::OK;
}

// ============================================================
// REMOTE, ADMIN & USER DB SYNC
// ============================================================

bool LockerManager::remoteSetLock(uint8_t index, bool locked, const char* source) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    if (!setLock(index, locked, source)) return false;
    emitEvent(locked ? "REMOTE_LOCK" : "REMOTE_UNLOCK", index, source, _records[index].userId);
    return true;
}

bool LockerManager::release(uint8_t index, const char* source) {
    if (index >= Cfg::LOCKER_COUNT) return false;
    char prevUser[25];
    strlcpy(prevUser, _records[index].userId, sizeof(prevUser));

    _records[index] = LockerRecord{};
    saveLocker(index);

    if (!setLock(index, false, source)) return false;
    emitEvent("LOCKER_RELEASED", index, source, prevUser);
    return true;
}

bool LockerManager::upsertUser(const String& userId, const String& uid,
                               const String& salt, const String& pinHash) {
    if (userId.isEmpty()) return false;

    int targetSlot = -1;
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        if (_users[i].active && userId.equalsIgnoreCase(_users[i].userId)) {
            targetSlot = i;
            break;
        }
        if (!_users[i].active && targetSlot < 0) {
            targetSlot = i;
        }
    }
    if (targetSlot < 0) return false;

    UserRecord u{};
    u.active = 1;
    strlcpy(u.userId, userId.c_str(), sizeof(u.userId));
    strlcpy(u.uid, uid.c_str(), sizeof(u.uid));
    strlcpy(u.salt, salt.c_str(), sizeof(u.salt));
    strlcpy(u.pinHash, pinHash.c_str(), sizeof(u.pinHash));

    _users[targetSlot] = u;
    _storage->saveUser(targetSlot, u);

    const int lockerIdx = findAssignedLockerByUserId(userId);
    if (lockerIdx >= 0) {
        strlcpy(_records[lockerIdx].uid, uid.c_str(), sizeof(_records[lockerIdx].uid));
        strlcpy(_records[lockerIdx].salt, salt.c_str(), sizeof(_records[lockerIdx].salt));
        strlcpy(_records[lockerIdx].pinHash, pinHash.c_str(), sizeof(_records[lockerIdx].pinHash));
        saveLocker(lockerIdx);
    }
    return true;
}

bool LockerManager::deleteUser(const String& userId) {
    if (userId.isEmpty()) return false;
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        if (_users[i].active && userId.equalsIgnoreCase(_users[i].userId)) {
            _users[i] = UserRecord{};
            _storage->saveUser(i, _users[i]);
            return true;
        }
    }
    return false;
}

void LockerManager::clearUsers() {
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        _users[i] = UserRecord{};
    }
    _storage->clearAllUsers();
}

void LockerManager::checkExpirationsAndDoorAlerts() {
    const uint32_t nowMs = millis();

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        if (!_hardware->doorClosed(i)) {
            if (_doorOpenedAt[i] == 0) _doorOpenedAt[i] = nowMs;
            if (nowMs - _doorOpenedAt[i] >= Cfg::DOOR_OPEN_ALERT_MS) {
                if (nowMs - _lastDoorAlertAt[i] >= Cfg::DOOR_ALERT_REPEAT_MS) {
                    _lastDoorAlertAt[i] = nowMs;
                    _hardware->beep(150);
                }
                if (!_doorTimeoutReported[i]) {
                    _doorTimeoutReported[i] = true;
                    emitEvent("DOOR_OPEN_TIMEOUT", i, "REED", _records[i].userId);
                }
            }
        }
    }

    if (!_clock->valid()) return;
    const uint64_t nowUtc = _clock->nowUtc();

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        LockerRecord& r = _records[i];
        if (!isAssigned(r)) continue;

        if (r.expiresAt == 0) {
            r.assignedAt = nowUtc;
            r.expiresAt = nowUtc + Cfg::RENTAL_SECONDS;
            saveLocker(i);
            continue;
        }

        if (!isExpiredHold(r) && nowUtc >= r.expiresAt) {
            r.flags |= 0x02;
            saveLocker(i);
            setLock(i, true, "SYSTEM");
            emitEvent("LOCKER_EXPIRED", i, "SYSTEM", r.userId);
        }
    }
}

void LockerManager::tick() {
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        bool closed = false;
        if (_hardware->doorChanged(i, closed)) {
            if (closed) {
                _doorOpenedAt[i] = 0;
                _doorTimeoutReported[i] = false;
                emitEvent("DOOR_CLOSED", i, "REED", _records[i].userId);

                if (setLock(i, true, "REED")) {
                    emitEvent("DOOR_CLOSED_AUTO_LOCKED", i, "REED", _records[i].userId);
                }
            } else {
                _doorOpenedAt[i] = millis();
                _lastDoorAlertAt[i] = 0;
                _doorTimeoutReported[i] = false;
                emitEvent("DOOR_OPENED", i, "REED", _records[i].userId);
            }
        }
    }

    if (millis() - _lastTimerCheck >= 1000) {
        _lastTimerCheck = millis();
        checkExpirationsAndDoorAlerts();
    }
}