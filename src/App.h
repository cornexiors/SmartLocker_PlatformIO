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

struct PendingOtp {
    bool active = false;
    char userId[25] = {};
    char otp[12] = {};
    uint32_t expiresAtMs = 0;
};

enum class ActionStatus : uint8_t {
    OK = 0,
    UNAUTHORIZED,
    OFFLINE_BLOCKED,
    EXPIRED,
    ALREADY_ASSIGNED,
    NO_LOCKER_FREE,
    LOCKER_NOT_ASSIGNED,
    HARDWARE_ERROR
};

enum class RgbStatus : uint8_t {
    ONLINE_IDLE = 0,
    MQTT_CONNECTING,
    OFFLINE_LOCAL,
    ACTIVE_SESSION,
    ADMIN_MODE,
    ALERT_ERROR
};

// ============================================================
// FREERTOS DUAL-CORE IPC MESSAGES
// ============================================================

enum class AuthPurpose : uint8_t {
    ASSIGN_USER_PIN = 0,
    UNLOCK_LOCKER_PIN,
    D_MENU_PIN
};

enum class AuthOutcome : uint8_t {
    FAILED = 0,
    MATCHED_USER,
    MATCHED_RFID_OTP,
    MATCHED_ADMIN_PIN
};

struct AuthRequest {
    uint32_t requestId = 0;
    AuthPurpose purpose = AuthPurpose::ASSIGN_USER_PIN;
    uint8_t lockerIndex = 0;
    char pin[Cfg::PIN_MAX_LENGTH + 1] = {};
};

struct AuthResult {
    uint32_t requestId = 0;
    AuthPurpose purpose = AuthPurpose::ASSIGN_USER_PIN;
    uint8_t lockerIndex = 0;
    AuthOutcome outcome = AuthOutcome::FAILED;
    char userId[25] = {};
    char otpCode[12] = {};
};

enum class MqttInType : uint8_t {
    COMMAND = 0,
    USER_SYNC
};

struct MqttInMsg {
    MqttInType type = MqttInType::COMMAND;
    char payload[Cfg::MQTT_BUFFER_SIZE] = {};
};

struct MqttOutMsg {
    char topic[64] = {};
    char payload[Cfg::MQTT_BUFFER_SIZE] = {};
    bool retained = false;
};

// ============================================================
// SECURITY & CRYPTOGRAPHY (SHARED PURE-COMPUTE MODULE)
// ============================================================

class Security {
public:
    static String makePinHash(const char* salt, const String& pin);
    static String hmacSha256(const String& message);
    static bool secureEqual(const String& a, const String& b);
    static bool verifySignedPacket(const String& packet, uint8_t expectedFields,
                                   String* outFields, String& errorReason);

    static int findCachedCommand(const String& lockerId, const String& commandId);
    static String getCachedCommandAck(int cacheIndex);
    static void cacheCommand(const String& lockerId, const String& commandId, const String& ack);

    // Hardware & Serial Admin Credentials (stored as salted PBKDF2-SHA256 in NVS)
    static void initAdminCredentials();
    static bool verifySerialAdmin(const String& username, const String& password);

    static bool isDMenuAdminProvisioned();
    static bool provisionDMenuAdmin(const String& pin, const String& rfidUid);
    static bool verifyDMenuAdminPin(const String& pin);
    static bool verifyDMenuAdminRfid(const String& rfidUid);
    static void resetDMenuAdmin();
};

// ============================================================
// STORAGE (MUTEX-GUARDED NVS + LITTLEFS / SD LOGS)
// ============================================================

class Storage {
public:
    bool begin();

    void loadLockers(LockerRecord records[Cfg::LOCKER_COUNT]);
    void saveLocker(uint8_t index, const LockerRecord& record);
    void clearAllLockers();

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

    // Admin NVS accessors (guarded by storage mutex)
    bool getAdminString(const char* key, String& outVal);
    void putAdminString(const char* key, const String& val);
    void clearAdminNamespace();

    bool fsReady() const { return _fsReady; }
    bool sdReady() const { return _sdReady; }
    void logEvent(const String& line);
    void dumpLogs(Stream& out);
    void clearLogs();
    void factoryResetAll();

private:
    void lock() const;
    void unlock() const;

    mutable SemaphoreHandle_t _mutex = nullptr;
    Preferences _lockersPrefs;
    Preferences _usersPrefs;
    Preferences _queuePrefs;
    Preferences _netPrefs;
    Preferences _adminPrefs;

    bool _fsReady = false;
    bool _sdReady = false;
    uint8_t _head = 0;
    uint8_t _tail = 0;
};

// ============================================================
// CLOCK SERVICE (CORE 0 SNTP + ESP32-S3 INTERNAL RTC)
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
// HARDWARE & GRAPHIC TFT DRIVER (CORE 1)
// ============================================================

class Hardware {
public:
    bool begin();

    char scanKey();
    bool readCard(String& uid);

    bool doorClosed(uint8_t index);
    bool doorChanged(uint8_t index, bool& closed);

    bool setLockerActuator(uint8_t index, bool locked);
    bool setServoAngle(uint8_t index, int angle);

    void setOnlineLed(bool on);
    void setRgbStatus(RgbStatus status);
    void beep(uint16_t durationMs = 60);

    // Diagnostic probes for STATUS command
    bool probePcf8575();
    uint8_t probeMfrc522Version();

    // Graphic TFT UI (128x140)
    void drawTopBar(bool wifiOk, bool mqttOk, const char* shortTime, bool overdue);
    void drawHome(bool wifiOk, bool mqttOk, const char* shortTime,
                  uint8_t available, uint8_t overdueCount);
    void updateHomeTopBar(bool wifiOk, bool mqttOk, const char* shortTime, bool overdue);

    void drawQrRegister(bool wifiOk, bool mqttOk, const char* shortTime,
                        const char* reasonTitle, uint16_t secsLeft, uint16_t totalSecs);

    void drawDMenuPin(bool wifiOk, bool mqttOk, const char* shortTime,
                      uint8_t pinLength, uint16_t secsLeft, uint16_t totalSecs);

    void drawCredentialPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                              const char* header, const char* subheader,
                              uint8_t pinLength, uint8_t maxLen,
                              uint16_t secsLeft, uint16_t totalSecs);
    void updatePinBoxes(uint8_t pinLength, uint8_t maxLen);

    void drawVerifying(bool wifiOk, bool mqttOk, const char* shortTime);

    void drawLockerSelect(bool wifiOk, bool mqttOk, const char* shortTime,
                          const char* header, uint8_t selectedIdx,
                          const LockerRecord records[Cfg::LOCKER_COUNT],
                          const bool locked[Cfg::LOCKER_COUNT],
                          uint16_t secsLeft, uint16_t totalSecs);

    void drawUnlockAction(bool wifiOk, bool mqttOk, const char* shortTime,
                          const char* lockerId, uint16_t secsLeft, uint16_t totalSecs);

    void drawTapRfidPrompt(bool wifiOk, bool mqttOk, const char* shortTime,
                           const char* header, const char* line1, const char* line2,
                           uint16_t secsLeft, uint16_t totalSecs);

    void drawResult(bool wifiOk, bool mqttOk, const char* shortTime,
                    const String& title, const String& detail, bool isError,
                    uint16_t secsLeft, uint16_t totalSecs);

    void drawAdminMenu(bool wifiOk, bool mqttOk, const char* shortTime,
                       const LockerRecord records[Cfg::LOCKER_COUNT],
                       const bool locked[Cfg::LOCKER_COUNT],
                       uint8_t userCount, uint16_t secsLeft, uint16_t totalSecs);

    void drawAdminUidInput(bool wifiOk, bool mqttOk, const char* shortTime,
                           const String& userIdInput, uint16_t secsLeft, uint16_t totalSecs);

    void updateCountdownBadge(uint16_t secsLeft, uint16_t totalSecs);

    bool displayReady() const { return _displayReady; }
    bool keypadReady() const { return _pcfReady; }
    bool rfidReady() const { return _rfidReady; }

private:
    bool pcfWrite(uint16_t value);
    bool pcfRead(uint16_t& value);
    uint16_t lastPcfBits() const { return _lastPcfBits; }
    void drawConnectivityIcon(int16_t x, int16_t y, bool wifiOk, bool mqttOk);
    void drawQrMatrix(int16_t x, int16_t y, uint8_t scale, const char* text);

    bool _i2cReady = false;
    bool _pcfReady = false;
    bool _displayReady = false;
    bool _rfidReady = false;

    uint8_t _pcfAddress = Cfg::PCF8575_ADDRESS;
    uint16_t _pcfOutputState = 0xFF00;
    uint16_t _lastPcfBits = 0xFFFF;
    uint32_t _lastRfidMs = 0;

    bool _doorRaw[Cfg::LOCKER_COUNT] = {};
    bool _doorStable[Cfg::LOCKER_COUNT] = {};
    uint32_t _doorChangedAt[Cfg::LOCKER_COUNT] = {};

    RgbStatus _lastRgb = static_cast<RgbStatus>(0xFF);

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
// NETWORK SERVICE (CORE 0 WORKER)
// ============================================================

class NetworkService {
public:
    void begin(const char* ssid = Cfg::WIFI_SSID,
               const char* password = Cfg::WIFI_PASSWORD);
    void setWifiCredentials(const String& ssid, const String& password);
    String currentSsid() const;

    // Called exclusively by Core 0 NetCryptoTask
    void tickCore0();

    bool wifiConnected() const;
    bool mqttConnected() const;
    bool online() const;

    // Thread-safe enqueue from Core 1 -> Core 0
    bool queuePublish(const char* topic, const String& payload, bool retained = false);
    bool publishEvent(const String& payload);
    bool publishLockerState(uint8_t index, bool locked, bool assigned,
                            bool expired, const char* userId, const char* method);
    bool publishAck(uint8_t index, const String& payload);
    bool publishRfidRegAck(const String& userId, const String& uid,
                           bool success, const char* reason);
    bool publishUserSyncRequest(uint8_t userCount);

    void requestUserSync() { _userSyncPending = true; }
    bool userSyncPending() const { return _userSyncPending; }

private:
    static void callback(char* topic, byte* payload, unsigned int length);
    void connectMqtt();
    void drainOutbox();

    WiFiClientSecure _tlsClient;
    PubSubClient _mqtt;

    mutable SemaphoreHandle_t _cfgMutex = nullptr;
    String _ssid;
    String _password;
    volatile bool _reconnectRequested = false;

    uint32_t _lastWifiAttempt = 0;
    uint32_t _lastMqttAttempt = 0;
    uint32_t _mqttBackoffMs = Cfg::MQTT_RETRY_MS;
    volatile bool _mqttUp = false;
    volatile bool _userSyncPending = false;

    static NetworkService* _self;
};

// ============================================================
// LOCKER MANAGER (CORE 1 DOMAIN STATE MACHINE)
// ============================================================

class LockerManager {
public:
    void begin(Storage& storage, Hardware& hardware,
               ClockService& clock, NetworkService& network);
    void tick();

    // User-facing assign & unlock (Finish vs Extend)
    ActionStatus assignByUid(const String& uid, String& lockerName);
    ActionStatus assignByUserId(const String& userId, const char* method, String& lockerName);
    ActionStatus verifyRfidForLocker(uint8_t index, const String& uid, String& outUserId);
    ActionStatus unlockFinish(uint8_t index, const char* method, String& lockerName);
    ActionStatus unlockExtend(uint8_t index, const char* method, String& lockerName);

    // D-Menu RFID OTP Registration & Admin RFID Binding
    bool registerPendingOtp(const String& userId, const String& otp);
    bool matchPendingOtp(const String& otp, String& outUserId) const;
    ActionStatus completeOtpRfidRegistration(const String& otp, const String& uid, String& outUserId);
    ActionStatus adminBindUserRfid(const String& userId, const String& uid);

    // Admin / Peer-Sync Operations
    bool adminAssignUnregistered(uint8_t index, const char* source = "ADMIN_UI");
    bool remoteSetLock(uint8_t index, bool locked, const char* source = "BACKEND");
    bool release(uint8_t index, const char* source = "BACKEND");
    void overwriteClearExpired(const char* source = "ADMIN");
    void broadcastAllStates(const char* method = "SYNC");

    // Two-way user database sync
    bool upsertUser(const String& userId, const String& uid,
                    const String& salt, const String& pinHash);
    bool deleteUser(const String& userId);
    void clearUsers();
    bool assignFromBackend(const String& lockerId, const String& userId, bool open);
    bool unassignFromBackend(const String& lockerId, const String& userId);
    void clearAssignments();

    // Thread-safe PIN lookup helper executed on Core 0
    AuthResult evaluateAuthRequestOnCore0(const AuthRequest& req);

    uint8_t registeredUserCount() const;
    void listUsersToSerial(Stream& out) const;

    int findLockerId(const String& id) const;
    int findUserById(const String& userId) const;
    uint8_t availableCount() const;
    uint8_t problematicCount() const;

    bool isLocked(uint8_t index) const {
        return index < Cfg::LOCKER_COUNT ? _locked[index] : true;
    }

    const LockerRecord& record(uint8_t index) const {
        return _records[index];
    }

private:
    int findRegisteredUserByUid(const String& uid) const;
    int findAssignedLockerByUserId(const String& userId) const;

    void saveLocker(uint8_t index);
    void emitEvent(const char* name, uint8_t index, const char* method, const char* userId = "");
    void publishFullLockerState(uint8_t index, const char* method);
    bool setLock(uint8_t index, bool locked, const char* method);
    void checkExpirationsAndDoorAlerts();

    mutable SemaphoreHandle_t _dataMutex = nullptr;
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
// USER INTERFACE (CORE 1 NON-BLOCKING STATE MACHINE)
// ============================================================

class UserInterface {
public:
    void begin(Hardware& hw, LockerManager& lockers,
               ClockService& clock, NetworkService& net);
    void tick();

    void onAuthResult(const AuthResult& res);
    void notifyRemoteCommand(const String& title, const String& detail, bool isError);
    bool isAdminActive() const;
    bool isSessionActive() const;

private:
    enum State : uint8_t {
        HOME = 0,
        ASSIGN_CRED,
        UNLOCK_SELECT,
        UNLOCK_CRED,
        UNLOCK_ACTION,
        QR_REGISTER,
        D_MENU_PIN,
        RFID_OTP_TAP,
        ADMIN_SETUP_PIN,
        ADMIN_SETUP_RFID,
        ADMIN_AUTH_RFID,
        ADMIN_MENU,
        ADMIN_ASSIGN_SELECT,
        ADMIN_CONTROL_SELECT,
        ADMIN_SET_RFID_UID,
        ADMIN_SET_RFID_TAP,
        VERIFYING_PIN,
        RESULT
    };

    void onKey(char key);
    void onRfidScanned(const String& uid);
    void submitAsyncPinAuth(AuthPurpose purpose, uint8_t lockerIdx, const String& pin);
    void showQrRegister(const char* reasonTitle);
    void showResult(const String& title, const String& detail, bool isError);
    void enterAdminMenu();
    void returnHome();
    void touchActivity();
    uint16_t currentTimeoutSeconds() const;
    uint16_t remainingSeconds() const;
    void refreshCurrentScreen();

    Hardware* _hw = nullptr;
    LockerManager* _lockers = nullptr;
    ClockService* _clock = nullptr;
    NetworkService* _net = nullptr;

    State _state = HOME;
    String _pin;
    String _setupAdminPin;
    String _activeOtp;
    String _otpUserId;
    String _targetUserId;
    uint8_t _selectedLocker = 0;
    uint32_t _pendingAuthReqId = 0;

    bool _hasPendingNotice = false;
    String _pendingTitle;
    String _pendingDetail;
    bool _pendingError = false;

    uint32_t _lastActivityMs = 0;
    uint32_t _lastKeyScan = 0;
    uint32_t _lastCountdownDraw = 0;
    uint32_t _resultDeadline = 0;

    char _rawKey = 0;
    char _stableKey = 0;
    uint8_t _keyStable = 0;
    bool _keyReleased = true;
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
extern QueueHandle_t gMqttInQueue;
extern QueueHandle_t gMqttOutQueue;

void appLog(const char* level, const char* fmt, ...);