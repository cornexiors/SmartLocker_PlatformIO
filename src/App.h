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
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "Config.h"

// ============================================================
// DOMAIN DATA STRUCTURES
// ============================================================

struct LockerRecord {
    uint8_t flags = 0;         // 0x01 = ASSIGNED, 0x02 = EXPIRED_HOLD
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

struct AdminConfig {
    uint8_t provisioned = 0;
    char buildSig[32] = {};
    char rfidUid[21] = {};
    char pinSalt[17] = {};
    char pinHash[65] = {};
};

enum class ActionStatus : uint8_t {
    OK = 0,
    UNAUTHORIZED,
    EXPIRED,
    ALREADY_ASSIGNED,
    NO_LOCKER_FREE,
    OFFLINE_BLOCKED,
    HARDWARE_ERROR
};

enum class RgbState : uint8_t {
    ONLINE_IDLE = 0,   // Green
    MQTT_CONNECTING,   // Blue
    OFFLINE_LOCAL,     // Amber
    ACTIVE_SESSION,    // Cyan
    ADMIN_ACTIVE,      // Purple
    ALERT_ERROR        // Red
};

// ============================================================
// INTER-CORE FREERTOS IPC MESSAGES
// ============================================================

enum class AuthRequestType : uint8_t {
    USER_ASSIGN_PIN = 0,
    USER_UNLOCK_PIN,
    ADMIN_SERIAL_LOGIN,
    ADMIN_DMENU_PIN
};

struct AuthRequest {
    uint32_t requestId = 0;
    AuthRequestType type = AuthRequestType::USER_ASSIGN_PIN;
    int8_t lockerIndex = -1;
    char username[32] = {};
    char pin[12] = {};
};

struct AuthResult {
    uint32_t requestId = 0;
    AuthRequestType type = AuthRequestType::USER_ASSIGN_PIN;
    bool matched = false;
    int8_t lockerIndex = -1;
    int16_t userSlot = -1;
    char userId[25] = {};
};

struct MqttInMsg {
    char topic[72] = {};
    char payload[Cfg::MQTT_BUFFER_SIZE] = {};
};

// ============================================================
// SECURITY & CRYPTOGRAPHY (SHARED PURE-COMPUTE + REPLAY GUARD)
// ============================================================

namespace Security {
    String generateSaltHex();
    String makePinHash(const char* salt, const String& pin);
    bool verifyPinHash(const char* salt, const String& pin, const char* expectedHash);
    bool secureEqual(const String& a, const String& b);
    String hmacSha256(const String& message);

    bool verifySignedPacket(const String& packet, uint8_t expectedFields,
                            String* outFields, String& errorReason,
                            bool checkTimestamp, uint64_t nowUtc);

    int findCachedCommand(const String& lockerId, const String& commandId);
    String getCachedCommandAck(int cacheIndex);
    void cacheCommand(const String& lockerId, const String& commandId, const String& ack);

    bool verifySerialAdmin(const String& username, const String& password);
}

// ============================================================
// MUTEX-PROTECTED STORAGE (NVS + LITTLEFS + SD)
// ============================================================

class Storage {
public:
    bool begin();

    void loadLockers(LockerRecord records[Cfg::LOCKER_COUNT]);
    void saveLocker(uint8_t index, const LockerRecord& record);

    void loadUsers(UserRecord users[Cfg::MAX_USERS]);
    void saveUser(uint8_t slot, const UserRecord& user);
    void clearAllUsers();

    void loadAdminConfig(AdminConfig& cfg);
    void saveAdminConfig(const AdminConfig& cfg);
    void clearAdminConfig();

    bool enqueue(const String& payload);
    bool peek(String& payload);
    void pop();
    uint8_t queueCount() const;

    void saveWifiOverride(const String& ssid, const String& password);
    bool loadWifiOverride(String& ssid, String& password);
    void clearWifiOverride();

    void factoryResetAll();

    bool sdReady() const { return _sdReady; }
    bool fsReady() const { return _fsReady; }
    void logEvent(const String& payload);
    void dumpLogs(Stream& out);
    void clearLogs();

private:
    SemaphoreHandle_t _mutex = nullptr;
    Preferences _prefs;
    Preferences _users;
    Preferences _queue;
    Preferences _netCfg;
    Preferences _adminPrefs;

    bool _sdReady = false;
    bool _fsReady = false;
    uint8_t _head = 0;
    uint8_t _tail = 0;
};

// ============================================================
// CLOCK SERVICE (CORE 0 SNTP + INTERNAL ESP32-S3 RTC)
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

private:
    bool _ntpStarted = false;
    uint32_t _lastNtpSyncMs = 0;
};

// ============================================================
// HARDWARE & ST7735 GRAPHIC DRIVER (CORE 1)
// ============================================================

class Hardware {
public:
    bool begin();

    char scanKey();
    bool readCard(String& uid);

    bool doorClosed(uint8_t index);
    bool doorChanged(uint8_t index, bool& closed);

    bool setLockerActuator(uint8_t index, bool locked);
    bool setServoAngleDirect(uint8_t index, int angle);

    void setOnlineLed(bool on);
    void setRgbState(RgbState state);
    void testRgbCycle();
    void beep(uint16_t durationMs = 60);

    // Graphic TFT UI (128x140)
    void drawTopBar(bool wifiOk, bool mqttOk, const char* shortTime);
    void updateTopClock(bool wifiOk, bool mqttOk, const char* shortTime);
    void drawCountdownBadge(uint16_t secondsLeft, uint16_t totalSeconds);

    void drawHome(bool wifiOk, bool mqttOk, const char* shortTime,
                  uint8_t available, uint8_t problematic);

    void drawQrRegister(bool wifiOk, bool mqttOk, const char* shortTime,
                        const char* url, const char* subtitle, uint16_t secLeft);

    void drawCredentialPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                              const char* header, const char* hint,
                              uint8_t pinLength, uint16_t secLeft);
    void updatePinBoxes(uint8_t pinLength);

    void drawLockerSelector(bool wifiOk, bool mqttOk, const char* shortTime,
                            const LockerRecord records[Cfg::LOCKER_COUNT],
                            const bool locked[Cfg::LOCKER_COUNT],
                            uint8_t selectedIndex, uint16_t secLeft);

    void drawUnlockActionPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                                uint8_t lockerIndex, uint16_t secLeft);

    void drawDMenuFirstPage(bool wifiOk, bool mqttOk, const char* shortTime,
                            const char* url, uint8_t pinLength, uint16_t secLeft);

    void drawTapCardPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                           const char* title, const char* subtitle, uint16_t secLeft);

    void drawResult(bool wifiOk, bool mqttOk, const char* shortTime,
                    const String& title, const String& detail,
                    bool isError, uint16_t secLeft);

    void drawAdminMenu(bool wifiOk, bool mqttOk, const char* shortTime,
                       const LockerRecord records[Cfg::LOCKER_COUNT],
                       const bool locked[Cfg::LOCKER_COUNT],
                       uint8_t userCount, uint16_t secLeft);

    void drawAdminTargetPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                               const char* title, const char* prompt,
                               const String& value, uint16_t secLeft);

    bool displayReady() const { return _displayReady; }
    bool keypadReady() const { return _pcfReady; }
    bool rfidReady() const { return _rfidReady; }
    uint8_t rfidVersionReg();

private:
    bool pcfWrite(uint16_t mask);
    bool pcfRead(uint16_t& value);
    void drawConnectivityIcon(int16_t x, int16_t y, bool wifiOk, bool mqttOk);
    void drawQrMatrix(int16_t x0, int16_t y0, uint8_t scale, const char* url);

    bool _i2cReady = false;
    bool _pcfReady = false;
    bool _displayReady = false;
    bool _rfidReady = false;

    uint8_t _pcfAddress = Cfg::PCF8575_ADDRESS;
    uint16_t _pcfLastWord = 0xFFFF;
    uint32_t _lastRfidMs = 0;
    RgbState _lastRgb = RgbState::OFFLINE_LOCAL;

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
// NETWORK & CRYPTO WORKER SERVICE (CORE 0)
// ============================================================

class NetworkService {
public:
    void begin(const char* ssid = Cfg::WIFI_SSID,
               const char* password = Cfg::WIFI_PASSWORD);
    void setWifiCredentials(const String& ssid, const String& password);
    String currentSsid() const;

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

    bool dequeueIncomingMqtt(MqttInMsg& out);

private:
    static void callback(char* topic, byte* payload, unsigned int length);
    void connectMqtt();

    WiFiClientSecure _tlsClient;
    PubSubClient _mqtt;

    mutable SemaphoreHandle_t _netMutex = nullptr;
    QueueHandle_t _inQueue = nullptr;

    String _ssid;
    String _password;
    volatile bool _wifiCredsDirty = false;

    uint32_t _lastWifiAttempt = 0;
    uint32_t _lastMqttAttempt = 0;
    uint32_t _mqttBackoffMs = Cfg::MQTT_RETRY_MS;
    volatile bool _userSyncPending = false;

    static NetworkService* _self;
};

// ============================================================
// LOCKER MANAGER (DOMAIN STATE MACHINE)
// ============================================================

class LockerManager {
public:
    void begin(Storage& storage, Hardware& hardware,
               ClockService& clock, NetworkService& network);
    void tick();

    // User-facing assign & unlock (Finish vs Extend)
    ActionStatus assignByUid(const String& uid, String& lockerName);
    ActionStatus assignByUserSlot(int userSlot, const char* method, String& lockerName);

    ActionStatus verifyUnlockByUid(uint8_t lockerIdx, const String& uid, String& outUserId);
    bool verifyUnlockByUserSlot(uint8_t lockerIdx, int userSlot, String& outUserId);
    ActionStatus executeUnlockFinish(uint8_t lockerIdx, const char* method, String& lockerName);
    ActionStatus executeUnlockExtend(uint8_t lockerIdx, const char* method, String& lockerName);

    // D-Menu RFID OTP & Admin RFID Registration
    bool registerPendingOtp(const String& userId, const String& otp);
    bool verifyOtp(const String& otp, String& outUserId);
    ActionStatus completeOtpRfidRegistration(const String& otp, const String& uid, String& outUserId);
    ActionStatus adminSetUserRfid(const String& userId, const String& uid);

    // Admin / Remote / Serial controls
    bool adminAssignUnregistered(uint8_t index, const String& label);
    bool remoteSetLock(uint8_t index, bool locked, const char* source = "BACKEND");
    bool release(uint8_t index, const char* source = "BACKEND");
    void overwriteClearExpired(int indexOrAll, const char* source = "ADMIN");
    void broadcastAllStates(const char* method = "SYNC");

    // Admin hardware provisioning (D-menu first-boot & 2FA)
    bool isAdminProvisioned() const;
    bool provisionAdmin(const String& pin, const String& rfidUid);
    bool verifyAdminRfid(const String& rfidUid) const;
    void resetAdminProvisioning();

    // Synced User Database
    bool upsertUser(const String& userId, const String& uid,
                    const String& salt, const String& pinHash);
    bool deleteUser(const String& userId);
    void clearUsers();
    bool assignFromBackend(const String& lockerId, const String& userId, bool open);
    bool unassignFromBackend(const String& lockerId, const String& userId);
    void clearAssignments();
    uint8_t registeredUserCount() const;
    void listUsersToSerial(Stream& out) const;

    // Async Core 0 PBKDF2 Auth Worker Support
    void processAuthRequestOnCore0(const AuthRequest& req, AuthResult& res);

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
    int findRegisteredUserByUserId(const String& userId) const;
    int findAssignedLockerByUserId(const String& userId) const;

    void saveLocker(uint8_t index);
    void emitEvent(const char* name, uint8_t index, const char* method, const char* userId = "");
    void publishFullLockerState(uint8_t index, const char* method);
    bool setLock(uint8_t index, bool locked, const char* method);
    void checkExpirationsAndDoorAlerts();

    Storage* _storage = nullptr;
    Hardware* _hardware = nullptr;
    ClockService* _clock = nullptr;
    NetworkService* _network = nullptr;

    mutable SemaphoreHandle_t _stateMutex = nullptr;
    LockerRecord _records[Cfg::LOCKER_COUNT];
    UserRecord _users[Cfg::MAX_USERS];
    AdminConfig _adminCfg;
    PendingOtp _otps[Cfg::MAX_PENDING_OTPS];

    bool _locked[Cfg::LOCKER_COUNT] = {true, true, true, true};
    uint32_t _doorOpenedAt[Cfg::LOCKER_COUNT] = {0, 0, 0, 0};
    uint32_t _lastDoorAlertAt[Cfg::LOCKER_COUNT] = {0, 0, 0, 0};
    bool _doorTimeoutReported[Cfg::LOCKER_COUNT] = {false, false, false, false};

    uint32_t _lastTimerCheck = 0;
};

// ============================================================
// USER INTERFACE STATE MACHINE (CORE 1)
// ============================================================

class UserInterface {
public:
    void begin(Hardware& hw, LockerManager& lockers,
               ClockService& clock, NetworkService& net);
    void tick();

    void onAsyncAuthResult(const AuthResult& res);
    void notifyRemoteCommand(const String& title, const String& detail, bool isError);
    bool isAdminActive() const;
    bool isSessionBusy() const;

private:
    enum State : uint8_t {
        HOME = 0,
        QR_REGISTER,
        ASSIGN_CRED,
        UNLOCK_SELECT,
        UNLOCK_CRED,
        UNLOCK_ACTION,
        D_MENU_PIN,
        RFID_OTP_TAP,
        ADMIN_SETUP_PIN,
        ADMIN_SETUP_RFID,
        ADMIN_AUTH_RFID,
        ADMIN_MENU,
        ADMIN_PICK_LOCKER,
        ADMIN_SET_RFID_UID,
        ADMIN_SET_RFID_TAP,
        RESULT
    };

    enum AdminOp : uint8_t {
        OP_NONE = 0,
        OP_ASSIGN_UNREG,
        OP_TOGGLE_LOCK,
        OP_RELEASE
    };

    void onKey(char key);
    void onRfidScanned(const String& uid);
    void submitAsyncPin(AuthRequestType type, int8_t lockerIdx, const String& pin);
    void showResult(const String& title, const String& detail, bool isError);
    void showQrPrompt(const char* subtitle);
    void enterState(State next, uint32_t timeoutMs);
    void returnHome();
    void touchActivity();
    uint16_t remainingSeconds() const;
    void refreshCurrentScreen(bool fullRedraw);

    Hardware* _hw = nullptr;
    LockerManager* _lockers = nullptr;
    ClockService* _clock = nullptr;
    NetworkService* _net = nullptr;

    State _state = HOME;
    AdminOp _adminOp = OP_NONE;

    String _inputBuf;
    String _activeOtp;
    String _pendingUserId;
    String _setupAdminPin;
    uint8_t _selectedLocker = 0;
    uint32_t _pendingReqId = 0;
    bool _verifyingAsync = false;

    bool _hasPendingNotice = false;
    String _pendingTitle;
    String _pendingDetail;
    bool _pendingError = false;

    uint32_t _stateEnteredMs = 0;
    uint32_t _stateTimeoutMs = 0;
    uint32_t _lastKeyScan = 0;
    uint32_t _lastUiRefresh = 0;

    char _rawKey = 0;
    char _stableKey = 0;
    uint8_t _keyStable = 0;
};

// ============================================================
// GLOBALS & FREERTOS QUEUES
// ============================================================

extern Storage gStorage;
extern ClockService gClock;
extern Hardware gHardware;
extern NetworkService gNetwork;
extern LockerManager gLockers;
extern UserInterface gUi;

extern QueueHandle_t gAuthReqQueue;
extern QueueHandle_t gAuthResQueue;

void appLog(const char* level, const char* fmt, ...);
