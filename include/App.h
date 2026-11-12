
#pragma once

#include <Arduino.h>
#include <WiFi.h>
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
// LOCKER RECORD
// ============================================================

struct LockerRecord {
    uint8_t flags = 0;
    uint8_t uidLength = 0;

    char uid[21] = {};
    char salt[17] = {};
    char pinHash[65] = {};

    uint64_t assignedAt = 0;
    uint64_t expiresAt = 0;

    uint8_t extensions = 0;
    uint8_t warningMask = 0;
    uint16_t warningDate = 0;
};

inline bool isAssigned(const LockerRecord& r) {
    return (r.flags & 0x01) != 0;
}

inline bool isExpiredHold(const LockerRecord& r) {
    return (r.flags & 0x02) != 0;
}

// ============================================================
// STORAGE
// ============================================================

class Storage {
public:
    bool begin();

    void load(LockerRecord records[Cfg::LOCKER_COUNT]);
    void save(uint8_t index, const LockerRecord& record);

    bool enqueue(const String& payload);
    bool peek(String& payload);
    void pop();

    bool sdReady() const {
        return _sdReady;
    }

    void logEvent(const String& payload);

private:
    Preferences _prefs;
    Preferences _queue;

    bool _sdReady = false;
    uint8_t _head = 0;
    uint8_t _tail = 0;
};

// ============================================================
// CLOCK SERVICE
// DS1307 is optional for startup and normal locker operation.
// Time-dependent expiry features require a valid RTC.
// ============================================================

class ClockService {
public:
    bool begin();
    void tick();

    bool valid() const;
    uint64_t nowUtc() const;

    void beginNetworkSync();
    void syncFromNtpIfReady();

    void formatLocal(char* out, size_t length) const;
    uint8_t localHour() const;
    uint16_t localDateKey() const;
    uint64_t localTodayAt(uint8_t hour) const;

    bool rtcReady() const {
        return _rtcReady;
    }

private:
    mutable RTC_DS1307 _rtc;

    bool _rtcReady = false;
    bool _rtcValid = false;

    bool _ntpStarted = false;
    bool _ntpSynced = false;

    uint32_t _lastRtcProbe = 0;
};

// ============================================================
// HARDWARE
// ============================================================

class Hardware {
public:
    bool begin();

    char scanKey();
    bool readCard(String& uid);

    bool doorClosed(uint8_t index);
    bool doorChanged(uint8_t index, bool& closed);

    void setServo(uint8_t index, bool locked);
    bool setLockerActuator(uint8_t index, bool locked);

    // P8-P15 digital outputs on the SAME PCF8575.
    bool setExpansionOutput(uint8_t bit, bool high);

    // Compatibility with existing main.cpp status output.
    bool expansionReady() const {
        return _pcfReady;
    }

    void setLed(bool on);
    void beep(uint16_t durationMs = 60);

    void drawHome(bool online, uint8_t available,
                  const char* timeText);

    void updateHome(bool online, uint8_t available,
                    const char* timeText);

    void drawMenu();
    void drawAuth(bool assigning, bool rfidMode);
    void drawPin(bool assigning, const String& maskedPin);
    void drawWaitRfid(bool assigning);
    void drawResult(const String& first, const String& second);
    void drawDoorState(uint8_t index, bool closed);

    bool displayReady() const {
        return _displayReady;
    }

    bool keypadReady() const {
        return _pcfReady;
    }

    bool rfidReady() const {
        return _rfidReady;
    }

    bool rtcBusReady() const {
        return _i2cReady;
    }

private:
    bool pcfWrite(uint16_t value);
    bool pcfRead(uint16_t& value);

    bool _i2cReady = false;
    bool _pcfReady = false;
    bool _displayReady = false;
    bool _rfidReady = false;

    uint8_t _pcfAddress = Cfg::PCF8575_ADDRESS;

    // P8-P15 initially released/high.
    uint16_t _pcfOutputState = 0xFF00;

    uint16_t _lastKeyBits = 0xFFFF;
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
    void begin();
    void tick();

    bool online();
    bool mqttConnected();

    bool publishEvent(const String& payload);
    bool publishStatus(uint8_t index, const char* state);
    bool publishAck(uint8_t index, const String& payload);

    void setCommandHandler(void (*handler)(const String&));

private:
    static void callback(
        char* topic,
        byte* payload,
        unsigned int length
    );

    void connectMqtt();

    WiFiClient _client;
    PubSubClient _mqtt{_client};

    uint32_t _lastWifiAttempt = 0;
    uint32_t _lastMqttAttempt = 0;

    static NetworkService* _self;

    void (*_commandHandler)(const String&) = nullptr;
};

// ============================================================
// LOCKER MANAGER
// ============================================================

class LockerManager {
public:
    void begin(
        Storage& storage,
        Hardware& hardware,
        ClockService& clock,
        NetworkService& network
    );

    void tick();

    bool assignByUid(const String& uid, String& lockerName);
    bool assignByPin(const String& pin, String& lockerName);

    bool unlockByUid(const String& uid, String& lockerName);
    bool unlockByPin(const String& pin, String& lockerName);

    bool extend(uint8_t index, String& reason);
    bool release(uint8_t index);

    int findByUid(const String& uid) const;
    int findByPin(const String& pin) const;
    int availableCount() const;
    int findLockerId(const String& id) const;

    bool remoteSet(uint8_t index, bool locked);

    bool isLocked(uint8_t index) const {
        return index < Cfg::LOCKER_COUNT
            ? _locked[index]
            : true;
    }

    const LockerRecord& record(uint8_t index) const {
        return _records[index];
    }

private:
    void save(uint8_t index);
    void event(const char* name, uint8_t index, const char* method);

    // Returns false if the hardware actuator operation fails.
    bool setLock(uint8_t index, bool locked);

    void processExpiryAndWarnings();

    Storage* _storage = nullptr;
    Hardware* _hardware = nullptr;
    ClockService* _clock = nullptr;
    NetworkService* _network = nullptr;

    LockerRecord _records[Cfg::LOCKER_COUNT];

    bool _locked[Cfg::LOCKER_COUNT] = {
        true, true, true, true
    };

    uint32_t _lastTimerCheck = 0;
};

// ============================================================
// USER INTERFACE
// ============================================================

class UserInterface {
public:
    void begin(
        Hardware& hw,
        LockerManager& lockers,
        ClockService& clock,
        NetworkService& net
    );

    void tick();

private:
    enum State : uint8_t {
        HOME,
        MENU,
        AUTH_METHOD,
        WAIT_RFID,
        PIN_ENTRY,
        RESULT
    };

    void onKey(char key);
    void finishResult(const String& a, const String& b);
    void returnHome();

    Hardware* _hw = nullptr;
    LockerManager* _lockers = nullptr;
    ClockService* _clock = nullptr;
    NetworkService* _net = nullptr;

    State _state = HOME;

    bool _assigning = false;
    bool _useRfid = false;

    String _pin;
    String _lastUid;
    String _resultA;
    String _resultB;

    uint32_t _lastUidAt = 0;
    uint32_t _lastKeyScan = 0;
    uint32_t _lastUiRefresh = 0;
    uint32_t _resultDeadline = 0;

    char _rawKey = 0;
    char _stableKey = 0;
    uint8_t _keyStable = 0;
};

// ============================================================
// GLOBAL OBJECTS AND SECURITY HELPERS
// ============================================================

extern Storage gStorage;
extern ClockService gClock;
extern Hardware gHardware;
extern NetworkService gNetwork;
extern LockerManager gLockers;
extern UserInterface gUi;

String makePinHash(const char* salt, const String& pin);
bool verifySignedCommand(const String& command, String& result);
