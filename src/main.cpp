#include "App.h"
#include <mbedtls/md.h>
#include <cstring>

Storage gStorage;
ClockService gClock;
Hardware gHardware;
NetworkService gNetwork;
LockerManager gLockers;
UserInterface gUi;

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

// Generic signed packet verifier:
// Expected format: F0|F1|...|F(N-2)|SIGNATURE
// where F(N-3) = TIMESTAMP and F(N-2) = NONCE
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
// REMOTE COMMAND HANDLER
// Format: LOCKER_ID|ACTION|TIMESTAMP|NONCE|COMMAND_ID|SIGNATURE
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
        Serial.println(ack);

        // Display successful remote command on TFT for 3 seconds
        if (accepted) {
            gUi.notifyRemoteCommand(String("REMOTE ") + action, lockerId, false);
        }
    } else {
        Serial.printf("Rejected command: %s\n", result.c_str());
    }
}

// ============================================================
// TWO-WAY USER DATABASE SYNC HANDLER
// Formats:
//   UPSERT|USER_ID|RFID_UID|SALT|PIN_HASH|TIMESTAMP|NONCE|SIGNATURE (8 fields)
//   DELETE|USER_ID|TIMESTAMP|NONCE|SIGNATURE (5 fields)
//   CLEAR|TIMESTAMP|NONCE|SIGNATURE (4 fields)
// ============================================================

static void handleUserSync(const String& message) {
    String op = message.substring(0, message.indexOf('|'));
    op.trim();
    op.toUpperCase();

    String fields[8];
    String reason;

    if (op == "UPSERT") {
        if (!verifySignedPacket(message, 8, fields, reason)) {
            Serial.printf("UserSync UPSERT rejected: %s\n", reason.c_str());
            return;
        }
        const bool ok = gLockers.upsertUser(fields[1], fields[2], fields[3], fields[4]);
        Serial.printf("UserSync UPSERT %s: %s (total=%u)\n",
                      fields[1].c_str(), ok ? "OK" : "FULL", gLockers.registeredUserCount());
    } else if (op == "DELETE") {
        if (!verifySignedPacket(message, 5, fields, reason)) {
            Serial.printf("UserSync DELETE rejected: %s\n", reason.c_str());
            return;
        }
        const bool ok = gLockers.deleteUser(fields[1]);
        Serial.printf("UserSync DELETE %s: %s\n", fields[1].c_str(), ok ? "OK" : "NOT_FOUND");
    } else if (op == "CLEAR") {
        if (!verifySignedPacket(message, 4, fields, reason)) {
            Serial.printf("UserSync CLEAR rejected: %s\n", reason.c_str());
            return;
        }
        gLockers.clearUsers();
        Serial.println(F("UserSync CLEAR: OK"));
    } else {
        Serial.printf("UserSync unknown op: %s\n", op.c_str());
    }
}

// ============================================================
// SERIAL CONSOLE COMMANDS (USB CDC)
// ============================================================

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

        if (serialBuffer.startsWith("UPSERT|") || serialBuffer.startsWith("DELETE|") || serialBuffer.startsWith("CLEAR|")) {
            handleUserSync(serialBuffer);
        } else if (serialBuffer.indexOf('|') >= 0) {
            handleRemoteCommand(serialBuffer);
        } else {
            String input = serialBuffer;
            input.toUpperCase();

            if (input == "STATUS") {
                Serial.printf("Available: %u/%u | Overdue(>24h): %u | Registered Users: %u\n",
                              gLockers.availableCount(), Cfg::LOCKER_COUNT,
                              gLockers.problematicCount(), gLockers.registeredUserCount());
                for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
                    const LockerRecord& r = gLockers.record(i);
                    Serial.printf("%s assigned=%u expired=%u locked=%u door=%s user=%s\n",
                                  Cfg::LOCKER_IDS[i], isAssigned(r), isExpiredHold(r),
                                  gLockers.isLocked(i),
                                  gHardware.doorClosed(i) ? "CLOSED" : "OPEN",
                                  r.userId);
                }
            } else if (input == "TIME") {
                char t[32];
                gClock.formatLocal(t, sizeof(t));
                Serial.println(t);
            } else if (input.startsWith("LOCK ") || input.startsWith("UNLOCK ") || input.startsWith("RELEASE ")) {
                const bool isLock = input.startsWith("LOCK ");
                const bool isRelease = input.startsWith("RELEASE ");
                const String id = input.substring(isRelease ? 8 : (isLock ? 5 : 7));
                const int i = gLockers.findLockerId(id);
                if (i >= 0) {
                    if (isRelease) gLockers.release(i, "SERIAL");
                    else gLockers.remoteSetLock(i, isLock, "SERIAL");
                } else {
                    Serial.println(F("Unknown locker ID"));
                }
            } else {
                Serial.println(F("Commands: STATUS, TIME, LOCK LOCKER_001, UNLOCK LOCKER_001, RELEASE LOCKER_001"));
            }
        }
        serialBuffer = "";
    }
}

// ============================================================
// SETUP & LOOP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("Smart Locker | ESP32-S3 N16R8 | Revised Firmware"));

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
    gNetwork.begin();
    gUi.begin(gHardware, gLockers, gClock, gNetwork);

    Serial.printf("Users loaded: %u | SD: %s | Keypad: %s\n",
                  gLockers.registeredUserCount(),
                  gStorage.sdReady() ? "ready" : "optional/absent",
                  gHardware.keypadReady() ? "ready" : "missing");
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

    // Drain offline NVS telemetry queue at max 1 event per 250 ms
    const uint32_t now = millis();
    if (gNetwork.mqttConnected() && now - lastSyncAttempt >= 250) {
        lastSyncAttempt = now;
        String pending;
        if (gStorage.peek(pending) && gNetwork.publishEvent(pending)) {
            gStorage.pop();
        }
    }

    // Periodic full locker state heartbeat every 30 seconds
    static uint32_t lastStateBroadcast = 0;
    if (gNetwork.mqttConnected() && now - lastStateBroadcast >= 30000) {
        lastStateBroadcast = now;
        for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
            const LockerRecord& r = gLockers.record(i);
            gNetwork.publishLockerState(
                i,
                gLockers.isLocked(i),
                isAssigned(r),
                isExpiredHold(r),
                r.userId,
                "HEARTBEAT"
            );
        }
    }
}