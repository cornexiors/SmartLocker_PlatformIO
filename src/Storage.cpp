#include "App.h"

bool Storage::begin() {
    if (!_prefs.begin("lockerdata", false)) return false;
    if (!_users.begin("lockerusers", false)) {
        _prefs.end();
        return false;
    }
    if (!_queue.begin("lockersync", false)) {
        _users.end();
        _prefs.end();
        return false;
    }
    if (!_netCfg.begin("lockercfg", false)) {
        _queue.end();
        _users.end();
        _prefs.end();
        return false;
    }

    _head = _queue.getUChar("head", 0) % Cfg::SYNC_QUEUE_CAPACITY;
    _tail = _queue.getUChar("tail", 0) % Cfg::SYNC_QUEUE_CAPACITY;

    _sdReady = SD.begin(Cfg::SD_CS, SPI, Cfg::SD_SPI_HZ);
    return true;
}

void Storage::loadLockers(LockerRecord records[Cfg::LOCKER_COUNT]) {
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        char key[5];
        snprintf(key, sizeof(key), "L%u", i);
        if (_prefs.getBytesLength(key) == sizeof(LockerRecord)) {
            _prefs.getBytes(key, &records[i], sizeof(LockerRecord));
        } else {
            records[i] = LockerRecord{};
        }
    }
}

void Storage::saveLocker(uint8_t index, const LockerRecord& record) {
    if (index >= Cfg::LOCKER_COUNT) return;
    char key[5];
    snprintf(key, sizeof(key), "L%u", index);
    _prefs.putBytes(key, &record, sizeof(record));
}

void Storage::loadUsers(UserRecord users[Cfg::MAX_USERS]) {
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        char key[6];
        snprintf(key, sizeof(key), "U%02u", i);
        if (_users.getBytesLength(key) == sizeof(UserRecord)) {
            _users.getBytes(key, &users[i], sizeof(UserRecord));
        } else {
            users[i] = UserRecord{};
        }
    }
}

void Storage::saveUser(uint8_t slot, const UserRecord& user) {
    if (slot >= Cfg::MAX_USERS) return;
    char key[6];
    snprintf(key, sizeof(key), "U%02u", slot);
    _users.putBytes(key, &user, sizeof(user));
}

void Storage::clearAllUsers() {
    _users.clear();
}

bool Storage::enqueue(const String& payload) {
    const uint8_t next = static_cast<uint8_t>((_tail + 1) % Cfg::SYNC_QUEUE_CAPACITY);
    if (next == _head) return false;
    char key[6];
    snprintf(key, sizeof(key), "Q%02u", _tail);
    if (_queue.putString(key, payload) == 0) return false;
    _tail = next;
    _queue.putUChar("tail", _tail);
    return true;
}

bool Storage::peek(String& payload) {
    if (_head == _tail) return false;
    char key[6];
    snprintf(key, sizeof(key), "Q%02u", _head);
    payload = _queue.getString(key, "");
    return !payload.isEmpty();
}

void Storage::pop() {
    if (_head == _tail) return;
    char key[6];
    snprintf(key, sizeof(key), "Q%02u", _head);
    _queue.remove(key);
    _head = static_cast<uint8_t>((_head + 1) % Cfg::SYNC_QUEUE_CAPACITY);
    _queue.putUChar("head", _head);
}

uint8_t Storage::queueCount() const {
    return static_cast<uint8_t>((_tail + Cfg::SYNC_QUEUE_CAPACITY - _head) % Cfg::SYNC_QUEUE_CAPACITY);
}

void Storage::saveWifiOverride(const String& ssid, const String& password) {
    _netCfg.putString("ssid", ssid);
    _netCfg.putString("pass", password);
}

bool Storage::loadWifiOverride(String& ssid, String& password) {
    ssid = _netCfg.getString("ssid", "");
    if (ssid.isEmpty()) return false;
    password = _netCfg.getString("pass", "");
    return true;
}

void Storage::clearWifiOverride() {
    _netCfg.remove("ssid");
    _netCfg.remove("pass");
}

void Storage::logEvent(const String& payload) {
    if (!_sdReady) return;
    File file = SD.open("/locker.log", FILE_APPEND);
    if (!file) return;
    file.println(payload);
    file.close();
}

void Storage::dumpSdLog(Stream& out) {
    if (!_sdReady) {
        out.println(F("SD card not present."));
        return;
    }
    File file = SD.open("/locker.log", FILE_READ);
    if (!file) {
        out.println(F("No /locker.log file found on SD."));
        return;
    }
    out.println(F("--- SD /locker.log ---"));
    while (file.available()) {
        out.write(file.read());
    }
    out.println(F("--- END SD LOG ---"));
    file.close();
}

void Storage::clearSdLog() {
    if (!_sdReady) return;
    if (SD.exists("/locker.log")) {
        SD.remove("/locker.log");
    }
}