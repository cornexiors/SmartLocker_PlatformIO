#include "App.h"
#include <cstdarg>
#include <cstring>

Storage gStorage;
ClockService gClock;
Hardware gHardware;
NetworkService gNetwork;
LockerManager gLockers;
UserInterface gUi;

QueueHandle_t gAuthReqQueue = nullptr;
QueueHandle_t gAuthResQueue = nullptr;

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
    gStorage.dumpLogs(Serial);
}

// ============================================================
// CORE 0: REMOTE COMMAND & PEER STATE SYNC HANDLERS
// ============================================================

static void handleRemoteCommand(const String& command) {
    String fields[6];
    String reason;
    const bool valid = Security::verifySignedPacket(
        command, 6, fields, reason,
        Cfg::ENABLE_COMMAND_TIMESTAMP_CHECK, gClock.nowUtc()
    );

    String lockerId = fields[0];
    lockerId.toUpperCase();
    String action = fields[1];
    action.toUpperCase();
    const String& commandId = fields[4];

    const int index = gLockers.findLockerId(lockerId);
    if (index >= 0) {
        const int cached = Security::findCachedCommand(lockerId, commandId);
        if (cached >= 0) {
            gNetwork.publishAck(index, Security::getCachedCommandAck(cached));
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
        if (valid) Security::cacheCommand(lockerId, commandId, ack);
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
    const uint64_t nowUtc = gClock.nowUtc();

    if (op == "UPSERT") {
        if (!Security::verifySignedPacket(message, 8, fields, reason,
                                          Cfg::ENABLE_COMMAND_TIMESTAMP_CHECK, nowUtc)) {
            appLog("WARN", "UserSync UPSERT rejected: %s", reason.c_str());
            return;
        }
        const bool ok = gLockers.upsertUser(fields[1], fields[2], fields[3], fields[4]);
        appLog("SYNC", "UPSERT %s: %s (total=%u)",
               fields[1].c_str(), ok ? "OK" : "FULL", gLockers.registeredUserCount());
    } else if (op == "ASSIGN") {
        if (!Security::verifySignedPacket(message, 7, fields, reason,
                                          Cfg::ENABLE_COMMAND_TIMESTAMP_CHECK, nowUtc)) {
            appLog("WARN", "UserSync ASSIGN rejected: %s", reason.c_str());
            return;
        }
        const bool open = fields[3] == "OPEN";
        const bool ok = (open || fields[3] == "PRESERVE") &&
            gLockers.assignFromBackend(fields[1], fields[2], open);
        appLog("SYNC", "ASSIGN %s to %s: %s",
               fields[2].c_str(), fields[1].c_str(), ok ? "OK" : "REJECTED");
    } else if (op == "UNASSIGN") {
        if (!Security::verifySignedPacket(message, 6, fields, reason,
                                          Cfg::ENABLE_COMMAND_TIMESTAMP_CHECK, nowUtc)) {
            appLog("WARN", "UserSync UNASSIGN rejected: %s", reason.c_str());
            return;
        }
        const bool ok = gLockers.unassignFromBackend(fields[1], fields[2]);
        appLog("SYNC", "UNASSIGN %s from %s: %s",
               fields[2].c_str(), fields[1].c_str(), ok ? "OK" : "REJECTED");
    } else if (op == "UNASSIGN_ALL") {
        if (!Security::verifySignedPacket(message, 4, fields, reason,
                                          Cfg::ENABLE_COMMAND_TIMESTAMP_CHECK, nowUtc)) {
            appLog("WARN", "UserSync UNASSIGN_ALL rejected: %s", reason.c_str());
            return;
        }
        gLockers.clearAssignments();
        appLog("SYNC", "UNASSIGN_ALL: OK");
    } else if (op == "RFID_OTP") {
        if (!Security::verifySignedPacket(message, 6, fields, reason,
                                          Cfg::ENABLE_COMMAND_TIMESTAMP_CHECK, nowUtc)) {
            appLog("WARN", "UserSync RFID_OTP rejected: %s", reason.c_str());
            return;
        }
        const bool ok = gLockers.registerPendingOtp(fields[1], fields[2]);
        appLog("SYNC", "RFID_OTP for %s: %s", fields[1].c_str(), ok ? "QUEUED" : "INVALID");
    } else if (op == "DELETE") {
        if (!Security::verifySignedPacket(message, 5, fields, reason,
                                          Cfg::ENABLE_COMMAND_TIMESTAMP_CHECK, nowUtc)) {
            appLog("WARN", "UserSync DELETE rejected: %s", reason.c_str());
            return;
        }
        const bool ok = gLockers.deleteUser(fields[1]);
        appLog("SYNC", "DELETE %s: %s", fields[1].c_str(), ok ? "OK" : "NOT_FOUND");
    } else if (op == "CLEAR") {
        if (!Security::verifySignedPacket(message, 4, fields, reason,
                                          Cfg::ENABLE_COMMAND_TIMESTAMP_CHECK, nowUtc)) {
            appLog("WARN", "UserSync CLEAR rejected: %s", reason.c_str());
            return;
        }
        gLockers.clearUsers();
        appLog("SYNC", "CLEAR users: OK");
    } else {
        appLog("WARN", "UserSync unknown op: %s", op.c_str());
    }
}

static void routeIncomingMqtt(const MqttInMsg& msg) {
    char syncTopic[64];
    snprintf(syncTopic, sizeof(syncTopic), "%susers/sync", Cfg::MQTT_BASE);

    const String payload(msg.payload);
    if (strcmp(msg.topic, syncTopic) == 0) {
        handleUserSync(payload);
        return;
    }

    const int sep = payload.indexOf('|');
    if (sep <= 0) return;

    String lockerId = payload.substring(0, sep);
    lockerId.trim();
    lockerId.toUpperCase();

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        char expected[64];
        snprintf(expected, sizeof(expected), "%s%s/command", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[i]);
        if (lockerId == Cfg::LOCKER_IDS[i] && strcmp(msg.topic, expected) == 0) {
            handleRemoteCommand(payload);
            return;
        }
    }
}

// ============================================================
// CORE 0 FREERTOS TASK: NETWORK + SNTP + PBKDF2 CRYPTO WORKER
// ============================================================

static void netCryptoTask(void* pvParameters) {
    uint32_t lastSyncAttempt = 0;
    uint32_t lastStateBroadcast = 0;

    for (;;) {
        gNetwork.tick();
        gClock.tick(gNetwork.wifiConnected());

        // Process incoming TLS MQTT messages on Core 0
        MqttInMsg inMsg;
        while (gNetwork.dequeueIncomingMqtt(inMsg)) {
            routeIncomingMqtt(inMsg);
        }

        // Execute heavy 10,000-iteration PBKDF2-HMAC-SHA256 requests off Core 1
        AuthRequest req;
        while (xQueueReceive(gAuthReqQueue, &req, 0) == pdTRUE) {
            AuthResult res{};
            gLockers.processAuthRequestOnCore0(req, res);
            xQueueSend(gAuthResQueue, &res, portMAX_DELAY);
        }

        const uint32_t now = millis();
        if (gNetwork.mqttConnected() && (now - lastSyncAttempt >= 250)) {
            lastSyncAttempt = now;
            String pending;
            if (gStorage.peek(pending) && gNetwork.publishEvent(pending)) {
                gStorage.pop();
            }
        }

        if (gNetwork.mqttConnected() && gNetwork.userSyncPending() &&
            gStorage.queueCount() == 0) {
            gNetwork.publishUserSyncRequest(gLockers.registeredUserCount());
        }

        if (gNetwork.mqttConnected() && (now - lastStateBroadcast >= 30000)) {
            lastStateBroadcast = now;
            gLockers.broadcastAllStates("HEARTBEAT");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================
// SERIAL CONSOLE CLI WITH ADMIN AUTHENTICATION
// ============================================================

static String serialBuffer;
static bool serialAdminAuthed = false;
static uint32_t serialAdminLastActivity = 0;

static bool checkSerialAdmin() {
    if (!serialAdminAuthed) {
        Serial.println(F("DENIED: Run ADMIN_LOGIN <username> <password> first."));
        return false;
    }
    if (millis() - serialAdminLastActivity >= Cfg::SERIAL_ADMIN_TIMEOUT_MS) {
        serialAdminAuthed = false;
        Serial.println(F("SESSION EXPIRED: Run ADMIN_LOGIN <username> <password> again."));
        return false;
    }
    serialAdminLastActivity = millis();
    return true;
}

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
    Serial.println(F("=== Smart Locker v2.1 Serial Console ==="));
    Serial.println(F("  HELP                                - Show this command list"));
    Serial.println(F("  STATUS                              - Live peripheral probe, buzzer test & system status"));
    Serial.println(F("  TIME                                - Show current WIB internal-RTC time"));
    Serial.println(F("  ADMIN_LOGIN <USER> <PASS>           - Authenticate Serial Admin session (10m)"));
    Serial.println(F("  ADMIN_LOGOUT                        - Lock Serial Admin session"));
    Serial.println(F("--- Admin Commands (require ADMIN_LOGIN) ---"));
    Serial.println(F("  LOCK <1..4|LOCKER_001>              - Lock compartment (165 deg)"));
    Serial.println(F("  UNLOCK <1..4|LOCKER_001>            - Unlock compartment (15 deg)"));
    Serial.println(F("  SERVO <1..4> <0..180>               - Direct servo angle test"));
    Serial.println(F("  RELEASE <1..4|LOCKER_001>           - Force release & unassign locker"));
    Serial.println(F("  OVERWRITE_CLEAR <1..4|ALL>          - Clear overdue flags & overwrite state"));
    Serial.println(F("  SYNC_PUSH                           - Flush NVS queue & push all states to MQTT"));
    Serial.println(F("  SYNC_PULL                           - Request user/assignment snapshot from backend"));
    Serial.println(F("  FORCE_ACK <1..4> [MSG]              - Publish manual ACK & state"));
    Serial.println(F("  ADD_USER_PIN <UID> <RFID|-> <PIN>   - Add/update local user with PBKDF2 hash"));
    Serial.println(F("  SET_USER_RFID <USER_ID> <RFID_UID>  - Bind RFID card & sync RFID_REG_ACK"));
    Serial.println(F("  DEL_USER <USER_ID>                  - Remove user from NVS"));
    Serial.println(F("  CLEAR_USERS                         - Wipe all synced users in NVS"));
    Serial.println(F("  LIST_USERS                          - List all synced users"));
    Serial.println(F("  RESET_ADMIN                         - Re-arm D-menu first-boot admin setup"));
    Serial.println(F("  WIFI <SSID> <PASSWORD>              - Save Wi-Fi override in NVS & reconnect"));
    Serial.println(F("  WIFI_RESET                          - Revert Wi-Fi to Secrets.h default"));
    Serial.println(F("  NTP_SYNC                            - Trigger immediate SNTP sync"));
    Serial.println(F("  LED_TEST                            - Cycle onboard RGB LED & test GPIO 14"));
    Serial.println(F("  LOGS / CLEAR_LOGS                   - Dump or clear RAM, LittleFS & SD logs"));
    Serial.println(F("  FACTORY_RESET                       - Wipe all NVS namespaces & reboot"));
}

static void printFullStatus() {
    char timeStr[32];
    gClock.formatLocal(timeStr, sizeof(timeStr));

    // Physically chirp buzzer as part of live peripheral check
    gHardware.beep(70);

    Serial.println(F("=== SYSTEM & PERIPHERAL STATUS ==="));
    Serial.printf("Firmware: v%s | Location: %s | Device: %s\n",
                  Cfg::FIRMWARE_VERSION, Cfg::LOCATION_ID, Cfg::DEVICE_ID);
    Serial.printf("Reset Reason: %d | Free Heap: %u bytes | Time: %s\n",
                  static_cast<int>(esp_reset_reason()), ESP.getFreeHeap(), timeStr);
    Serial.printf("Network -> WiFi:%s (%s, %d dBm) | TLS MQTT:%s | Queue:%u/%u\n",
                  gNetwork.wifiConnected() ? "UP" : "DOWN",
                  gNetwork.currentSsid().c_str(),
                  gNetwork.wifiConnected() ? WiFi.RSSI() : 0,
                  gNetwork.mqttConnected() ? "UP" : "DOWN",
                  gStorage.queueCount(), Cfg::SYNC_QUEUE_CAPACITY);
    Serial.printf("Peripherals -> TFT(ST7735):%s | PCF8575(0x%02X):%s | RFID(ver=0x%02X):%s | Buzzer:CHIRPED | LittleFS:%s | SD:%s\n",
                  gHardware.displayReady() ? "OK" : "FAIL",
                  Cfg::PCF8575_ADDRESS,
                  gHardware.keypadReady() ? "OK" : "FAIL",
                  gHardware.rfidVersionReg(),
                  gHardware.rfidReady() ? "OK" : "FAIL",
                  gStorage.fsReady() ? "OK" : "FAIL",
                  gStorage.sdReady() ? "OK" : "ABSENT");
    Serial.printf("Lockers -> Free:%u/%u | Overdue:%u | SyncedUsers:%u | AdminProvisioned:%s\n",
                  gLockers.availableCount(), Cfg::LOCKER_COUNT,
                  gLockers.problematicCount(),
                  gLockers.registeredUserCount(),
                  gLockers.isAdminProvisioned() ? "YES" : "NO");

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        const LockerRecord& r = gLockers.record(i);
        Serial.printf("  %s (Servo GPIO%u, Reed P1%u): assigned=%u overdue=%u state=%s door=%s user=%s\n",
                      Cfg::LOCKER_IDS[i],
                      Cfg::SERVO_PINS[i],
                      i,
                      isAssigned(r),
                      isExpiredHold(r),
                      gLockers.isLocked(i) ? "LOCKED" : "UNLOCKED",
                      gHardware.doorClosed(i) ? "CLOSED" : "OPEN",
                      r.userId[0] ? r.userId : "-");
    }
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

        // Raw signed protocol packets
        if (serialBuffer.startsWith("UPSERT|") || serialBuffer.startsWith("ASSIGN|") ||
            serialBuffer.startsWith("UNASSIGN|") || serialBuffer.startsWith("UNASSIGN_ALL|") ||
            serialBuffer.startsWith("RFID_OTP|") || serialBuffer.startsWith("DELETE|") ||
            serialBuffer.startsWith("CLEAR|")) {
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
        } else if (upper == "STATUS" || upper == "DIAG") {
            printFullStatus();
        } else if (upper == "TIME") {
            char t[32];
            gClock.formatLocal(t, sizeof(t));
            Serial.println(t);
        } else if (upper.startsWith("ADMIN_LOGIN ")) {
            String rest = serialBuffer.substring(12);
            rest.trim();
            const int sp = rest.indexOf(' ');
            if (sp > 0) {
                const String u = rest.substring(0, sp);
                const String p = rest.substring(sp + 1);
                if (Security::verifySerialAdmin(u, p)) {
                    serialAdminAuthed = true;
                    serialAdminLastActivity = millis();
                    gHardware.beep(80);
                    Serial.println(F("ADMIN_LOGIN: OK (Broad-scope admin unlocked for 10m)"));
                } else {
                    gHardware.beep(200);
                    Serial.println(F("ADMIN_LOGIN: INVALID CREDENTIALS"));
                }
            } else {
                Serial.println(F("Usage: ADMIN_LOGIN <username> <password>"));
            }
        } else if (upper == "ADMIN_LOGOUT") {
            serialAdminAuthed = false;
            Serial.println(F("ADMIN_LOGOUT: Session locked."));
        } else if (upper.startsWith("LOCK ") || upper.startsWith("UNLOCK ") || upper.startsWith("RELEASE ")) {
            if (checkSerialAdmin()) {
                const bool isLock = upper.startsWith("LOCK ");
                const bool isRelease = upper.startsWith("RELEASE ");
                const String arg = serialBuffer.substring(isRelease ? 8 : (isLock ? 5 : 7));
                const int idx = parseLockerArg(arg);
                if (idx >= 0) {
                    if (isRelease) gLockers.release(idx, "SERIAL_ADMIN");
                    else gLockers.remoteSetLock(idx, isLock, "SERIAL_ADMIN");
                } else {
                    Serial.println(F("Invalid locker ID (use 1..4 or LOCKER_001..004)"));
                }
            }
        } else if (upper.startsWith("SERVO ")) {
            if (checkSerialAdmin()) {
                String rest = serialBuffer.substring(6);
                rest.trim();
                const int sp = rest.indexOf(' ');
                if (sp > 0) {
                    const int idx = parseLockerArg(rest.substring(0, sp));
                    const int angle = rest.substring(sp + 1).toInt();
                    if (idx >= 0 && gHardware.setServoAngleDirect(idx, angle)) {
                        appLog("SERIAL", "SERVO %s -> %d deg", Cfg::LOCKER_IDS[idx], angle);
                    }
                }
            }
        } else if (upper.startsWith("OVERWRITE_CLEAR ")) {
            if (checkSerialAdmin()) {
                String arg = serialBuffer.substring(16);
                arg.trim();
                if (arg.equalsIgnoreCase("ALL")) {
                    gLockers.overwriteClearExpired(-1, "SERIAL_OVERWRITE");
                    appLog("SERIAL", "Cleared all overdue holds & pushed states");
                } else {
                    const int idx = parseLockerArg(arg);
                    if (idx >= 0) {
                        gLockers.overwriteClearExpired(idx, "SERIAL_OVERWRITE");
                        appLog("SERIAL", "Cleared overdue hold on %s", Cfg::LOCKER_IDS[idx]);
                    }
                }
            }
        } else if (upper == "SYNC_PUSH" || upper == "PUSH_STATES" || upper == "FLUSH_QUEUE") {
            if (checkSerialAdmin()) {
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
                gLockers.broadcastAllStates("SERIAL_SYNC_PUSH");
                appLog("SERIAL", "SYNC_PUSH drained %u events & broadcast all states", flushed);
            }
        } else if (upper == "SYNC_PULL") {
            if (checkSerialAdmin()) {
                gNetwork.requestUserSync();
                appLog("SERIAL", "SYNC_PULL queued");
            }
        } else if (upper.startsWith("FORCE_ACK ")) {
            if (checkSerialAdmin()) {
                String rest = serialBuffer.substring(10);
                rest.trim();
                const int sp = rest.indexOf(' ');
                const String target = (sp > 0) ? rest.substring(0, sp) : rest;
                const String note = (sp > 0) ? rest.substring(sp + 1) : "MANUAL_SYNC";
                const int idx = parseLockerArg(target);
                if (idx >= 0) {
                    const String ack = String("ACCEPTED|") + Cfg::LOCKER_IDS[idx] + "|SERIAL|" + note;
                    gNetwork.publishAck(idx, ack);
                    gLockers.broadcastAllStates("SERIAL_ACK");
                    appLog("SERIAL", "FORCE_ACK %s", ack.c_str());
                }
            }
        } else if (upper.startsWith("ADD_USER_PIN ")) {
            if (checkSerialAdmin()) {
                String rest = serialBuffer.substring(13);
                rest.trim();
                const int s1 = rest.indexOf(' ');
                const int s2 = (s1 > 0) ? rest.indexOf(' ', s1 + 1) : -1;
                if (s1 > 0 && s2 > s1) {
                    String uidArg = rest.substring(0, s1);
                    String rfidArg = rest.substring(s1 + 1, s2);
                    String pinArg = rest.substring(s2 + 1);
                    uidArg.trim(); rfidArg.trim(); pinArg.trim();
                    if (rfidArg == "-" || rfidArg.equalsIgnoreCase("NONE")) rfidArg = "";

                    const String salt = Security::generateSaltHex();
                    const String hash = Security::makePinHash(salt.c_str(), pinArg);
                    const bool ok = !hash.isEmpty() && gLockers.upsertUser(uidArg, rfidArg, salt, hash);
                    appLog("SERIAL", "ADD_USER_PIN %s -> %s", uidArg.c_str(), ok ? "OK" : "FAILED");
                }
            }
        } else if (upper.startsWith("SET_USER_RFID ")) {
            if (checkSerialAdmin()) {
                String rest = serialBuffer.substring(14);
                rest.trim();
                const int sp = rest.indexOf(' ');
                if (sp > 0) {
                    const String uId = rest.substring(0, sp);
                    const String rfid = rest.substring(sp + 1);
                    const ActionStatus st = gLockers.adminSetUserRfid(uId, rfid);
                    appLog("SERIAL", "SET_USER_RFID %s -> %d", uId.c_str(), static_cast<int>(st));
                }
            }
        } else if (upper.startsWith("DEL_USER ")) {
            if (checkSerialAdmin()) {
                String uidArg = serialBuffer.substring(9);
                uidArg.trim();
                const bool ok = gLockers.deleteUser(uidArg);
                appLog("SERIAL", "DEL_USER %s -> %s", uidArg.c_str(), ok ? "OK" : "NOT_FOUND");
            }
        } else if (upper == "CLEAR_USERS") {
            if (checkSerialAdmin()) {
                gLockers.clearUsers();
                appLog("SERIAL", "All synced users cleared");
            }
        } else if (upper == "LIST_USERS") {
            if (checkSerialAdmin()) {
                gLockers.listUsersToSerial(Serial);
            }
        } else if (upper == "RESET_ADMIN") {
            if (checkSerialAdmin()) {
                gLockers.resetAdminProvisioning();
                appLog("SERIAL", "D-menu admin credentials cleared; next D-press runs setup");
            }
        } else if (upper.startsWith("WIFI ")) {
            if (checkSerialAdmin()) {
                String rest = serialBuffer.substring(5);
                rest.trim();
                int sep = rest.indexOf('|');
                if (sep < 0) sep = rest.indexOf(' ');
                if (sep > 0) {
                    String newSsid = rest.substring(0, sep);
                    String newPass = rest.substring(sep + 1);
                    newSsid.trim(); newPass.trim();
                    gStorage.saveWifiOverride(newSsid, newPass);
                    gNetwork.setWifiCredentials(newSsid, newPass);
                    appLog("SERIAL", "Wi-Fi override saved -> %s", newSsid.c_str());
                }
            }
        } else if (upper == "WIFI_RESET") {
            if (checkSerialAdmin()) {
                gStorage.clearWifiOverride();
                gNetwork.setWifiCredentials(Cfg::WIFI_SSID, Cfg::WIFI_PASSWORD);
                appLog("SERIAL", "Wi-Fi reverted to Secrets.h default");
            }
        } else if (upper == "NTP_SYNC") {
            if (checkSerialAdmin()) {
                gClock.forceNtpSync();
                appLog("SERIAL", "SNTP resync requested");
            }
        } else if (upper == "LED_TEST") {
            if (checkSerialAdmin()) {
                gHardware.setOnlineLed(true);
                gHardware.testRgbCycle();
                Serial.println(F("LED_TEST complete."));
            }
        } else if (upper == "LOGS") {
            if (checkSerialAdmin()) printDebugLogs();
        } else if (upper == "CLEAR_LOGS") {
            if (checkSerialAdmin()) {
                debugLogHead = 0;
                debugLogCount = 0;
                gStorage.clearLogs();
                Serial.println(F("RAM, LittleFS & SD logs cleared."));
            }
        } else if (upper == "FACTORY_RESET") {
            if (checkSerialAdmin()) {
                Serial.println(F("FACTORY_RESET: Wiping NVS & restarting..."));
                gStorage.factoryResetAll();
                delay(300);
                ESP.restart();
            }
        } else {
            Serial.println(F("Unknown command. Type HELP for available commands."));
        }

        serialBuffer = "";
    }
}

// ============================================================
// SETUP & CORE 1 REAL-TIME LOOP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(600);
    appLog("INFO", "Smart Locker v%s | ESP32-S3 N16R8 | Reset reason: %d",
           Cfg::FIRMWARE_VERSION, static_cast<int>(esp_reset_reason()));

    gAuthReqQueue = xQueueCreate(4, sizeof(AuthRequest));
    gAuthResQueue = xQueueCreate(4, sizeof(AuthResult));

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

    String savedSsid, savedPass;
    if (gStorage.loadWifiOverride(savedSsid, savedPass)) {
        appLog("INFO", "Using NVS Wi-Fi override");
        gNetwork.begin(savedSsid.c_str(), savedPass.c_str());
    } else {
        gNetwork.begin();
    }

    gUi.begin(gHardware, gLockers, gClock, gNetwork);

    // Pin Network, SNTP & Heavy PBKDF2 Crypto Task to Core 0
    xTaskCreatePinnedToCore(
        netCryptoTask,
        "NetCryptoTask",
        Cfg::CORE0_TASK_STACK,
        nullptr,
        Cfg::CORE0_TASK_PRIORITY,
        nullptr,
        0
    );

    appLog("INFO", "Ready | Users:%u | SD:%s | Type HELP for Serial commands",
           gLockers.registeredUserCount(),
           gStorage.sdReady() ? "ready" : "absent");
}

void loop() {
    const uint32_t tickStart = millis();

    // 1. Poll Core 0 PBKDF2 Auth Results
    AuthResult authRes;
    while (xQueueReceive(gAuthResQueue, &authRes, 0) == pdTRUE) {
        gUi.onAsyncAuthResult(authRes);
    }

    // 2. Update Discrete LED (GPIO 14) & Onboard RGB LED (GPIO 48)
    const bool wifiOk = gNetwork.wifiConnected();
    const bool mqttOk = gNetwork.mqttConnected();
    gHardware.setOnlineLed(mqttOk);

    if (gUi.isAdminActive() || serialAdminAuthed) {
        gHardware.setRgbState(RgbState::ADMIN_ACTIVE);
    } else if (gLockers.problematicCount() > 0) {
        gHardware.setRgbState(RgbState::ALERT_ERROR);
    } else if (gUi.isSessionBusy()) {
        gHardware.setRgbState(RgbState::ACTIVE_SESSION);
    } else if (mqttOk) {
        gHardware.setRgbState(RgbState::ONLINE_IDLE);
    } else if (wifiOk) {
        gHardware.setRgbState(RgbState::MQTT_CONNECTING);
    } else {
        gHardware.setRgbState(RgbState::OFFLINE_LOCAL);
    }

    // 3. Real-Time Keypad, RFID, TFT UI, Reed Auto-Lock & Serial CLI on Core 1
    gUi.tick();
    gLockers.tick();
    handleSerial();

    const uint32_t elapsed = millis() - tickStart;
    if (elapsed < Cfg::CORE1_LOOP_TICK_MS) {
        delay(Cfg::CORE1_LOOP_TICK_MS - elapsed);
    }
}
