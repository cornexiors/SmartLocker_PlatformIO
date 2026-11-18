#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
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
    uint8_t assigned = 0;      // 0 = AVAILABLE, 1 = ASSIGNED (single-use)
    char uid[21] = {};         // RFID UID if assigned by RFID
    char pin[12] = {};         // 4-8 digit PIN if assigned by PIN
};

enum class ActionStatus : uint8_t {
    OK = 0,
    UNAUTHORIZED,
    ALREADY_ASSIGNED,
    NO_LOCKER_FREE,
    HARDWARE_ERROR
};

// ============================================================
// STORAGE (OFFLINE NVS PERSISTENCE)
// ============================================================

class Storage {
public:
    bool begin();

    void loadLockers(LockerRecord records[Cfg::LOCKER_COUNT]);
    void saveLocker(uint8_t index, const LockerRecord& record);
    void clearAllLockers();

    void saveWifiOverride(const String& ssid, const String& password);
    bool loadWifiOverride(String& ssid, String& password);
    void clearWifiOverride();

private:
    Preferences _prefs;
    Preferences _netCfg;
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
    bool setServoAngle(uint8_t index, int angle);

    void setLed(bool on);
    void beep(uint16_t durationMs = 60);

    // Graphic TFT UI (128x140)
    void drawTopBar(bool wifiOk, bool mqttOk, const char* title);
    void drawHome(bool wifiOk, bool mqttOk, uint8_t available, bool door1Closed);
    void drawCredentialPrompt(bool wifiOk, bool mqttOk, bool assigning, uint8_t pinLength);
    void updateCredentialPin(uint8_t pinLength);
    void drawResult(bool wifiOk, bool mqttOk,
                    const String& title, const String& detail, bool isError);

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
// NETWORK SERVICE (PLAIN MQTT ON PORT 1883)
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
    bool publishLockerState(uint8_t index, bool locked, bool assigned, const char* method);
    bool publishAck(uint8_t index, const String& payload);

    void setCommandHandler(void (*handler)(const String&));

private:
    static void callback(char* topic, byte* payload, unsigned int length);
    void connectMqtt();

    WiFiClient _plainClient;
    PubSubClient _mqtt;

    String _ssid;
    String _password;

    uint32_t _lastWifiAttempt = 0;
    uint32_t _lastMqttAttempt = 0;
    uint32_t _mqttBackoffMs = Cfg::MQTT_RETRY_MS;

    static NetworkService* _self;
    void (*_commandHandler)(const String&) = nullptr;
};

// ============================================================
// LOCKER MANAGER (OFFLINE SINGLE-USE LIFECYCLE)
// ============================================================

class LockerManager {
public:
    void begin(Storage& storage, Hardware& hardware, NetworkService& network);
    void tick();

    // User-facing offline assign & single-use unlock/release
    ActionStatus assignByUid(const String& uid, String& lockerName);
    ActionStatus assignByPin(const String& pin, String& lockerName);
    ActionStatus unlockAndReleaseByUid(const String& uid, String& lockerName);
    ActionStatus unlockAndReleaseByPin(const String& pin, String& lockerName);

    // Online / Serial control
    bool remoteSetLock(uint8_t index, bool locked, const char* source = "BACKEND");
    bool release(uint8_t index, const char* source = "BACKEND");
    void resetAllLockers();
    void broadcastAllStates(const char* method = "SYNC");

    // Queries
    int findLockerId(const String& id) const;
    uint8_t availableCount() const;

    bool isLocked(uint8_t index) const {
        return index < Cfg::LOCKER_COUNT ? _locked[index] : true;
    }

    const LockerRecord& record(uint8_t index) const {
        return _records[index];
    }

private:
    int findLockerByUid(const String& uid) const;
    int findLockerByPin(const String& pin) const;

    void saveLocker(uint8_t index);
    void emitEvent(const char* name, uint8_t index, const char* method);
    void publishFullLockerState(uint8_t index, const char* method);
    bool setLock(uint8_t index, bool locked, const char* method);

    Storage* _storage = nullptr;
    Hardware* _hardware = nullptr;
    NetworkService* _network = nullptr;

    LockerRecord _records[Cfg::LOCKER_COUNT];
    bool _locked[Cfg::LOCKER_COUNT] = {true, true, true, true};
};

// ============================================================
// USER INTERFACE
// ============================================================

class UserInterface {
public:
    void begin(Hardware& hw, LockerManager& lockers, NetworkService& net);
    void tick();

    void notifyRemoteCommand(const String& title, const String& detail, bool isError);

private:
    enum State : uint8_t {
        HOME,
        ASSIGN_CRED,
        UNLOCK_CRED,
        RESULT
    };

    void onKey(char key);
    void onRfidScanned(const String& uid);
    void showResult(const String& title, const String& detail, bool isError);
    void returnHome();
    void refreshCurrentScreen();

    Hardware* _hw = nullptr;
    LockerManager* _lockers = nullptr;
    NetworkService* _net = nullptr;

    State _state = HOME;
    String _pin;

    bool _hasPendingNotice = false;
    String _pendingTitle;
    String _pendingDetail;
    bool _pendingError = false;

    uint32_t _lastActivityMs = 0;
    uint32_t _lastKeyScan = 0;
    uint32_t _resultDeadline = 0;

    char _rawKey = 0;
    char _stableKey = 0;
    uint8_t _keyStable = 0;
    bool _keyReleased = true;
};

extern Storage gStorage;
extern Hardware gHardware;
extern NetworkService gNetwork;
extern LockerManager gLockers;
extern UserInterface gUi;