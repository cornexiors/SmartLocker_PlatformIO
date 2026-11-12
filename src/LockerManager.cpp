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
      Cfg::PIN_PBKDF2_ITERATIONS, sizeof(digest), digest);
  }
  mbedtls_md_free(&ctx);
  if (rc != 0) return String();
  static const char hex[] = "0123456789abcdef";
  char out[65];
  for (uint8_t i = 0; i < 32; ++i) { out[i * 2] = hex[digest[i] >> 4]; out[i * 2 + 1] = hex[digest[i] & 0x0F]; }
  out[64] = '\0';
  return String(out);
}

static bool constantEqual(const char* a, const String& b) {
  const size_t n = strlen(a);
  if (n != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

void LockerManager::begin(Storage& storage, Hardware& hardware, ClockService& clock, NetworkService& network) {
  _storage = &storage; _hardware = &hardware; _clock = &clock; _network = &network;
  _storage->load(_records);
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
    _locked[i] = true;
    if (!_hardware->setLockerActuator(i, true))
      Serial.printf("WARN: actuator initialization failed for %s\n", Cfg::LOCKER_IDS[i]);
  }
}

void LockerManager::save(uint8_t index) { if (_storage) _storage->save(index, _records[index]); }

void LockerManager::event(const char* name, uint8_t index, const char* method) {
  char json[240];
  const uint64_t now = _clock ? _clock->nowUtc() : 0;
  const char* locker = index < Cfg::LOCKER_COUNT ? Cfg::LOCKER_IDS[index] : "SYSTEM";
  snprintf(json, sizeof(json), "{\"event_id\":\"%s-%08lX\",\"device_id\":\"%s\",\"event\":\"%s\",\"locker_id\":\"%s\",\"method\":\"%s\",\"timestamp\":%llu}",
           Cfg::DEVICE_ID, static_cast<unsigned long>(esp_random()), Cfg::DEVICE_ID, name, locker, method,
           static_cast<unsigned long long>(now));
  const String payload(json);
  _storage->logEvent(payload);
  if (!_storage->enqueue(payload)) Serial.println(F("WARN: local sync queue full; event only in SD log/local state"));
}

bool LockerManager::setLock(uint8_t index, bool locked) {
  if (index >= Cfg::LOCKER_COUNT) return false;
  if (!_hardware->setLockerActuator(index, locked)) {
    Serial.printf("ERROR: actuator command failed for %s\n", Cfg::LOCKER_IDS[index]);
    return false;
  }
  _locked[index] = locked;
  _network->publishStatus(index, locked ? "LOCKED" : "UNLOCKED");
  return true;
}

int LockerManager::availableCount() const {
  int count = 0;
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) if (!isAssigned(_records[i])) ++count;
  return count;
}

int LockerManager::findByUid(const String& uid) const {
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i)
    if (isAssigned(_records[i]) && uid.equalsIgnoreCase(_records[i].uid)) return i;
  return -1;
}

int LockerManager::findByPin(const String& pin) const {
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
    const LockerRecord& r = _records[i];
    if (!isAssigned(r) || !r.pinHash[0]) continue;
    const String candidate = makePinHash(r.salt, pin);
    if (constantEqual(r.pinHash, candidate)) return i;
  }
  return -1;
}

int LockerManager::findLockerId(const String& id) const {
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) if (id.equalsIgnoreCase(Cfg::LOCKER_IDS[i])) return i;
  return -1;
}

bool LockerManager::assignByUid(const String& uid, String& lockerName) {
  if (uid.isEmpty() || findByUid(uid) >= 0) return false;
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
    if (isAssigned(_records[i])) continue;
    LockerRecord r{}; r.flags = 0x01; r.uidLength = static_cast<uint8_t>(min((size_t)20, uid.length()));
    strlcpy(r.uid, uid.c_str(), sizeof(r.uid));
    r.assignedAt = _clock->nowUtc(); r.expiresAt = _clock->valid() ? r.assignedAt + Cfg::RENTAL_SECONDS : 0;
    r.warningDate = _clock->valid() ? _clock->localDateKey() : 0;
    _records[i] = r; save(i);
    if (!setLock(i, false)) { _records[i] = LockerRecord{}; save(i); return false; }
    event("LOCKER_ASSIGNED", i, "RFID");
    lockerName = Cfg::LOCKER_IDS[i]; return true;
  }
  return false;
}

bool LockerManager::assignByPin(const String& pin, String& lockerName) {
  if (pin.length() < Cfg::PIN_MIN_LENGTH || pin.length() > Cfg::PIN_MAX_LENGTH || findByPin(pin) >= 0) return false;
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
    if (isAssigned(_records[i])) continue;
    LockerRecord r{}; r.flags = 0x01;
    const uint64_t random = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
    snprintf(r.salt, sizeof(r.salt), "%016llx", static_cast<unsigned long long>(random));
    const String hash = makePinHash(r.salt, pin);
    if (hash.isEmpty()) return false;
    strlcpy(r.pinHash, hash.c_str(), sizeof(r.pinHash));
    r.assignedAt = _clock->nowUtc(); r.expiresAt = _clock->valid() ? r.assignedAt + Cfg::RENTAL_SECONDS : 0;
    r.warningDate = _clock->valid() ? _clock->localDateKey() : 0;
    _records[i] = r; save(i);
    if (!setLock(i, false)) { _records[i] = LockerRecord{}; save(i); return false; }
    event("LOCKER_ASSIGNED", i, "PIN");
    lockerName = Cfg::LOCKER_IDS[i]; return true;
  }
  return false;
}

bool LockerManager::unlockByUid(const String& uid, String& lockerName) {
  const int i = findByUid(uid);
  if (i < 0 || isExpiredHold(_records[i])) return false;
  if (_records[i].expiresAt && _clock->valid() && _clock->nowUtc() >= _records[i].expiresAt) return false;
  if (!setLock(i, false)) return false;
  event("LOCKER_UNLOCKED", i, "RFID"); lockerName = Cfg::LOCKER_IDS[i]; return true;
}

bool LockerManager::unlockByPin(const String& pin, String& lockerName) {
  const int i = findByPin(pin);
  if (i < 0 || isExpiredHold(_records[i])) return false;
  if (_records[i].expiresAt && _clock->valid() && _clock->nowUtc() >= _records[i].expiresAt) return false;
  if (!setLock(i, false)) return false;
  event("LOCKER_UNLOCKED", i, "PIN"); lockerName = Cfg::LOCKER_IDS[i]; return true;
}

bool LockerManager::extend(uint8_t index, String& reason) {
  if (index >= Cfg::LOCKER_COUNT || !isAssigned(_records[index]) || isExpiredHold(_records[index])) { reason = "NOT_ACTIVE"; return false; }
  if (!_clock->valid()) { reason = "CLOCK_NOT_VALID"; return false; }
  LockerRecord& r = _records[index];
  if (r.extensions >= Cfg::MAX_EXTENSIONS) { reason = "EXTENSION_LIMIT"; return false; }
  if (r.expiresAt && _clock->nowUtc() >= r.expiresAt) { reason = "RENTAL_EXPIRED"; return false; }
  if (_clock->localHour() < 15) { reason = "AVAILABLE_AFTER_15"; return false; }
  // A warning-day extension carries the rental through to the following evening.
  const uint64_t deadline = _clock->localTodayAt(17) + 86400ULL;
  r.expiresAt = deadline; ++r.extensions; save(index);
  event("LOCKER_EXTENDED", index, "BACKEND"); reason = "EXTENDED"; return true;
}

bool LockerManager::release(uint8_t index) {
  if (index >= Cfg::LOCKER_COUNT || !isAssigned(_records[index])) return false;
  if (!setLock(index, true)) return false;
  _records[index] = LockerRecord{}; save(index);
  event("LOCKER_RELEASED_BY_OPERATOR", index, "BACKEND"); return true;
}

bool LockerManager::remoteSet(uint8_t index, bool locked) {
  if (index >= Cfg::LOCKER_COUNT) return false;
  if (!locked && (!isAssigned(_records[index]) || isExpiredHold(_records[index]))) return false;
  if (!locked && _records[index].expiresAt && _clock->valid() && _clock->nowUtc() >= _records[index].expiresAt) return false;
  if (!setLock(index, locked)) return false;
  event(locked ? "REMOTE_LOCK" : "REMOTE_UNLOCK", index, "BACKEND");
  return true;
}

void LockerManager::processExpiryAndWarnings() {
  if (!_clock->valid()) return;
  const uint64_t now = _clock->nowUtc();
  const uint16_t today = _clock->localDateKey();
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
    LockerRecord& r = _records[i];
    if (!isAssigned(r)) continue;
    if (!isExpiredHold(r) && r.expiresAt && now >= r.expiresAt) {
      r.flags |= 0x02; save(i);
      if (setLock(i, true)) event("RENTAL_EXPIRED_OPERATOR_CHECK", i, "SYSTEM");
      else event("ACTUATOR_FAILURE_ON_EXPIRY", i, "SYSTEM");
      continue;
    }
    if (isExpiredHold(r)) continue;
    // If an offline assignment was made before the clock was valid, start its
    // rental clock once RTC/NTP time becomes trustworthy.
    if (!r.expiresAt) {
      r.assignedAt = now;
      r.expiresAt = now + Cfg::RENTAL_SECONDS;
      r.warningDate = today;
      r.warningMask = 0;
      save(i);
      continue;
    }
    if (r.warningDate != today) {
      r.warningDate = today;
      r.warningMask = 0;
      save(i);
    }
    for (uint8_t bit = 0; bit < 2; ++bit) {
      const uint8_t hour = bit == 0 ? 15 : 17;
      const uint8_t mask = static_cast<uint8_t>(1U << bit);
      const uint64_t threshold = _clock->localTodayAt(hour);
      if (!(r.warningMask & mask) && r.assignedAt <= threshold && now >= threshold) {
        r.warningMask |= mask; save(i);
        event(bit == 0 ? "RENTAL_WARNING_15" : "RENTAL_WARNING_17", i, "SYSTEM");
      }
    }
  }
}

void LockerManager::tick() {
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
    bool closed = false;
    if (_hardware->doorChanged(i, closed)) {
      event(closed ? "DOOR_CLOSED" : "DOOR_OPENED", i, "REED");
      _hardware->drawDoorState(i, closed);
      if (closed && isAssigned(_records[i])) {
        if (setLock(i, true)) event("DOOR_CLOSED_AUTO_LOCKED", i, "REED");
        else event("ACTUATOR_FAILURE_ON_AUTO_LOCK", i, "SYSTEM");
      }
    }
  }
  if (millis() - _lastTimerCheck >= 1000) { _lastTimerCheck = millis(); processExpiryAndWarnings(); }
}
