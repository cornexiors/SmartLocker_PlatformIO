#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>

#include "Config.h"

// ============================================================
// DATA STRUCTURES
// ============================================================

struct LockerRecord {
    uint8_t flags = 0;         // 0x01 = ASSIGNED, 0x02 = EXPIRED_HOLD (>24h)
    char userId[25] = {};
    char uid[21] = {};
    char salt[17] = {};
    char pinHash[65] = {};
    uint64_t assignedAt = 0;
    uint64_t expiresAt = 0;
};

inline bool isAssigned(const LockerRecord& r) {
    return (r.flags & 0x01) != 0;
}

inline bool isExpiredHold(const LockerRecord& r) {
    return (r.flags & 0x02) != 0;
}

struct UserRecord {
    uint8_t active = 0;
    char userId[25] = {};
    char uid[21] = {};
    char salt[17] = {};
    char pinHash[65] = {};
};

enum class ActionStatus : uint8_t {
    OK = 0,
    UNAUTHORIZED,
    EXPIRED,
    ALREADY_ASSIGNED,
    NO_LOCKER_FREE,
    HARDWARE_ERROR
};

// ============================================================
// STORAGE
// ============================================================

class Storage {
public:
    bool begin();

    void loadLockers(LockerRecord records[Cfg::LOCKER_COUNT]);
    void saveLocker(uint8_t index, const LockerRecord& record);

    void loadUsers(UserRecord users[Cfg::MAX_USERS]);
    void saveUser(uint8_t slot, const UserRecord& user);
    void clearAllUsers();

    bool enqueue(const String& payload);
    bool peek(String& payload);
    void pop();
    uint8_t queueCount() const;

    void saveWifiOverride(const String& ssid, const String& password);
    bool loadWifiOverride(String& ssid, String& password);
    void clearWifiOverride();

    bool sdReady() const { return _sdReady; }
    void logEvent(const String& payload);
    void dumpSdLog(Stream& out);
    void clearSdLog();

private:
    Preferences _prefs;
    Preferences _users;
    Preferences _queue;
    Preferences _netCfg;

    bool _sdReady = false;
    uint8_t _head = 0;
    uint8_t _tail = 0;
};

// ============================================================
// CLOCK SERVICE
// ============================================================

class ClockService {
public:
    bool begin();
    void tick(bool wifiConnected);

    bool valid() const;
    uint64_t nowUtc() const;

    void forceNtpSync();

    void formatLocal(char* out, size_t length) const;
    void formatShortLocal(char* out, size_t length) const;

    bool externalRtcReady() const { return _extRtcReady; }

private:
    void setSystemEpoch(uint64_t epoch);

    mutable RTC_DS1307 _rtc;
    bool _extRtcReady = false;
    bool _ntpStarted = false;
    uint32_t _lastRtcProbe = 0;
    uint32_t _lastNtpSyncMs = 0;
};

// ============================================================
// HARDWARE & GRAPHIC UI DRIVER
// ============================================================

class Hardware {
public:
    bool begin();

    char scanKey();
    bool readCard(String& uid);

    bool doorClosed(uint8_t index);
    bool doorChanged(uint8_t index, bool& closed);

    bool setLockerActuator(uint8_t index, bool locked);
    bool setExpansionOutput(uint8_t bit, bool high);

    void setLed(bool on);
    void beep(uint16_t durationMs = 60);

    // Graphic TFT UI (128x140)
    void drawTopBar(bool wifiOk, bool mqttOk, const char* shortTime);
    void drawHome(bool wifiOk, bool mqttOk, const char* shortTime,
                  uint8_t available, uint8_t problematic);
    void updateHomeClock(bool wifiOk, bool mqttOk, const char* shortTime);

    void drawRegisterPrompt(bool wifiOk, bool mqttOk, const char* shortTime);
    void drawQrCode(bool wifiOk, bool mqttOk, const char* shortTime, const char* url);

    void drawCredentialPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                              bool assigning, uint8_t pinLength);
    void updateCredentialPin(uint8_t pinLength);

    void drawOtpPrompt(bool wifiOk, bool mqttOk, const char* shortTime, uint8_t pinLength);
    void drawOtpTapRfid(bool wifiOk, bool mqttOk, const char* shortTime, const char* userId);

    void drawResult(bool wifiOk, bool mqttOk, const char* shortTime,
                    const String& title, const String& detail, bool isError);

    void drawAdminMenu(bool wifiOk, bool mqttOk, const char* shortTime,
                       const LockerRecord records[Cfg::LOCKER_COUNT],
                       const bool locked[Cfg::LOCKER_COUNT],
                       uint8_t userCount);

    bool displayReady() const { return _displayReady; }
    bool keypadReady() const { return _pcfReady; }
    bool rfidReady() const { return _rfidReady; }

private:
    bool pcfWrite(uint16_t value);
    bool pcfRead(uint16_t& value);
    void drawConnectivityIcon(int16_t x, int16_t y, bool wifiOk, bool mqttOk);
    void drawPinUnderlineBoxes(uint8_t pinLength);

    bool _i2cReady = false;
    bool _pcfReady = false;
    bool _displayReady = false;
    bool _rfidReady = false;

    uint8_t _pcfAddress = Cfg::PCF8575_ADDRESS;
    uint16_t _pcfOutputState = 0xFF00;
    uint32_t _lastRfidMs = 0;

    bool _doorRaw[Cfg::LOCKER_COUNT] = {};
    bool _doorStable[Cfg::LOCKER_COUNT] = {};
    uint32_t _doorChangedAt[Cfg::LOCKER_COUNT] = {};

    Servo _servos[Cfg::LOCKER_COUNT];

    MFRC522DriverPinSimple _rfidSS{Cfg::RFID_CS};
    MFRC522DriverSPI _rfidDriver{_rfidSS, SPI};
    MFRC522 _rfid{_rfidDriver};

    Adafruit_ST7735 _tft{
        &SPI,
        Cfg::TFT_CS,
        Cfg::TFT_DC,
        Cfg::TFT_RST
    };
};

// ============================================================
// NETWORK SERVICE
// ============================================================

class NetworkService {
public:
    void begin(const char* ssid = Cfg::WIFI_SSID,
               const char* password = Cfg::WIFI_PASSWORD);
    void setWifiCredentials(const String& ssid, const String& password);
    String currentSsid() const { return _ssid; }

    void tick();

    bool wifiConnected() const;
    bool mqttConnected();
    bool online();

    bool publishEvent(const String& payload);
    bool publishLockerState(uint8_t index, bool locked, bool assigned,
                            bool expired, const char* userId, const char* method);
    bool publishAck(uint8_t index, const String& payload);
    bool publishRfidRegAck(const String& userId, const String& uid,
                           bool success, const char* reason);
    bool publishUserSyncRequest(uint8_t userCount);
    void requestUserSync() { _userSyncPending = true; }
    bool userSyncPending() const { return _userSyncPending; }

    void setCommandHandler(void (*handler)(const String&));
    void setUserSyncHandler(void (*handler)(const String&));

private:
    static void callback(char* topic, byte* payload, unsigned int length);
    void connectMqtt();

    WiFiClientSecure _tlsClient;
    PubSubClient _mqtt;

    String _ssid;
    String _password;

    uint32_t _lastWifiAttempt = 0;
    uint32_t _lastMqttAttempt = 0;
    uint32_t _mqttBackoffMs = Cfg::MQTT_RETRY_MS;
    bool _userSyncPending = false;

    static NetworkService* _self;
    void (*_commandHandler)(const String&) = nullptr;
    void (*_userSyncHandler)(const String&) = nullptr;
};

// ============================================================
// LOCKER MANAGER
// ============================================================

class LockerManager {
public:
    void begin(Storage& storage, Hardware& hardware,
               ClockService& clock, NetworkService& network);
    void tick();

    // User-facing assign & single-use unlock/release
    ActionStatus assignByUid(const String& uid, String& lockerName);
    ActionStatus assignByPin(const String& pin, String& lockerName);
    ActionStatus unlockAndReleaseByUid(const String& uid, String& lockerName);
    ActionStatus unlockAndReleaseByPin(const String& pin, String& lockerName);

    // D-mode self-service RFID registration via App OTP
    bool registerPendingOtp(const String& userId, const String& otp);
    bool verifyOtp(const String& otp, String& outUserId);
    ActionStatus completeOtpRfidRegistration(const String& otp, const String& uid,
                                             String& outUserId);

    // Remote / Admin / Serial operations
    bool remoteSetLock(uint8_t index, bool locked, const char* source = "BACKEND");
    bool release(uint8_t index, const char* source = "BACKEND");
    void broadcastAllStates(const char* method = "SYNC");

    // Two-way user database sync
    bool upsertUser(const String& userId, const String& uid,
                    const String& salt, const String& pinHash);
    bool deleteUser(const String& userId);
    void clearUsers();
    bool assignFromBackend(const String& lockerId, const String& userId, bool open);
    bool unassignFromBackend(const String& lockerId, const String& userId);
    void clearAssignments();
    uint8_t registeredUserCount() const;
    void listUsersToSerial(Stream& out) const;

    // Queries
    int findLockerId(const String& id) const;
    uint8_t availableCount() const;
    uint8_t problematicCount() const;

    bool isLocked(uint8_t index) const {
        return index < Cfg::LOCKER_COUNT ? _locked[index] : true;
    }

    const LockerRecord& record(uint8_t index) const {
        return _records[index];
    }

private:
    struct PendingOtp {
        bool active = false;
        char userId[25] = {};
        char otp[12] = {};
        uint32_t expiresAtMs = 0;
    };

    int findRegisteredUserByUid(const String& uid) const;
    int findRegisteredUserByPin(const String& pin) const;
    int findAssignedLockerByUserId(const String& userId) const;
    int findAssignedLockerByUid(const String& uid) const;
    int findAssignedLockerByPin(const String& pin) const;

    ActionStatus releaseUnlockedLocker(int lockerIdx, const char* method, String& lockerName);

    void saveLocker(uint8_t index);
    void emitEvent(const char* name, uint8_t index, const char* method, const char* userId = "");
    void publishFullLockerState(uint8_t index, const char* method);
    bool setLock(uint8_t index, bool locked, const char* method);
    void checkExpirationsAndDoorAlerts();

    Storage* _storage = nullptr;
    Hardware* _hardware = nullptr;
    ClockService* _clock = nullptr;
    NetworkService* _network = nullptr;

    LockerRecord _records[Cfg::LOCKER_COUNT];
    UserRecord _users[Cfg::MAX_USERS];
    PendingOtp _otps[Cfg::MAX_PENDING_OTPS];

    bool _locked[Cfg::LOCKER_COUNT] = {true, true, true, true};
    uint32_t _doorOpenedAt[Cfg::LOCKER_COUNT] = {0, 0, 0, 0};
    uint32_t _lastDoorAlertAt[Cfg::LOCKER_COUNT] = {0, 0, 0, 0};
    bool _doorTimeoutReported[Cfg::LOCKER_COUNT] = {false, false, false, false};

    uint32_t _lastTimerCheck = 0;
};

// ============================================================
// USER INTERFACE
// ============================================================

class UserInterface {
public:
    void begin(Hardware& hw, LockerManager& lockers,
               ClockService& clock, NetworkService& net);
    void tick();

    void notifyRemoteCommand(const String& title, const String& detail, bool isError);

private:
    enum State : uint8_t {
        HOME,
        ASSIGN_ASK,
        QR_CODE,
        ASSIGN_CRED,
        UNLOCK_CRED,
        OTP_ENTRY,
        OTP_TAP_RFID,
        RESULT,
        ADMIN_MENU
    };

    void onKey(char key);
    void onRfidScanned(const String& uid);
    void showResult(const String& title, const String& detail, bool isError);
    void returnHome();
    void touchActivity();
    void refreshCurrentScreen();

    Hardware* _hw = nullptr;
    LockerManager* _lockers = nullptr;
    ClockService* _clock = nullptr;
    NetworkService* _net = nullptr;

    State _state = HOME;
    String _pin;
    String _activeOtp;
    String _otpUserId;

    bool _hasPendingNotice = false;
    String _pendingTitle;
    String _pendingDetail;
    bool _pendingError = false;

    uint32_t _lastActivityMs = 0;
    uint32_t _lastKeyScan = 0;
    uint32_t _lastUiRefresh = 0;
    uint32_t _resultDeadline = 0;

    char _rawKey = 0;
    char _stableKey = 0;
    uint8_t _keyStable = 0;
};

// ============================================================
// GLOBALS & HELPERS
// ============================================================

extern Storage gStorage;
extern ClockService gClock;
extern Hardware gHardware;
extern NetworkService gNetwork;
extern LockerManager gLockers;
extern UserInterface gUi;

String makePinHash(const char* salt, const String& pin);
bool verifySignedPacket(const String& packet, uint8_t expectedFields,
                        String* outFields, String& errorReason);
void appLog(const char* level, const char* fmt, ...);