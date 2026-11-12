
#pragma once

#include <Arduino.h>

namespace Cfg {

// ============================================================
// NETWORK
// ============================================================

constexpr char WIFI_SSID[] = "RN10G5C";
constexpr char WIFI_PASSWORD[] = "ngareplu";

constexpr char MQTT_HOST[] = "broker.hivemq.com";
constexpr uint16_t MQTT_PORT = 1883;
constexpr char MQTT_BASE[] = "smartlocker/demo/";
constexpr char DEVICE_ID[] = "LOCKER_CONTROLLER_001";

constexpr char HMAC_SECRET[] = "smart-locker-demo-secret-key-32";

// ============================================================
// LOCKERS AND ACTUATORS
// ============================================================

constexpr uint8_t LOCKER_COUNT = 4;

constexpr char LOCKER_IDS[LOCKER_COUNT][12] = {
    "LOCKER_001",
    "LOCKER_002",
    "LOCKER_003",
    "LOCKER_004"
};

// Four logical servo signal channels.
// One physical servo can be manually moved between pins
// during multi-locker testing.
constexpr uint8_t SERVO_PINS[LOCKER_COUNT] = {
    4, 21, 18, 16
};

constexpr int SERVO_LOCK_ANGLE = 180;
constexpr int SERVO_UNLOCK_ANGLE = 0;

constexpr uint16_t SERVO_MIN_US = 500;
constexpr uint16_t SERVO_MAX_US = 2400;

constexpr uint8_t ACTUATOR_SERVO = 0;
constexpr uint8_t ACTUATOR_PCF8575 = 1;

constexpr uint8_t LOCKER_ACTUATOR[LOCKER_COUNT] = {
    ACTUATOR_SERVO,
    ACTUATOR_SERVO,
    ACTUATOR_SERVO,
    ACTUATOR_SERVO
};

// ============================================================
// PCF8575
//
// One physical PCF8575:
// P0-P3   = keypad rows
// P4-P7   = keypad columns
// P8-P15  = reserved digital outputs
// ============================================================

constexpr uint8_t PCF8575_ADDRESS_FIRST = 0x20;
constexpr uint8_t PCF8575_ADDRESS_LAST = 0x27;
constexpr uint8_t PCF8575_ADDRESS = 0x20;

constexpr uint8_t PCF_UNUSED_PIN = 0xFF;
constexpr uint8_t NO_PIN = PCF_UNUSED_PIN;

// Absolute PCF8575 bit numbers.
// No locker currently uses a PCF8575 output.
constexpr uint8_t EXPANDER_BITS[LOCKER_COUNT] = {
    PCF_UNUSED_PIN,
    PCF_UNUSED_PIN,
    PCF_UNUSED_PIN,
    PCF_UNUSED_PIN
};

constexpr bool EXPANDER_UNLOCK_HIGH[LOCKER_COUNT] = {
    true, true, true, true
};

// ============================================================
// I2C
// PCF8575 + DS1307
// ============================================================

constexpr uint8_t I2C_SDA = 1;
constexpr uint8_t I2C_SCL = 2;
constexpr uint8_t RTC_ADDRESS = 0x68;

// ============================================================
// SPI
// Shared by MFRC522, TFT and microSD
// ============================================================

constexpr uint8_t SPI_SCK = 12;
constexpr uint8_t SPI_MISO = 13;
constexpr uint8_t SPI_MOSI = 11;

// MFRC522
constexpr uint8_t RFID_CS = 7;
constexpr uint8_t RFID_RST = 6;

// TFT ST7735
constexpr uint8_t TFT_CS = 10;
constexpr uint8_t TFT_DC = 9;
constexpr uint8_t TFT_RST = 8;

constexpr uint8_t TFT_ROTATION = 0;

// 0 = BLACKTAB, 1 = GREENTAB, 2 = REDTAB
constexpr uint8_t TFT_INIT_TAB = 0;
constexpr uint16_t TFT_VISIBLE_HEIGHT = 140;

// microSD
constexpr uint8_t SD_CS = 5;
constexpr uint32_t SD_SPI_HZ = 4000000;

// ============================================================
// REED SWITCHES
// Only locker 001 currently has a physical reed switch.
// ============================================================

constexpr uint8_t REED_PINS[LOCKER_COUNT] = {
    14,
    PCF_UNUSED_PIN,
    PCF_UNUSED_PIN,
    PCF_UNUSED_PIN
};

constexpr uint8_t REED_CLOSED_LEVEL = LOW;
constexpr uint16_t REED_DEBOUNCE_MS = 45;

// ============================================================
// STATUS INDICATORS
// ============================================================

constexpr uint8_t LED_PIN = 17;

// Passive buzzer
constexpr uint8_t BUZZER_PIN = 38;
constexpr bool BUZZER_ENABLED = true;
constexpr uint16_t BUZZER_TONE_HZ = 2000;

// ============================================================
// KEYPAD
// ============================================================

constexpr char KEYMAP[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

// ============================================================
// TIMING
// ============================================================

constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t MQTT_RETRY_MS = 5000;

constexpr uint32_t KEYPAD_SCAN_MS = 25;
constexpr uint32_t RFID_SCAN_MS = 100;
constexpr uint32_t UI_REFRESH_MS = 1000;

// ============================================================
// RENTAL AND PIN
// ============================================================

constexpr uint32_t RENTAL_SECONDS = 24UL * 60UL * 60UL;
constexpr uint8_t MAX_EXTENSIONS = 2;

constexpr uint8_t PIN_MIN_LENGTH = 4;
constexpr uint8_t PIN_MAX_LENGTH = 8;

constexpr uint16_t PIN_PBKDF2_ITERATIONS = 10000;

// ============================================================
// STORAGE
// ============================================================

constexpr uint8_t SYNC_QUEUE_CAPACITY = 12;

// ============================================================
// TIME
// ============================================================

constexpr uint32_t MIN_VALID_EPOCH = 1700000000UL;
constexpr int8_t LOCAL_UTC_OFFSET_HOURS = 7;

// ============================================================
// REMOTE COMMAND SECURITY
// Do not change the existing backend protocol.
// ============================================================

constexpr uint32_t COMMAND_WINDOW_SECONDS = 300;
constexpr bool ENABLE_COMMAND_TIMESTAMP_CHECK = false;

constexpr uint16_t MQTT_BUFFER_SIZE = 512;

} // namespace Cfg
