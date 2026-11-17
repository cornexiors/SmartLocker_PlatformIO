#include "App.h"
#include <time.h>
#include <sys/time.h>
#include <cstdio>

namespace {

constexpr uint32_t RTC_RETRY_MS = 5000;

bool validEpoch(uint64_t epoch) {
    return epoch >= Cfg::MIN_VALID_EPOCH;
}

} // namespace

bool ClockService::begin() {
    _extRtcReady = _rtc.begin(&Wire);
    _ntpStarted = false;
    _lastRtcProbe = millis();
    _lastNtpSyncMs = 0;

    // Seed ESP32 internal RTC from optional external RTC if valid
    if (_extRtcReady && _rtc.isrunning()) {
        const uint64_t rtcEpoch = _rtc.now().unixtime();
        if (validEpoch(rtcEpoch)) {
            setSystemEpoch(rtcEpoch);
        }
    }
    return true;
}

void ClockService::setSystemEpoch(uint64_t epoch) {
    struct timeval tv {};
    tv.tv_sec = static_cast<time_t>(epoch);
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
}

void ClockService::forceNtpSync() {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    _ntpStarted = true;
    _lastNtpSyncMs = millis();
}

void ClockService::tick(bool wifiConnected) {
    const uint32_t now = millis();

    // Periodically check if an external 3.3V RTC was attached
    if (now - _lastRtcProbe >= RTC_RETRY_MS) {
        _lastRtcProbe = now;
        _extRtcReady = _rtc.begin(&Wire);
        if (_extRtcReady && _rtc.isrunning() && !valid()) {
            const uint64_t rtcEpoch = _rtc.now().unixtime();
            if (validEpoch(rtcEpoch)) {
                setSystemEpoch(rtcEpoch);
            }
        }
    }

    if (!wifiConnected) return;

    // Sync NTP on boot/power instability (!valid()), or every 2 hours
    if (!_ntpStarted || !valid() || (now - _lastNtpSyncMs >= Cfg::NTP_RESYNC_MS)) {
        forceNtpSync();
    }

    // Keep optional external RTC aligned with internal system clock
    if (_extRtcReady && valid()) {
        const uint64_t sysEpoch = nowUtc();
        const uint64_t extEpoch = _rtc.now().unixtime();
        if (!validEpoch(extEpoch) || llabs(static_cast<long long>(sysEpoch - extEpoch)) > 5) {
            _rtc.adjust(DateTime(static_cast<uint32_t>(sysEpoch)));
        }
    }
}

bool ClockService::valid() const {
    return validEpoch(static_cast<uint64_t>(time(nullptr)));
}

uint64_t ClockService::nowUtc() const {
    const uint64_t epoch = static_cast<uint64_t>(time(nullptr));
    return validEpoch(epoch) ? epoch : 0;
}

void ClockService::formatShortLocal(char* out, size_t length) const {
    if (!out || length == 0) return;
    if (!valid()) {
        snprintf(out, length, "--:--:--");
        return;
    }

    const time_t localEpoch = static_cast<time_t>(
        nowUtc() + static_cast<int64_t>(Cfg::LOCAL_UTC_OFFSET_HOURS) * 3600LL
    );
    struct tm tmv {};
    gmtime_r(&localEpoch, &tmv);
    snprintf(out, length, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

void ClockService::formatLocal(char* out, size_t length) const {
    if (!out || length == 0) return;
    if (!valid()) {
        snprintf(out, length, "Clock unsynced");
        return;
    }

    const time_t localEpoch = static_cast<time_t>(
        nowUtc() + static_cast<int64_t>(Cfg::LOCAL_UTC_OFFSET_HOURS) * 3600LL
    );
    struct tm tmv {};
    gmtime_r(&localEpoch, &tmv);
    snprintf(out, length, "%04d-%02d-%02d %02d:%02d:%02d WIB",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}