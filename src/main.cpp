#include "App.h"
#include <mbedtls/md.h>
#include <cstdarg>
#include <cstring>

Storage gStorage;
ClockService gClock;
Hardware gHardware;
NetworkService gNetwork;
LockerManager gLockers;
UserInterface gUi;

// ============================================================
// IN-MEMORY RING-BUFFER DEBUG LOGGER
// ============================================================

static String debugLogs[Cfg::DEBUG_LOG_CAPACITY];
static uint8_t debugLogHead = 0;
static uint8_t debugLogCount = 0;

void appLog(const char* level, const char* fmt, ...) {
    char msg[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    char line[224];
    snprintf(line, sizeof(line), "[%lu ms][%s] %s",
             static_cast<unsigned long>(millis()), level, msg);

    Serial.println(line);

    debugLogs[debugLogHead] = String(line);
    debugLogHead = static_cast<uint8_t>((debugLogHead + 1) % Cfg::DEBUG_LOG_CAPACITY);
    if (debugLogCount < Cfg::DEBUG_LOG_CAPACITY) ++debugLogCount;
}

static void printDebugLogs() {
    Serial.printf("--- RAM DEBUG LOG (%u entries) ---\n", debugLogCount);
    const uint8_t start = (debugLogHead + Cfg::DEBUG_LOG_CAPACITY - debugLogCount) % Cfg::DEBUG_LOG_CAPACITY;
    for (uint8_t i = 0; i < debugLogCount; ++i) {
        const uint8_t idx = (start + i) % Cfg::DEBUG_LOG_CAPACITY;
        Serial.println(debugLogs[idx]);
    }
    Serial.println(F("--- END RAM LOG ---"));
    gStorage.dumpSdLog(Serial);
}

// ============================================================
// CRYPTO & REPLAY PROTECTION
// ============================================================

static String nonceHistory[24];
static uint8_t nonceIndex = 0;

struct ProcessedCommand {
    String lockerId;
    String commandId;
    String ack;
};
static ProcessedCommand commandCache[32];
static uint8_t commandCacheIndex = 0;

static uint32_t lastSyncAttempt = 0;
static String serialBuffer;

static int findCachedCommand(const String& lockerId, const String& commandId) {
    if (commandId.isEmpty()) return -1;
    for (uint8_t i = 0; i < 32; ++i) {
        if (commandCache[i].lockerId == lockerId && commandCache[i].commandId == commandId) {
            return i;
        }
    }
    return -1;
}

static void cacheCommand(const String& lockerId, const String& commandId, const String& ack) {
    if (commandId.isEmpty()) return;
    ProcessedCommand& item = commandCache[commandCacheIndex];
    item.lockerId = lockerId;
    item.commandId = commandId;
    item.ack = ack;
    commandCacheIndex = static_cast<uint8_t>((commandCacheIndex + 1) % 32);
}

static String hmacSha256(const String& message) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return String();
    uint8_t digest[32];
    if (mbedtls_md_hmac(md,
                        reinterpret_cast<const uint8_t*>(Cfg::HMAC_SECRET), strlen(Cfg::HMAC_SECRET),
                        reinterpret_cast<const uint8_t*>(message.c_str()), message.length(),
                        digest) != 0) {
        return String();
    }
    static const char hex[] = "0123456789abcdef";
    char out[65];
    for (uint8_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[64] = '\0';
    return String(out);
}

static bool secureEqual(const String& a, const String& b) {
    if (a.length() != 64 || b.length() != 64) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.length(); ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool verifySignedPacket(const String& packet, uint8_t expectedFields,
                        String* outFields, String& errorReason) {
    if (expectedFields < 4 || expectedFields > 10) {
        errorReason = "INTERNAL_ERR";
        return false;
    }

    int from = 0;
    for (uint8_t i = 0; i < expectedFields - 1; ++i) {
        const int pos = packet.indexOf('|', from);
        if (pos < 0) {
            errorReason = "MALFORMED";
            return false;
        }
        outFields[i] = packet.substring(from, pos);
        outFields[i].trim();
        from = pos + 1;
    }

    if (packet.indexOf('|', from) >= 0) {
        errorReason = "MALFORMED";
        return false;
    }
    outFields[expectedFields - 1] = packet.substring(from);
    outFields[expectedFields - 1].trim();

    String canonical = outFields[0];
    for (uint8_t i = 1; i < expectedFields - 1; ++i) {
        canonical += '|';
        canonical += outFields[i];
    }

    const String& signature = outFields[expectedFields - 1];
    if (!secureEqual(hmacSha256(canonical), signature)) {
        errorReason = "INVALID_SIGNATURE";
        return false;
    }

    const String& tsField = outFields[expectedFields - 3];
    const String& nonceField = outFields[expectedFields - 2];
    if (nonceField.isEmpty()) {
        errorReason = "MISSING_NONCE";
        return false;
    }

    if (Cfg::ENABLE_COMMAND_TIMESTAMP_CHECK) {
        char* end = nullptr;
        const long long ts = strtoll(tsField.c_str(), &end, 10);
        if (!end || *end || !gClock.valid()) {
            errorReason = "INVALID_TIMESTAMP";
            return false;
        }
        const long long delta = static_cast<long long>(gClock.nowUtc()) - ts;
        if (llabs(delta) > Cfg::COMMAND_WINDOW_SECONDS) {
            errorReason = "STALE_COMMAND";
            return false;
        }
    }

    for (const String& used : nonceHistory) {
        if (used == nonceField) {
            errorReason = "REPLAY";
            return false;
        }
    }
    nonceHistory[nonceIndex] = nonceField;
    nonceIndex = (nonceIndex + 1) % 24;
    return true;
}

// ============================================================
// REMOTE COMMAND & USER SYNC HANDLERS
// ============================================================

static void handleRemoteCommand(const String& command) {
    String fields[6];
    String reason;
    const bool valid = verifySignedPacket(command, 6, fields, reason);

    String lockerId = fields[0];
    lockerId.toUpperCase();
    String action = fields[1];
    action.toUpperCase();
    const String& commandId = fields[4];

    const int index = gLockers.findLockerId(lockerId);
    if (index >= 0) {
        const int cached = findCachedCommand(lockerId, commandId);
        if (cached >= 0) {
            gNetwork.publishAck(index, commandCache[cached].ack);
            Serial.println(commandCache[cached].ack);
            return;
        }
    }

    bool accepted = valid;
    String result = valid ? "ACCEPTED" : reason;

    if (accepted && index < 0) {
        accepted = false;
        result = "UNKNOWN_LOCKER";
    }

    if (accepted) {
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
            accepted = false;
            result = "INVALID_ACTION";
        }
    }

    if (index >= 0) {
        const String ack = String(accepted ? "ACCEPTED|" : "REJECTED|") +
                           lockerId + '|' + commandId + '|' + result;
        gNetwork.publishAck(index, ack);
        if (valid) cacheCommand(lockerId, commandId, ack);
        appLog("CMD", "%s", ack.c_str());

        if (accepted) {
            gUi.notifyRemoteCommand(String("REMOTE ") + action, lockerId, false);
        }
    } else {
        appLog("WARN", "Rejected command: %s", result.c_str());
    }
}

static void handleUserSync(const String& message) {
    const int firstPipe = message.indexOf('|');
    if (firstPipe <= 0) return;

    String op = message.substring(0, firstPipe);
    op.trim();
    op.toUpperCase();

    String fields[8];
    String reason;

    if (op == "UPSERT") {
        if (!verifySignedPacket(message, 8, fields, reason)) {
            appLog("WARN", "UserSync UPSERT rejected: %s", reason.c_str());
            return;
        }
        const bool ok = gLockers.upsertUser(fields[1], fields[2], fields[3], fields[4]);
        appLog("SYNC", "UPSERT %s: %s (total=%u)",
               fields[1].c_str(), ok ? "OK" : "FULL", gLockers.registeredUserCount());
    } else if (op == "RFID_OTP") {
        // Format: RFID_OTP|USER_ID|OTP_CODE|TIMESTAMP|NONCE|SIGNATURE
        if (!verifySignedPacket(message, 6, fields, reason)) {
            appLog("WARN", "UserSync RFID_OTP rejected: %s", reason.c_str());
            return;
        }
        const bool ok = gLockers.registerPendingOtp(fields[1], fields[2]);
        appLog("SYNC", "RFID_OTP for %s: %s", fields[1].c_str(), ok ? "QUEUED" : "INVALID");
    } else if (op == "DELETE") {
        if (!verifySignedPacket(message, 5, fields, reason)) {
            appLog("WARN", "UserSync DELETE rejected: %s", reason.c_str());
            return;
        }
        const bool ok = gLockers.deleteUser(fields[1]);
        appLog("SYNC", "DELETE %s: %s", fields[1].c_str(), ok ? "OK" : "NOT_FOUND");
    } else if (op == "CLEAR") {
        if (!verifySignedPacket(message, 4, fields, reason)) {
            appLog("WARN", "UserSync CLEAR rejected: %s", reason.c_str());
            return;
        }
        gLockers.clearUsers();
        appLog("SYNC", "CLEAR users: OK");
    } else {
        appLog("WARN", "UserSync unknown op: %s", op.c_str());
    }
}

// ============================================================
// EXPANDED SERIAL CONSOLE COMMANDS
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

static void printSerialHelp() {
    Serial.println(F("=== Smart Locker Serial Commands ==="));
    Serial.println(F("  STATUS                              - Show locker, user & network summary"));
    Serial.println(F("  DIAG                                - Show peripheral health, heap & RSSI"));
    Serial.println(F("  TIME                                - Show current local WIB time"));
    Serial.println(F("  LOCK <LOCKER_001|1..4>              - Force lock compartment"));
    Serial.println(F("  UNLOCK <LOCKER_001|1..4>            - Force unlock compartment"));
    Serial.println(F("  RELEASE <LOCKER_001|1..4>           - Force release & clear assignment"));
    Serial.println(F("  FORCE_ACK <LOCKER_001|1..4> [MSG]   - Publish manual ACK & state to MQTT"));
    Serial.println(F("  PUSH_STATES                         - Broadcast all 4 locker states to MQTT"));
    Serial.println(F("  SYNC_PULL                           - Request user DB sync from backend"));
    Serial.println(F("  FLUSH_QUEUE                         - Drain queued NVS events to MQTT now"));
    Serial.println(F("  ADD_USER_PIN <UID> <RFID|-> <PIN>   - Add/update local user with plain PIN"));
    Serial.println(F("  DEL_USER <USER_ID>                  - Remove user from local NVS DB"));
    Serial.println(F("  LIST_USERS                          - List all synced users in NVS"));
    Serial.println(F("  WIFI <SSID> <PASSWORD>              - Overwrite Wi-Fi in NVS & reconnect"));
    Serial.println(F("  WIFI_RESET                          - Revert Wi-Fi to Config.h defaults"));
    Serial.println(F("  LOGS                                - Print RAM ring-buffer & SD logs"));
    Serial.println(F("  CLEAR_LOGS                          - Clear RAM & SD logs"));
}

static void handleSerial() {
    while (Serial.available()) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r') continue;
        if (c != '\n') {
            if (serialBuffer.length() < 320) serialBuffer += c;
            continue;
        }

        serialBuffer.trim();
        if (serialBuffer.isEmpty()) continue;

        // Pass raw signed packets directly to protocol handlers
        if (serialBuffer.startsWith("UPSERT|") || serialBuffer.startsWith("RFID_OTP|") ||
            serialBuffer.startsWith("DELETE|") || serialBuffer.startsWith("CLEAR|")) {
            handleUserSync(serialBuffer);
            serialBuffer = "";
            continue;
        }
        if (serialBuffer.indexOf('|') >= 0 && !serialBuffer.startsWith("WIFI ")) {
            handleRemoteCommand(serialBuffer);
            serialBuffer = "";
            continue;
        }

        String upper = serialBuffer;
        upper.toUpperCase();

        if (upper == "HELP") {
            printSerialHelp();
        } else if (upper == "STATUS") {
            Serial.printf("WiFi:%s (%s) | MQTT:%s | Available:%u/%u | Overdue:%u | Users:%u | Queue:%u\n",
                          gNetwork.wifiConnected() ? "UP" : "DOWN",
                          gNetwork.currentSsid().c_str(),
                          gNetwork.mqttConnected() ? "UP" : "DOWN",
                          gLockers.availableCount(), Cfg::LOCKER_COUNT,
                          gLockers.problematicCount(),
                          gLockers.registeredUserCount(),
                          gStorage.queueCount());
            for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
                const LockerRecord& r = gLockers.record(i);
                Serial.printf("  %s: assigned=%u expired=%u locked=%u door=%s user=%s\n",
                              Cfg::LOCKER_IDS[i], isAssigned(r), isExpiredHold(r),
                              gLockers.isLocked(i),
                              gHardware.doorClosed(i) ? "CLOSED" : "OPEN",
                              r.userId[0] ? r.userId : "-");
            }
        } else if (upper == "DIAG") {
            Serial.printf("Heap free: %u bytes | WiFi RSSI: %d dBm | TLS: %s\n",
                          ESP.getFreeHeap(),
                          gNetwork.wifiConnected() ? WiFi.RSSI() : 0,
                          Cfg::MQTT_USE_TLS ? "ON" : "OFF");
            Serial.printf("Peripherals -> Keypad(PCF8575):%s | RFID:%s | TFT:%s | SD:%s | ExtRTC:%s\n",
                          gHardware.keypadReady() ? "OK" : "FAIL",
                          gHardware.rfidReady() ? "OK" : "FAIL",
                          gHardware.displayReady() ? "OK" : "FAIL",
                          gStorage.sdReady() ? "OK" : "ABSENT",
                          gClock.externalRtcReady() ? "OK" : "ABSENT");
        } else if (upper == "TIME") {
            char t[32];
            gClock.formatLocal(t, sizeof(t));
            Serial.println(t);
        } else if (upper.startsWith("LOCK ") || upper.startsWith("UNLOCK ") || upper.startsWith("RELEASE ")) {
            const bool isLock = upper.startsWith("LOCK ");
            const bool isRelease = upper.startsWith("RELEASE ");
            const String arg = serialBuffer.substring(isRelease ? 8 : (isLock ? 5 : 7));
            const int idx = parseLockerArg(arg);
            if (idx >= 0) {
                if (isRelease) gLockers.release(idx, "SERIAL");
                else gLockers.remoteSetLock(idx, isLock, "SERIAL");
            } else {
                Serial.println(F("Invalid locker ID (use LOCKER_001..004 or 1..4)"));
            }
        } else if (upper.startsWith("FORCE_ACK ")) {
            String rest = serialBuffer.substring(10);
            rest.trim();
            const int sp = rest.indexOf(' ');
            const String target = (sp > 0) ? rest.substring(0, sp) : rest;
            const String note = (sp > 0) ? rest.substring(sp + 1) : "MANUAL_SYNC";
            const int idx = parseLockerArg(target);
            if (idx >= 0) {
                const String ack = String("ACCEPTED|") + Cfg::LOCKER_IDS[idx] + "|SERIAL|" + note;
                const bool ok = gNetwork.publishAck(idx, ack);
                gLockers.broadcastAllStates("SERIAL_ACK");
                appLog("SERIAL", "FORCE_ACK %s -> %s", ack.c_str(), ok ? "SENT" : "MQTT_OFFLINE");
            } else {
                Serial.println(F("Invalid locker ID"));
            }
        } else if (upper == "PUSH_STATES") {
            gLockers.broadcastAllStates("SERIAL_PUSH");
            appLog("SERIAL", "Published all locker states to MQTT");
        } else if (upper == "SYNC_PULL") {
            const bool ok = gNetwork.publishUserSyncRequest(gLockers.registeredUserCount());
            appLog("SERIAL", "SYNC_PULL requested -> %s", ok ? "SENT" : "MQTT_OFFLINE");
        } else if (upper == "FLUSH_QUEUE") {
            uint8_t flushed = 0;
            String pending;
            while (gNetwork.mqttConnected() && gStorage.peek(pending)) {
                if (gNetwork.publishEvent(pending)) {
                    gStorage.pop();
                    ++flushed;
                } else {
                    break;
                }
            }
            appLog("SERIAL", "FLUSH_QUEUE drained %u events (remaining=%u)",
                   flushed, gStorage.queueCount());
        } else if (upper.startsWith("ADD_USER_PIN ")) {
            String rest = serialBuffer.substring(13);
            rest.trim();
            const int s1 = rest.indexOf(' ');
            const int s2 = (s1 > 0) ? rest.indexOf(' ', s1 + 1) : -1;
            if (s1 > 0 && s2 > s1) {
                String uidArg = rest.substring(0, s1);
                String rfidArg = rest.substring(s1 + 1, s2);
                String pinArg = rest.substring(s2 + 1);
                uidArg.trim();
                rfidArg.trim();
                pinArg.trim();
                if (rfidArg == "-" || rfidArg.equalsIgnoreCase("NONE")) rfidArg = "";

                char salt[17];
                const uint64_t rnd = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
                snprintf(salt, sizeof(salt), "%016llx", static_cast<unsigned long long>(rnd));
                const String hash = makePinHash(salt, pinArg);

                const bool ok = !hash.isEmpty() && gLockers.upsertUser(uidArg, rfidArg, salt, hash);
                appLog("SERIAL", "ADD_USER_PIN %s -> %s", uidArg.c_str(), ok ? "OK" : "FAILED");
            } else {
                Serial.println(F("Usage: ADD_USER_PIN <USER_ID> <RFID_UID|-> <PIN>"));
            }
        } else if (upper.startsWith("DEL_USER ")) {
            String uidArg = serialBuffer.substring(9);
            uidArg.trim();
            const bool ok = gLockers.deleteUser(uidArg);
            appLog("SERIAL", "DEL_USER %s -> %s", uidArg.c_str(), ok ? "REMOVED" : "NOT_FOUND");
        } else if (upper == "LIST_USERS") {
            gLockers.listUsersToSerial(Serial);
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
                appLog("SERIAL", "Wi-Fi updated in NVS -> SSID: %s", newSsid.c_str());
            } else {
                Serial.println(F("Usage: WIFI <SSID> <PASSWORD> (or WIFI <SSID>|<PASSWORD>)"));
            }
        } else if (upper == "WIFI_RESET") {
            gStorage.clearWifiOverride();
            gNetwork.setWifiCredentials(Cfg::WIFI_SSID, Cfg::WIFI_PASSWORD);
            appLog("SERIAL", "Wi-Fi reverted to Config.h default (%s)", Cfg::WIFI_SSID);
        } else if (upper == "LOGS") {
            printDebugLogs();
        } else if (upper == "CLEAR_LOGS") {
            debugLogHead = 0;
            debugLogCount = 0;
            gStorage.clearSdLog();
            Serial.println(F("RAM and SD logs cleared."));
        } else {
            Serial.println(F("Unknown command. Type HELP for available commands."));
        }

        serialBuffer = "";
    }
}

// ============================================================
// SETUP & LOOP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(600); // Allow Windows USB CDC COM port to re-enumerate after reset
    appLog("INFO", "Smart Locker | ESP32-S3 N16R8 | Reset reason: %d",
           static_cast<int>(esp_reset_reason()));

    gHardware.begin();
    gClock.begin();

    if (!gStorage.begin()) {
        Serial.println(F("FATAL: NVS initialization failed."));
        while (true) {
            gHardware.beep(250);
            delay(1000);
        }
    }

    gLockers.begin(gStorage, gHardware, gClock, gNetwork);
    gNetwork.setCommandHandler(handleRemoteCommand);
    gNetwork.setUserSyncHandler(handleUserSync);

    String savedSsid, savedPass;
    if (gStorage.loadWifiOverride(savedSsid, savedPass)) {
        appLog("INFO", "Using NVS Wi-Fi override: %s", savedSsid.c_str());
        gNetwork.begin(savedSsid.c_str(), savedPass.c_str());
    } else {
        gNetwork.begin();
    }

    gUi.begin(gHardware, gLockers, gClock, gNetwork);
    appLog("INFO", "Ready | Users:%u | SD:%s | Type HELP for Serial commands",
           gLockers.registeredUserCount(),
           gStorage.sdReady() ? "ready" : "absent");
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

    gClock.tick(gNetwork.wifiConnected());
    gUi.tick();
    gLockers.tick();
    handleSerial();

    const uint32_t now = millis();
    if (gNetwork.mqttConnected() && now - lastSyncAttempt >= 250) {
        lastSyncAttempt = now;
        String pending;
        if (gStorage.peek(pending) && gNetwork.publishEvent(pending)) {
            gStorage.pop();
        }
    }

    static uint32_t lastStateBroadcast = 0;
    if (gNetwork.mqttConnected() && now - lastStateBroadcast >= 30000) {
        lastStateBroadcast = now;
        gLockers.broadcastAllStates("HEARTBEAT");
    }
}