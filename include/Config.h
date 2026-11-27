#pragma once

#include <Arduino.h>

#if __has_include("Secrets.h")
#include "Secrets.h"
#else
#include "Secrets.example.h"
#endif

namespace Cfg {

// ============================================================
// SITE, CONTROLLER & SCALABILITY IDENTITY
// ============================================================

constexpr char LOCATION_ID[] = "LOC_CAMPUS_A";
constexpr char DEVICE_ID[]   = "LOCKER_CTRL_01";
constexpr uint8_t LOCKER_COUNT = 4;

constexpr char LOCKER_IDS[LOCKER_COUNT][12] = {
    "LOCKER_001",
    "LOCKER_002",
    "LOCKER_003",
    "LOCKER_004"
};

// ============================================================
// NETWORK & TLS MQTT (PORT 8883)
// ============================================================

constexpr bool MQTT_USE_TLS = true;
constexpr char MQTT_HOST[] = CfgSecrets::MQTT_HOST;
constexpr uint16_t MQTT_PORT = 8883;
constexpr char MQTT_BASE[] = "smartlocker/demo/";
constexpr char MQTT_USERNAME[] = CfgSecrets::MQTT_USERNAME;
constexpr char MQTT_PASSWORD[] = CfgSecrets::MQTT_PASSWORD;

// ISRG Root X1 PEM CA for HiveMQ Cloud / Let's Encrypt TLS validation
constexpr char MQTT_ROOT_CA[] = R"EOF(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----)EOF";

constexpr char WIFI_SSID[] = CfgSecrets::WIFI_SSID;
constexpr char WIFI_PASSWORD[] = CfgSecrets::WIFI_PASSWORD;
constexpr char HMAC_SECRET[] = CfgSecrets::HMAC_SECRET;

// ============================================================
// SERVO ACTUATORS (4 ADJACENT PINS, 165/15 DEG SAFE STOPS)
// ============================================================

constexpr uint8_t SERVO_PINS[LOCKER_COUNT] = {
    15, 16, 17, 18
};

constexpr int SERVO_LOCK_ANGLE = 165;
constexpr int SERVO_UNLOCK_ANGLE = 15;
constexpr uint16_t SERVO_MIN_US = 1000;
constexpr uint16_t SERVO_MAX_US = 2000;

// ============================================================
// PCF8575 I/O EXPANDER (P00-P07 KEYPAD, P8-P11 REED SENSORS)
// ============================================================

constexpr uint8_t PCF8575_ADDRESS_FIRST = 0x20;
constexpr uint8_t PCF8575_ADDRESS_LAST  = 0x27;
constexpr uint8_t PCF8575_ADDRESS       = 0x20;
constexpr uint8_t PCF_UNUSED_PIN        = 0xFF;
constexpr uint8_t NO_PIN                = PCF_UNUSED_PIN;

// PCF8575 bits 8..11 correspond to silkscreen P10..P13
constexpr uint8_t PCF_REED_BITS[LOCKER_COUNT] = {
    8, 9, 10, 11
};

constexpr bool REED_CLOSED_LEVEL = false; // Active-LOW to GND
constexpr uint16_t REED_DEBOUNCE_MS = 45;
constexpr uint32_t DOOR_OPEN_ALERT_MS = 60000;
constexpr uint32_t DOOR_ALERT_REPEAT_MS = 10000;

// ============================================================
// I2C, SPI, DUAL LEDS & BUZZER PINS
// ============================================================

constexpr uint8_t I2C_SDA = 1;
constexpr uint8_t I2C_SCL = 2;

constexpr uint8_t SPI_SCK  = 12;
constexpr uint8_t SPI_MISO = 13;
constexpr uint8_t SPI_MOSI = 11;

constexpr uint8_t RFID_CS  = 7;
constexpr uint8_t RFID_RST = 6;

constexpr uint8_t TFT_CS   = 10;
constexpr uint8_t TFT_DC   = 9;
constexpr uint8_t TFT_RST  = 8;

// 2 = Portrait rotated 180 degrees
constexpr uint8_t TFT_ROTATION = 2;
constexpr uint8_t TFT_INIT_TAB = 0;
constexpr uint16_t TFT_WIDTH = 128;
constexpr uint16_t TFT_VISIBLE_HEIGHT = 140;

constexpr uint8_t SD_CS = 5;
constexpr uint32_t SD_SPI_HZ = 4000000;

constexpr uint8_t LED_PIN = 14;      // Discrete online status LED
constexpr uint8_t RGB_LED_PIN = 48;  // ESP32-S3 onboard WS2812 RGB LED
constexpr uint8_t RGB_BRIGHTNESS = 28;

constexpr uint8_t BUZZER_PIN = 38;
constexpr bool BUZZER_ENABLED = true;
constexpr uint16_t BUZZER_TONE_HZ = 2000;

// ============================================================
// KEYPAD & TIMING
// ============================================================

constexpr char KEYMAP[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t MQTT_RETRY_MS = 5000;

constexpr uint32_t KEYPAD_SCAN_MS = 25;
constexpr uint32_t RFID_SCAN_MS = 100;
constexpr uint32_t UI_REFRESH_MS = 1000;
constexpr uint32_t UI_RESULT_MS = 3000;
constexpr uint32_t UI_IDLE_TIMEOUT_MS = 18000;
constexpr uint32_t ADMIN_MENU_TIMEOUT_MS = 120000;
constexpr uint32_t SERIAL_ADMIN_TIMEOUT_MS = 600000;

// ============================================================
// RENTAL, CREDENTIALS, ADMIN & QUEUE LIMITS
// ============================================================

constexpr uint32_t RENTAL_SECONDS = 24UL * 60UL * 60UL;
constexpr uint8_t PIN_MIN_LENGTH = 4;
constexpr uint8_t PIN_MAX_LENGTH = 8;
constexpr uint8_t RFID_OTP_LENGTH = 8;
constexpr uint16_t PIN_PBKDF2_ITERATIONS = 10000;

constexpr uint8_t MAX_USERS = 32;
constexpr uint8_t MAX_PENDING_OTPS = 16;
constexpr uint32_t OTP_TTL_MS = 5UL * 60UL * 1000UL; // 5 minutes

constexpr char SERIAL_ADMIN_USER[] = "pdt04'26";
constexpr char SERIAL_ADMIN_INIT_PASS[] = "ft.OverEngineered07";
constexpr char APP_REGISTER_URL[] = "app.smartlocker.id";

constexpr uint8_t SYNC_QUEUE_CAPACITY = 32;
constexpr uint8_t DEBUG_LOG_CAPACITY = 24;
constexpr size_t LOG_MAX_BYTES = 32768;

// ============================================================
// TIME, SECURITY & FREERTOS IPC
// ============================================================

constexpr uint32_t MIN_VALID_EPOCH = 1700000000UL;
constexpr int8_t LOCAL_UTC_OFFSET_HOURS = 7;
constexpr uint32_t NTP_RESYNC_MS = 2UL * 60UL * 60UL * 1000UL;

constexpr uint32_t COMMAND_WINDOW_SECONDS = 300;
constexpr bool ENABLE_COMMAND_TIMESTAMP_CHECK = false;
constexpr uint16_t MQTT_BUFFER_SIZE = 768;

constexpr uint8_t AUTH_QUEUE_LEN = 4;
constexpr uint8_t MQTT_IN_QUEUE_LEN = 12;
constexpr uint8_t MQTT_OUT_QUEUE_LEN = 16;

} // namespace Cfg