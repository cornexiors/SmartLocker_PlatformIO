#pragma once

#include <Arduino.h>

namespace Cfg {

// ============================================================
// NETWORK & PLAIN MQTT (PORT 1883 - NO TLS)
// ============================================================

constexpr char WIFI_SSID[] = "RN10G5C";
constexpr char WIFI_PASSWORD[] = "ngareplu";

constexpr char MQTT_HOST[] = "broker.hivemq.com";
constexpr uint16_t MQTT_PORT = 1883;
constexpr char MQTT_BASE[] = "smartlocker/demo/";
constexpr char DEVICE_ID[] = "LOCKER_CONTROLLER_001";
constexpr uint16_t MQTT_BUFFER_SIZE = 512;

// ============================================================
// LOCKERS AND 180-DEGREE POSITIONAL SERVOS
// ============================================================

constexpr uint8_t LOCKER_COUNT = 4;

constexpr char LOCKER_IDS[LOCKER_COUNT][12] = {
    "LOCKER_001",
    "LOCKER_002",
    "LOCKER_003",
    "LOCKER_004"
};

constexpr uint8_t SERVO_PINS[LOCKER_COUNT] = {
    4, 21, 18, 16
};

constexpr int SERVO_LOCK_ANGLE = 180;
constexpr int SERVO_UNLOCK_ANGLE = 0;

constexpr uint16_t SERVO_MIN_US = 1000;
constexpr uint16_t SERVO_MAX_US = 2000;

// ============================================================
// PCF8575 I2C EXPANDER (4x4 KEYPAD)
// ============================================================

constexpr uint8_t PCF8575_ADDRESS_FIRST = 0x20;
constexpr uint8_t PCF8575_ADDRESS_LAST = 0x27;
constexpr uint8_t PCF8575_ADDRESS = 0x20;
constexpr uint8_t NO_PIN = 0xFF;

// ============================================================
// I2C & SPI PINS
// ============================================================

constexpr uint8_t I2C_SDA = 1;
constexpr uint8_t I2C_SCL = 2;

constexpr uint8_t SPI_SCK = 12;
constexpr uint8_t SPI_MISO = 13;
constexpr uint8_t SPI_MOSI = 11;

constexpr uint8_t RFID_CS = 7;
constexpr uint8_t RFID_RST = 6;

constexpr uint8_t TFT_CS = 10;
constexpr uint8_t TFT_DC = 9;
constexpr uint8_t TFT_RST = 8;

constexpr uint8_t TFT_ROTATION = 2; // Upright portrait
constexpr uint8_t TFT_INIT_TAB = 0;
constexpr uint16_t TFT_WIDTH = 128;
constexpr uint16_t TFT_VISIBLE_HEIGHT = 140;

// ============================================================
// REED SWITCHES & INDICATORS
// ============================================================

constexpr uint8_t REED_PINS[LOCKER_COUNT] = {
    14,
    NO_PIN,
    NO_PIN,
    NO_PIN
};

constexpr uint8_t REED_CLOSED_LEVEL = LOW;
constexpr uint16_t REED_DEBOUNCE_MS = 45;

constexpr uint8_t LED_PIN = 17;
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
constexpr uint32_t UI_RESULT_MS = 2500;
constexpr uint32_t UI_IDLE_TIMEOUT_MS = 18000;

constexpr uint8_t PIN_MIN_LENGTH = 4;
constexpr uint8_t PIN_MAX_LENGTH = 8;

} // namespace Cfg