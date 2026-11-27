#include "App.h"
#include <cstring>

namespace {
constexpr char LOG_PATH[] = "/debug.log";
constexpr size_t MAX_LOG_BYTES = 16384;
}

bool Storage::begin() {
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) return false;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    bool ok = _prefs.begin("lockerv21", false) &&
              _users.begin("lockerusers", false) &&
              _queue.begin("lockersync", false) &&
              _netCfg.begin("lockernet", false) &&
              _adminPrefs.begin("lockeradmin", false);

    if (ok) {
        _head = _queue.getUChar("head", 0);
        _tail = _queue.getUChar("tail", 0);
        if (_head >= Cfg::SYNC_QUEUE_CAPACITY || _tail >= Cfg::SYNC_QUEUE_CAPACITY) {
            _head = 0;
            _tail = 0;
            _queue.putUChar("head", 0);
            _queue.putUChar("tail", 0);
        }
    }

    _fsReady = LittleFS.begin(true);
    _sdReady = SD.begin(Cfg::SD_CS, SPI, Cfg::SD_SPI_HZ);

    xSemaphoreGive(_mutex);
    return ok;
}

void Storage::loadLockers(LockerRecord records[Cfg::LOCKER_COUNT]) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "L%u", i);
        memset(&records[i], 0, sizeof(LockerRecord));
        if (_prefs.getBytesLength(key) == sizeof(LockerRecord)) {
            _prefs.getBytes(key, &records[i], sizeof(LockerRecord));
        }
    }
    xSemaphoreGive(_mutex);
}

void Storage::saveLocker(uint8_t index, const LockerRecord& record) {
    if (index >= Cfg::LOCKER_COUNT) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    char key[8];
    snprintf(key, sizeof(key), "L%u", index);
    _prefs.putBytes(key, &record, sizeof(LockerRecord));
    xSemaphoreGive(_mutex);
}

void Storage::loadUsers(UserRecord users[Cfg::MAX_USERS]) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < Cfg::MAX_USERS; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "U%u", i);
        memset(&users[i], 0, sizeof(UserRecord));
        if (_users.getBytesLength(key) == sizeof(UserRecord)) {
            _users.getBytes(key, &users[i], sizeof(UserRecord));
        }
    }
    xSemaphoreGive(_mutex);
}

void Storage::saveUser(uint8_t slot, const UserRecord& user) {
    if (slot >= Cfg::MAX_USERS) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    char key[8];
    snprintf(key, sizeof(key), "U%u", slot);
    if (user.active) {
        _users.putBytes(key, &user, sizeof(UserRecord));
    } else {
        _users.remove(key);
    }
    xSemaphoreGive(_mutex);
}

void Storage::clearAllUsers() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _users.clear();
    xSemaphoreGive(_mutex);
}

void Storage::loadAdminConfig(AdminConfig& cfg) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    memset(&cfg, 0, sizeof(AdminConfig));
    if (_adminPrefs.getBytesLength("adm") == sizeof(AdminConfig)) {
        _adminPrefs.getBytes("adm", &cfg, sizeof(AdminConfig));
    }
    xSemaphoreGive(_mutex);
}

void Storage::saveAdminConfig(const AdminConfig& cfg) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _adminPrefs.putBytes("adm", &cfg, sizeof(AdminConfig));
    xSemaphoreGive(_mutex);
}

void Storage::clearAdminConfig() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _adminPrefs.clear();
    xSemaphoreGive(_mutex);
}

bool Storage::enqueue(const String& payload) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const uint8_t nextTail = static_cast<uint8_t>((_tail + 1) % Cfg::SYNC_QUEUE_CAPACITY);
    if (nextTail == _head) {
        xSemaphoreGive(_mutex);
        return false;
    }

    char key[8];
    snprintf(key, sizeof(key), "Q%u", _tail);
    _queue.putString(key, payload);
    _tail = nextTail;
    _queue.putUChar("tail", _tail);
    xSemaphoreGive(_mutex);
    return true;
}

bool Storage::peek(String& payload) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_head == _tail) {
        xSemaphoreGive(_mutex);
        return false;
    }
    char key[8];
    snprintf(key, sizeof(key), "Q%u", _head);
    payload = _queue.getString(key, "");
    xSemaphoreGive(_mutex);
    return !payload.isEmpty();
}

void Storage::pop() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_head == _tail) {
        xSemaphoreGive(_mutex);
        return;
    }
    char key[8];
    snprintf(key, sizeof(key), "Q%u", _head);
    _queue.remove(key);
    _head = static_cast<uint8_t>((_head + 1) % Cfg::SYNC_QUEUE_CAPACITY);
    _queue.putUChar("head", _head);
    xSemaphoreGive(_mutex);
}

uint8_t Storage::queueCount() const {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint8_t cnt = (_tail >= _head)
        ? (_tail - _head)
        : (Cfg::SYNC_QUEUE_CAPACITY - _head + _tail);
    xSemaphoreGive(_mutex);
    return cnt;
}

void Storage::saveWifiOverride(const String& ssid, const String& password) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _netCfg.putString("ssid", ssid);
    _netCfg.putString("pass", password);
    xSemaphoreGive(_mutex);
}

bool Storage::loadWifiOverride(String& ssid, String& password) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    ssid = _netCfg.getString("ssid", "");
    password = _netCfg.getString("pass", "");
    xSemaphoreGive(_mutex);
    return !ssid.isEmpty();
}

void Storage::clearWifiOverride() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _netCfg.clear();
    xSemaphoreGive(_mutex);
}

void Storage::factoryResetAll() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _prefs.clear();
    _users.clear();
    _queue.clear();
    _netCfg.clear();
    _adminPrefs.clear();
    _head = 0;
    _tail = 0;
    if (_fsReady) LittleFS.remove(LOG_PATH);
    xSemaphoreGive(_mutex);
}

void Storage::logEvent(const String& payload) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_fsReady) {
        File f = LittleFS.open(LOG_PATH, FILE_APPEND);
        if (f) {
            if (f.size() > MAX_LOG_BYTES) {
                f.close();
                LittleFS.remove(LOG_PATH);
                f = LittleFS.open(LOG_PATH, FILE_WRITE);
            }
            if (f) {
                f.println(payload);
                f.close();
            }
        }
    }
    if (_sdReady) {
        File sdf = SD.open(LOG_PATH, FILE_APPEND);
        if (sdf) {
            sdf.println(payload);
            sdf.close();
        }
    }
    xSemaphoreGive(_mutex);
}

void Storage::dumpLogs(Stream& out) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_fsReady && LittleFS.exists(LOG_PATH)) {
        out.println(F("--- LittleFS PERSISTENT LOG ---"));
        File f = LittleFS.open(LOG_PATH, FILE_READ);
        while (f && f.available()) {
            out.write(f.read());
        }
        if (f) f.close();
        out.println(F("--- END LittleFS LOG ---"));
    }
    if (_sdReady && SD.exists(LOG_PATH)) {
        out.println(F("--- SD CARD LOG ---"));
        File sdf = SD.open(LOG_PATH, FILE_READ);
        while (sdf && sdf.available()) {
            out.write(sdf.read());
        }
        if (sdf) sdf.close();
        out.println(F("--- END SD LOG ---"));
    }
    xSemaphoreGive(_mutex);
}

void Storage::clearLogs() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_fsReady && LittleFS.exists(LOG_PATH)) LittleFS.remove(LOG_PATH);
    if (_sdReady && SD.exists(LOG_PATH)) SD.remove(LOG_PATH);
    xSemaphoreGive(_mutex);
}
