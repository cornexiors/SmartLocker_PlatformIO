#include "App.h"
#include <esp_system.h>

Storage gStorage;
Hardware gHardware;
NetworkService gNetwork;
LockerManager gLockers;
UserInterface gUi;

static String serialBuffer;

// ============================================================
// PIPE-DELIMITED ONLINE COMMAND HANDLER
// Accepts: LOCKER_001|LOCK, LOCKER_001|UNLOCK, LOCKER_001|RELEASE
// (Also tolerates extra trailing pipe fields for backward compatibility)
// ============================================================

static void handleRemoteCommand(const String& command) {
    const int p1 = command.indexOf('|');
    if (p1 <= 0) return;

    String lockerId = command.substring(0, p1);
    lockerId.trim();
    lockerId.toUpperCase();

    const int p2 = command.indexOf('|', p1 + 1);
    String action = (p2 > p1)
        ? command.substring(p1 + 1, p2)
        : command.substring(p1 + 1);
    action.trim();
    action.toUpperCase();

    const int index = gLockers.findLockerId(lockerId);
    if (index < 0) {
        Serial.printf("REJECTED|%s|UNKNOWN_LOCKER\n", lockerId.c_str());
        return;
    }

    bool accepted = false;
    String result = "ACCEPTED";

    if (action == "LOCK") {
        accepted = gLockers.remoteSetLock(index, true, "BACKEND");
        if (!accepted) result = "ACTUATOR_ERROR";
    } else if (action == "UNLOCK") {
        accepted = gLockers.remoteSetLock(index, false, "BACKEND");
        if (!accepted) result = "ACTUATOR_ERROR";
    } else if (action == "RELEASE") {
        accepted = gLockers.release(index, "BACKEND");
        if (!accepted) result = "ACTUATOR_ERROR";
    } else {
        result = "INVALID_ACTION";
    }

    const String ack = String(accepted ? "ACCEPTED|" : "REJECTED|") +
                       lockerId + '|' + action + '|' + result;
    gNetwork.publishAck(index, ack);
    Serial.println(ack);

    if (accepted) {
        gUi.notifyRemoteCommand(String("REMOTE ") + action, lockerId, false);
    }
}

// ============================================================
// SERIAL HELPER COMMANDS FOR LIVE TESTING
// ============================================================

static int parseLockerArg(const String& rawArg) {
    String arg = rawArg;
    arg.trim();
    if (arg.length() == 1 && arg[0] >= '1' && arg[0] <= '9') {
        const int idx = arg[0] - '1';
        return (idx >= 0 && idx < Cfg::LOCKER_COUNT) ? idx : -1;
    }
    return gLockers.findLockerId(arg);
}

static void handleSerial() {
    while (Serial.available()) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r') continue;
        if (c != '\n') {
            if (serialBuffer.length() < 240) serialBuffer += c;
            continue;
        }

        serialBuffer.trim();
        if (serialBuffer.isEmpty()) continue;

        if (serialBuffer.indexOf('|') >= 0 && !serialBuffer.startsWith("WIFI ")) {
            handleRemoteCommand(serialBuffer);
            serialBuffer = "";
            continue;
        }

        String upper = serialBuffer;
        upper.toUpperCase();

        if (upper == "HELP") {
            Serial.println(F("=== Smart Locker v1.2 MVP Serial Commands ==="));
            Serial.println(F("  STATUS                 - Show locker, Wi-Fi & MQTT state"));
            Serial.println(F("  LOCK <1..4|LOCKER_001> - Force lock compartment"));
            Serial.println(F("  UNLOCK <1..4>          - Force unlock compartment"));
            Serial.println(F("  RELEASE <1..4>         - Clear assignment & unlock"));
            Serial.println(F("  RESET_ALL              - Clear all assignments & lock all"));
            Serial.println(F("  WIFI <SSID> <PASSWORD> - Save new Wi-Fi in NVS & reconnect"));
            Serial.println(F("  WIFI_RESET             - Revert Wi-Fi to Config.h default"));
        } else if (upper == "STATUS") {
            Serial.printf("WiFi:%s (%s) | MQTT:%s | Available:%u/%u\n",
                          gNetwork.wifiConnected() ? "UP" : "DOWN",
                          gNetwork.currentSsid().c_str(),
                          gNetwork.mqttConnected() ? "UP" : "DOWN",
                          gLockers.availableCount(), Cfg::LOCKER_COUNT);
            for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
                const LockerRecord& r = gLockers.record(i);
                Serial.printf("  %s: assigned=%u locked=%u door=%s uid=%s pin=%s\n",
                              Cfg::LOCKER_IDS[i], r.assigned,
                              gLockers.isLocked(i),
                              gHardware.doorClosed(i) ? "CLOSED" : "OPEN",
                              r.uid[0] ? r.uid : "-",
                              r.pin[0] ? r.pin : "-");
            }
        } else if (upper.startsWith("LOCK ") || upper.startsWith("UNLOCK ") || upper.startsWith("RELEASE ")) {
            const bool isLock = upper.startsWith("LOCK ");
            const bool isRelease = upper.startsWith("RELEASE ");
            const String arg = serialBuffer.substring(isRelease ? 8 : (isLock ? 5 : 7));
            const int idx = parseLockerArg(arg);
            if (idx >= 0) {
                if (isRelease) gLockers.release(idx, "SERIAL");
                else gLockers.remoteSetLock(idx, isLock, "SERIAL");
            } else {
                Serial.println(F("Invalid locker ID (use 1..4 or LOCKER_001..004)"));
            }
        } else if (upper == "RESET_ALL") {
            gLockers.resetAllLockers();
            Serial.println(F("All lockers cleared and locked."));
        } else if (upper.startsWith("WIFI ")) {
            String rest = serialBuffer.substring(5);
            rest.trim();
            int sep = rest.indexOf('|');
            if (sep < 0) sep = rest.indexOf(' ');
            if (sep > 0) {
                String newSsid = rest.substring(0, sep);
                String newPass = rest.substring(sep + 1);
                newSsid.trim();
                newPass.trim();
                gStorage.saveWifiOverride(newSsid, newPass);
                gNetwork.setWifiCredentials(newSsid, newPass);
                Serial.printf("Wi-Fi updated -> %s\n", newSsid.c_str());
            } else {
                Serial.println(F("Usage: WIFI <SSID> <PASSWORD>"));
            }
        } else if (upper == "WIFI_RESET") {
            gStorage.clearWifiOverride();
            gNetwork.setWifiCredentials(Cfg::WIFI_SSID, Cfg::WIFI_PASSWORD);
            Serial.println(F("Wi-Fi reverted to Config.h default."));
        } else {
            Serial.println(F("Unknown command. Type HELP."));
        }

        serialBuffer = "";
    }
}

// ============================================================
// SETUP & LOOP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("Smart Locker | ESP32-S3 N16R8 | v1.2 MVP | Reset reason: %d\n",
                  static_cast<int>(esp_reset_reason()));

    gHardware.begin();

    if (!gStorage.begin()) {
        Serial.println(F("FATAL: NVS initialization failed."));
        while (true) {
            gHardware.beep(250);
            delay(1000);
        }
    }

    gLockers.begin(gStorage, gHardware, gNetwork);
    gNetwork.setCommandHandler(handleRemoteCommand);

    String savedSsid, savedPass;
    if (gStorage.loadWifiOverride(savedSsid, savedPass)) {
        Serial.printf("Using NVS Wi-Fi override: %s\n", savedSsid.c_str());
        gNetwork.begin(savedSsid.c_str(), savedPass.c_str());
    } else {
        gNetwork.begin();
    }

    gUi.begin(gHardware, gLockers, gNetwork);
    Serial.println(F("Ready (Offline + Plain MQTT 1883). Type HELP for Serial commands."));
}

void loop() {
    gNetwork.tick();

    static bool ledInit = false;
    static bool lastLed = false;
    const bool online = gNetwork.online();
    if (!ledInit || online != lastLed) {
        gHardware.setLed(online);
        lastLed = online;
        ledInit = true;
    }

    gUi.tick();
    gLockers.tick();
    handleSerial();

    static uint32_t lastStateBroadcast = 0;
    const uint32_t now = millis();
    if (gNetwork.mqttConnected() && now - lastStateBroadcast >= 30000) {
        lastStateBroadcast = now;
        gLockers.broadcastAllStates("HEARTBEAT");
    }
}