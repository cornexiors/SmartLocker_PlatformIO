#include "App.h"

bool Storage::begin() {
    if (!_prefs.begin("lockerv12", false)) return false;
    if (!_netCfg.begin("lockercfg", false)) {
        _prefs.end();
        return false;
    }
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

void Storage::clearAllLockers() {
    _prefs.clear();
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