
#include "App.h"

#include <time.h>
#include <sys/time.h>
#include <cstdio>

namespace {

constexpr uint32_t RTC_RETRY_MS = 5000;

bool validEpoch(uint64_t epoch) {
    return epoch >= Cfg::MIN_VALID_EPOCH;
}

// Gregorian calendar date to days since 1970-01-01.
// Does not depend on timegm().
int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= (month <= 2);

    const int era =
        (year >= 0 ? year : year - 399) / 400;

    const unsigned yearOfEra =
        static_cast<unsigned>(year - era * 400);

    const unsigned adjustedMonth =
        month > 2 ? month - 3 : month + 9;

    const unsigned dayOfYear =
        (153 * adjustedMonth + 2) / 5 + day - 1;

    const unsigned dayOfEra =
        yearOfEra * 365 +
        yearOfEra / 4 -
        yearOfEra / 100 +
        dayOfYear;

    return static_cast<int64_t>(era) * 146097 +
           static_cast<int64_t>(dayOfEra) - 719468;
}

} // namespace

// ============================================================
// INITIALIZATION
// ============================================================

bool ClockService::begin() {
    _rtcReady = _rtc.begin(&Wire);
    _rtcValid = false;

    _ntpStarted = false;
    _ntpSynced = false;

    _lastRtcProbe = millis();

    if (_rtcReady) {
        const DateTime dt = _rtc.now();

        _rtcValid =
            _rtc.isrunning() &&
            validEpoch(dt.unixtime());
    }

    // RTC failure must not prevent controller startup.
    return true;
}

// ============================================================
// RTC MONITORING
// ============================================================

void ClockService::tick() {
    const uint32_t now = millis();

    if (now - _lastRtcProbe < RTC_RETRY_MS) {
        return;
    }

    _lastRtcProbe = now;

    // Re-probe whether previously detected or not.
    _rtcReady = _rtc.begin(&Wire);

    if (!_rtcReady) {
        _rtcValid = false;
        _ntpSynced = false;
        return;
    }

    const bool running = _rtc.isrunning();
    const DateTime dt = _rtc.now();

    _rtcValid =
        running &&
        validEpoch(dt.unixtime());

    if (!_rtcValid) {
        _ntpSynced = false;
    }
}

// ============================================================
// TIME VALIDITY
// ============================================================

bool ClockService::valid() const {
    return _rtcReady && _rtcValid;
}

uint64_t ClockService::nowUtc() const {
    if (!valid()) {
        return 0;
    }

    const DateTime dt = _rtc.now();
    const uint64_t epoch = dt.unixtime();

    return validEpoch(epoch) ? epoch : 0;
}

// ============================================================
// NTP
// ============================================================

void ClockService::beginNetworkSync() {
    if (_ntpStarted) {
        return;
    }

    // System time remains UTC.
    // WIB conversion is performed separately.
    configTime(
        0,
        0,
        "pool.ntp.org",
        "time.google.com"
    );

    _ntpStarted = true;
}

void ClockService::syncFromNtpIfReady() {
    if (!_ntpStarted || !_rtcReady || _ntpSynced) {
        return;
    }

    const time_t systemNow = time(nullptr);

    if (!validEpoch(static_cast<uint64_t>(systemNow))) {
        return;
    }

    _rtc.adjust(
        DateTime(static_cast<uint32_t>(systemNow))
    );

    const DateTime updated = _rtc.now();

    _rtcValid =
        _rtc.isrunning() &&
        validEpoch(updated.unixtime());

    _ntpSynced = _rtcValid;
}

// ============================================================
// LOCAL TIME
// ============================================================

uint8_t ClockService::localHour() const {
    if (!valid()) {
        return 0;
    }

    const time_t localEpoch = static_cast<time_t>(
        nowUtc() +
        static_cast<int64_t>(
            Cfg::LOCAL_UTC_OFFSET_HOURS
        ) * 3600LL
    );

    struct tm tmv {};
    gmtime_r(&localEpoch, &tmv);

    return static_cast<uint8_t>(tmv.tm_hour);
}

uint16_t ClockService::localDateKey() const {
    if (!valid()) {
        return 0;
    }

    const time_t localEpoch = static_cast<time_t>(
        nowUtc() +
        static_cast<int64_t>(
            Cfg::LOCAL_UTC_OFFSET_HOURS
        ) * 3600LL
    );

    struct tm tmv {};
    gmtime_r(&localEpoch, &tmv);

    // MMDD key, matching LockerRecord::warningDate.
    return static_cast<uint16_t>(
        (tmv.tm_mon + 1) * 100 + tmv.tm_mday
    );
}

uint64_t ClockService::localTodayAt(uint8_t hour) const {
    if (!valid() || hour > 23) {
        return 0;
    }

    const time_t localEpoch = static_cast<time_t>(
        nowUtc() +
        static_cast<int64_t>(
            Cfg::LOCAL_UTC_OFFSET_HOURS
        ) * 3600LL
    );

    struct tm tmv {};
    gmtime_r(&localEpoch, &tmv);

    const int year = tmv.tm_year + 1900;

    const unsigned month =
        static_cast<unsigned>(tmv.tm_mon + 1);

    const unsigned day =
        static_cast<unsigned>(tmv.tm_mday);

    const int64_t localTarget =
        daysFromCivil(year, month, day) * 86400LL +
        static_cast<int64_t>(hour) * 3600LL;

    const int64_t utcTarget =
        localTarget -
        static_cast<int64_t>(
            Cfg::LOCAL_UTC_OFFSET_HOURS
        ) * 3600LL;

    return utcTarget > 0
        ? static_cast<uint64_t>(utcTarget)
        : 0;
}

// ============================================================
// FORMATTING
// ============================================================

void ClockService::formatLocal(char* out, size_t length) const {
    if (!out || length == 0) {
        return;
    }

    if (!valid()) {
        snprintf(out, length, "RTC unavailable");
        return;
    }

    const time_t localEpoch = static_cast<time_t>(
        nowUtc() +
        static_cast<int64_t>(
            Cfg::LOCAL_UTC_OFFSET_HOURS
        ) * 3600LL
    );

    struct tm tmv {};
    gmtime_r(&localEpoch, &tmv);

    snprintf(
        out,
        length,
        "%04d-%02d-%02d %02d:%02d:%02d WIB",
        tmv.tm_year + 1900,
        tmv.tm_mon + 1,
        tmv.tm_mday,
        tmv.tm_hour,
        tmv.tm_min,
        tmv.tm_sec
    );
}
