#include "App.h"
#include <time.h>
#include <esp_sntp.h>

bool ClockService::begin() {
    // Configure POSIX timezone for WIB (UTC+7) and initialize ESP32-S3 internal RTC
    setenv("TZ", "WIB-7", 1);
    tzset();
    return true;
}

void ClockService::tick(bool wifiConnected) {
    if (!wifiConnected) {
        _ntpStarted = false;
        return;
    }

    const uint32_t now = millis();
    if (!_ntpStarted || (now - _lastNtpSyncMs >= Cfg::NTP_RESYNC_MS)) {
        forceNtpSync();
        _ntpStarted = true;
        _lastNtpSyncMs = now;
    }
}

void ClockService::forceNtpSync() {
    configTzTime("WIB-7", "pool.ntp.org", "time.google.com", "id.pool.ntp.org");
    _lastNtpSyncMs = millis();
}

bool ClockService::valid() const {
    return nowUtc() >= Cfg::MIN_VALID_EPOCH;
}

uint64_t ClockService::nowUtc() const {
    const time_t now = time(nullptr);
    return now > 0 ? static_cast<uint64_t>(now) : 0;
}

void ClockService::formatLocal(char* out, size_t length) const {
    if (!valid()) {
        snprintf(out, length, "UNSYNCED (NTP)");
        return;
    }
    const time_t now = time(nullptr);
    struct tm tmLocal {};
    localtime_r(&now, &tmLocal);
    strftime(out, length, "%Y-%m-%d %H:%M:%S WIB", &tmLocal);
}

void ClockService::formatShortLocal(char* out, size_t length) const {
    if (!valid()) {
        snprintf(out, length, "--:--");
        return;
    }
    const time_t now = time(nullptr);
    struct tm tmLocal {};
    localtime_r(&now, &tmLocal);
    strftime(out, length, "%H:%M", &tmLocal);
}
