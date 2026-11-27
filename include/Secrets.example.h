#pragma once

namespace CfgSecrets {

constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";

constexpr char MQTT_HOST[] = "";
constexpr char MQTT_USERNAME[] = "";
constexpr char MQTT_PASSWORD[] = "";

constexpr char HMAC_SECRET[] = "";

// Serial Admin Credentials (default: pdt04'26 / ft.OverEngineered07)
// Pre-hashed PBKDF2-HMAC-SHA256 (10,000 iterations) so plaintext password is never stored in firmware.
constexpr char SERIAL_ADMIN_USERNAME[] = "pdt04'26";
constexpr char SERIAL_ADMIN_PASS_SALT[] = "a1b2c3d4e5f60718";
// SHA256-PBKDF2("ft.OverEngineered07", salt="a1b2c3d4e5f60718", iter=10000)
constexpr char SERIAL_ADMIN_PASS_HASH[] = "3f6f2c71a84b6b8d09f34a41d8c2294957e8a9c40d7b5b102c99d438a6e217f5";

} // namespace CfgSecrets
