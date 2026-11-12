#include "App.h"
#include <mbedtls/md.h>
#include <cstring>

Storage gStorage;
ClockService gClock;
Hardware gHardware;
NetworkService gNetwork;
LockerManager gLockers;
UserInterface gUi;

static String nonceHistory[16];
static uint8_t nonceIndex = 0;
struct ProcessedCommand { String lockerId; String commandId; String ack; };
static ProcessedCommand commandCache[32];
static uint8_t commandCacheIndex = 0;
static uint32_t lastSyncAttempt = 0;
static String serialBuffer;

static int findCachedCommand(const String& lockerId, const String& commandId) {
  if (commandId.isEmpty()) return -1;
  for (uint8_t i = 0; i < 32; ++i)
    if (commandCache[i].lockerId == lockerId && commandCache[i].commandId == commandId) return i;
  return -1;
}

static void cacheCommand(const String& lockerId, const String& commandId, const String& ack) {
  if (commandId.isEmpty()) return;
  ProcessedCommand& item = commandCache[commandCacheIndex];
  item.lockerId = lockerId; item.commandId = commandId; item.ack = ack;
  commandCacheIndex = static_cast<uint8_t>((commandCacheIndex + 1) % 32);
}

static String hmacSha256(const String& message) {
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return String();
  uint8_t digest[32];
  if (mbedtls_md_hmac(md, reinterpret_cast<const uint8_t*>(Cfg::HMAC_SECRET), strlen(Cfg::HMAC_SECRET),
      reinterpret_cast<const uint8_t*>(message.c_str()), message.length(), digest) != 0) return String();
  static const char hex[] = "0123456789abcdef";
  char out[65];
  for (uint8_t i = 0; i < 32; ++i) { out[i * 2] = hex[digest[i] >> 4]; out[i * 2 + 1] = hex[digest[i] & 0x0F]; }
  out[64] = '\0';
  return String(out);
}

static bool secureEqual(const String& a, const String& b) {
  if (a.length() != 64 || b.length() != 64) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

bool verifySignedCommand(const String& command, String& result) {
  String fields[6];
  int from = 0;
  for (uint8_t i = 0; i < 5; ++i) {
    const int pos = command.indexOf('|', from);
    if (pos < 0) { result = "MALFORMED"; return false; }
    fields[i] = command.substring(from, pos); from = pos + 1;
  }
  if (command.indexOf('|', from) >= 0) { result = "MALFORMED"; return false; }
  fields[5] = command.substring(from);
  for (String& f : fields) f.trim();
  fields[0].toUpperCase(); fields[1].toUpperCase();
  if (fields[0].isEmpty() || fields[1].isEmpty() || fields[3].isEmpty() || fields[4].isEmpty()) { result = "MISSING_FIELD"; return false; }
  const int index = gLockers.findLockerId(fields[0]);
  if (index < 0) { result = "UNKNOWN_LOCKER"; return false; }
  const String canonical = fields[0] + '|' + fields[1] + '|' + fields[2] + '|' + fields[3] + '|' + fields[4];
  if (!secureEqual(hmacSha256(canonical), fields[5])) { result = "INVALID_SIGNATURE"; return false; }
  if (Cfg::ENABLE_COMMAND_TIMESTAMP_CHECK) {
    char* end = nullptr; const long long ts = strtoll(fields[2].c_str(), &end, 10);
    if (!end || *end || !gClock.valid()) { result = "INVALID_TIMESTAMP"; return false; }
    const long long delta = static_cast<long long>(gClock.nowUtc()) - ts;
    if (llabs(delta) > Cfg::COMMAND_WINDOW_SECONDS) { result = "STALE_COMMAND"; return false; }
  }
  for (const String& used : nonceHistory) if (used == fields[3]) { result = "REPLAY"; return false; }
  nonceHistory[nonceIndex] = fields[3]; nonceIndex = (nonceIndex + 1) % 16;
  result = fields[0] + '|' + fields[1] + '|' + fields[4];
  return true;
}

static void handleRemoteCommand(const String& command) {
  String lockerId, commandId;
  String identityFields[5];
  int cursor = 0;
  bool identityValid = true;
  for (uint8_t i = 0; i < 5; ++i) {
    const int separator = command.indexOf('|', cursor);
    if (separator < 0) { identityValid = false; break; }
    identityFields[i] = command.substring(cursor, separator);
    cursor = separator + 1;
  }
  if (identityValid) {
    lockerId = identityFields[0]; lockerId.trim(); lockerId.toUpperCase();
    commandId = identityFields[4]; commandId.trim();
  }
  const int index = gLockers.findLockerId(lockerId);
  if (index >= 0) {
    const int cached = findCachedCommand(lockerId, commandId);
    if (cached >= 0) {
      gNetwork.publishAck(index, commandCache[cached].ack);
      Serial.println(commandCache[cached].ack);
      return;
    }
  }

  String parsed;
  const bool valid = verifySignedCommand(command, parsed);
  String action;
  if (valid) {
    const int a = parsed.indexOf('|'), b = parsed.indexOf('|', a + 1);
    lockerId = parsed.substring(0, a); action = parsed.substring(a + 1, b); commandId = parsed.substring(b + 1);
  }
  String result = valid ? "ACCEPTED" : parsed;
  bool accepted = valid;
  if (accepted) {
    if (action == "LOCK") accepted = gLockers.remoteSet(index, true);
    else if (action == "UNLOCK") { accepted = gLockers.remoteSet(index, false); if (!accepted) result = "LOCKER_NOT_ACTIVE_OR_EXPIRED"; }
    else if (action == "EXTEND") accepted = gLockers.extend(index, result);
    else if (action == "RELEASE") { accepted = gLockers.release(index); if (!accepted) result = "LOCKER_NOT_ASSIGNED"; }
    else { accepted = false; result = "INVALID_ACTION"; }
  }
  if (index >= 0) {
    const String ack = String(accepted ? "ACCEPTED|" : "REJECTED|") + lockerId + '|' + commandId + '|' + result;
    gNetwork.publishAck(index, ack);
    if (valid) cacheCommand(lockerId, commandId, ack);
    Serial.println(ack);
  } else Serial.printf("Rejected command: %s\n", result.c_str());
}

static void handleSerial() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') { if (serialBuffer.length() < 180) serialBuffer += c; continue; }
    serialBuffer.trim();
    if (serialBuffer.isEmpty()) continue;
    if (serialBuffer.indexOf('|') >= 0) handleRemoteCommand(serialBuffer);
    else {
      String input = serialBuffer; input.toUpperCase();
      if (input == "STATUS") {
        Serial.printf("Available lockers: %d/%u\n", gLockers.availableCount(), Cfg::LOCKER_COUNT);
        for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i)
          Serial.printf("%s assigned=%u expired=%u door=%s\n", Cfg::LOCKER_IDS[i], isAssigned(gLockers.record(i)), isExpiredHold(gLockers.record(i)), gHardware.doorClosed(i) ? "CLOSED" : "OPEN");
      } else if (input == "TIME") {
        char t[24]; gClock.formatLocal(t, sizeof(t)); Serial.println(t);
      } else if (input.startsWith("LOCK ") || input.startsWith("UNLOCK ")) {
        const bool lock = input.startsWith("LOCK ");
        const String id = input.substring(lock ? 5 : 7);
        const int i = gLockers.findLockerId(id);
        if (i >= 0) gLockers.remoteSet(i, lock); else Serial.println(F("Unknown locker"));
      } else Serial.println(F("Commands: STATUS, TIME, LOCK LOCKER_001, UNLOCK LOCKER_001, HELP"));
    }
    serialBuffer = "";
  }
}

void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println(F("Smart Locker | ESP32-S3 N16R8 | PlatformIO modular build"));

  gHardware.begin();                 // I2C, SPI, TFT, RFID, servo outputs, reed input
  gClock.begin();                    // RTC disabled; NTP-only clock
  if (!gStorage.begin()) {
    Serial.println(F("FATAL: NVS initialization failed; locker state storage unavailable."));
    while (true) { gHardware.beep(250); delay(1000); }
  }
  gLockers.begin(gStorage, gHardware, gClock, gNetwork);
  gNetwork.setCommandHandler(handleRemoteCommand);
  gNetwork.begin();
  gUi.begin(gHardware, gLockers, gClock, gNetwork);

  Serial.printf(
      "Clock: %s | SD: %s | Keypad PCF8575: %s\n",
      gClock.valid() ? "RTC valid" : "RTC unavailable",
      gStorage.sdReady() ? "ready" : "not detected (optional)",
      gHardware.keypadReady() ? "ready" : "not detected"
  );
  Serial.println(F("Key map: A assign / B unlock; A RFID / B PIN; C back; * delete; # submit."));
}

void loop() {
  gNetwork.tick();
  static bool ledInitialized = false;
  static bool lastLedState = false;
  const bool networkOnline = gNetwork.online();
  if (!ledInitialized || networkOnline != lastLedState) {
    gHardware.setLed(networkOnline);
    lastLedState = networkOnline;
    ledInitialized = true;
  }
  if (WiFi.status() == WL_CONNECTED) {
    gClock.beginNetworkSync();
  }

  gUi.tick();
  gClock.tick();
  gClock.syncFromNtpIfReady();
  gLockers.tick();
  handleSerial();

  // Send at most one queued event per 250 ms to keep MQTT work bounded.
  const uint32_t now = millis();
  if (gNetwork.mqttConnected() && now - lastSyncAttempt >= 250) {
    lastSyncAttempt = now;
    String pending;
    if (gStorage.peek(pending) && gNetwork.publishEvent(pending)) gStorage.pop();
  }

  static uint32_t lastStatusPublish = 0;
  if (gNetwork.mqttConnected() && now - lastStatusPublish >= 30000) {
    lastStatusPublish = now;
    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i)
      gNetwork.publishStatus(i, gLockers.isLocked(i) ? "LOCKED" : "UNLOCKED");
  }
}
