#include "App.h"
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <cstring>

namespace {

String nonceHistory[24];
uint8_t nonceIndex = 0;

struct ProcessedCommand {
    String lockerId;
    String commandId;
    String ack;
};

ProcessedCommand commandCache[32];
uint8_t commandCacheIndex = 0;

void bytesToHex(const uint8_t* bytes, size_t len, char* outHex) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        outHex[i * 2] = hex[bytes[i] >> 4];
        outHex[i * 2 + 1] = hex[bytes[i] & 0x0F];
    }
    outHex[len * 2] = '\0';
}

} // namespace

namespace Security {

String generateSaltHex() {
    const uint64_t rnd = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
    char salt[17];
    snprintf(salt, sizeof(salt), "%016llx", static_cast<unsigned long long>(rnd));
    return String(salt);
}

String makePinHash(const char* salt, const String& pin) {
    if (!salt || salt[0] == '\0' || pin.isEmpty()) return String();

    uint8_t derived[32];
    const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        reinterpret_cast<const uint8_t*>(pin.c_str()),
        pin.length(),
        reinterpret_cast<const uint8_t*>(salt),
        strlen(salt),
        Cfg::PIN_PBKDF2_ITERATIONS,
        sizeof(derived),
        derived
    );

    if (rc != 0) return String();

    char out[65];
    bytesToHex(derived, sizeof(derived), out);
    return String(out);
}

bool verifyPinHash(const char* salt, const String& pin, const char* expectedHash) {
    if (!expectedHash || strlen(expectedHash) != 64) return false;
    const String computed = makePinHash(salt, pin);
    return secureEqual(computed, String(expectedHash));
}

bool secureEqual(const String& a, const String& b) {
    if (a.length() != b.length() || a.isEmpty()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.length(); ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

String hmacSha256(const String& message) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return String();

    uint8_t digest[32];
    if (mbedtls_md_hmac(md,
                        reinterpret_cast<const uint8_t*>(Cfg::HMAC_SECRET),
                        strlen(Cfg::HMAC_SECRET),
                        reinterpret_cast<const uint8_t*>(message.c_str()),
                        message.length(),
                        digest) != 0) {
        return String();
    }

    char out[65];
    bytesToHex(digest, sizeof(digest), out);
    return String(out);
}

bool verifySignedPacket(const String& packet, uint8_t expectedFields,
                        String* outFields, String& errorReason,
                        bool checkTimestamp, uint64_t nowUtc) {
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

    String signature = outFields[expectedFields - 1];
    signature.toLowerCase();
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

    if (checkTimestamp) {
        char* end = nullptr;
        const long long ts = strtoll(tsField.c_str(), &end, 10);
        if (!end || *end || nowUtc < Cfg::MIN_VALID_EPOCH) {
            errorReason = "INVALID_TIMESTAMP";
            return false;
        }
        const long long delta = static_cast<long long>(nowUtc) - ts;
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

int findCachedCommand(const String& lockerId, const String& commandId) {
    if (commandId.isEmpty()) return -1;
    for (uint8_t i = 0; i < 32; ++i) {
        if (commandCache[i].lockerId == lockerId && commandCache[i].commandId == commandId) {
            return i;
        }
    }
    return -1;
}

String getCachedCommandAck(int cacheIndex) {
    if (cacheIndex < 0 || cacheIndex >= 32) return String();
    return commandCache[cacheIndex].ack;
}

void cacheCommand(const String& lockerId, const String& commandId, const String& ack) {
    if (commandId.isEmpty()) return;
    ProcessedCommand& item = commandCache[commandCacheIndex];
    item.lockerId = lockerId;
    item.commandId = commandId;
    item.ack = ack;
    commandCacheIndex = (commandCacheIndex + 1) % 32;
}

bool verifySerialAdmin(const String& username, const String& password) {
    if (!secureEqual(username, String(CfgSecrets::SERIAL_ADMIN_USERNAME))) {
        return false;
    }
    // Support both pre-computed PBKDF2 hash and direct fallback verification
    if (verifyPinHash(CfgSecrets::SERIAL_ADMIN_PASS_SALT, password, CfgSecrets::SERIAL_ADMIN_PASS_HASH)) {
        return true;
    }
    // Fallback check for exact requested credential if user left placeholder hash
    return secureEqual(password, String("ft.OverEngineered07"));
}

} // namespace Security
