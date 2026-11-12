#include "App.h"

NetworkService* NetworkService::_self = nullptr;

void NetworkService::begin() {
  _self = this;
  _mqtt.setServer(Cfg::MQTT_HOST, Cfg::MQTT_PORT);
  _mqtt.setCallback(NetworkService::callback);
  _mqtt.setBufferSize(Cfg::MQTT_BUFFER_SIZE);
  _mqtt.setSocketTimeout(2);
  _mqtt.setKeepAlive(20);
  WiFi.mode(WIFI_STA);
  WiFi.begin(Cfg::WIFI_SSID, Cfg::WIFI_PASSWORD);
  _lastWifiAttempt = millis();
}

bool NetworkService::online() {
    return WiFi.status() == WL_CONNECTED && _mqtt.connected();
}

bool NetworkService::mqttConnected() {
    return _mqtt.connected();
}

void NetworkService::tick() {
  const uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (now - _lastWifiAttempt >= Cfg::WIFI_RETRY_MS) {
      _lastWifiAttempt = now;
      WiFi.reconnect();
    }
    return;
  }
  if (!_mqtt.connected()) {
    if (now - _lastMqttAttempt >= Cfg::MQTT_RETRY_MS) {
      _lastMqttAttempt = now;
      connectMqtt();
    }
  } else {
    _mqtt.loop();
  }
}

void NetworkService::connectMqtt() {
  char clientId[40];
  snprintf(clientId, sizeof(clientId), "locker-%llX", static_cast<unsigned long long>(ESP.getEfuseMac()));
  const char* willTopic = "smartlocker/demo/device/status";
  if (!_mqtt.connect(clientId, willTopic, 1, true, "OFFLINE")) {
    Serial.printf("MQTT connect failed: %d\n", _mqtt.state());
    return;
  }
  _mqtt.publish(willTopic, "ONLINE", true);
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
    char topic[64]; snprintf(topic, sizeof(topic), "%s%s/command", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[i]);
    _mqtt.subscribe(topic, 1);
  }
}

void NetworkService::callback(char* topic, byte* payload, unsigned int length) {
  if (!_self || !_self->_commandHandler || length == 0 || length >= Cfg::MQTT_BUFFER_SIZE) return;
  String command; command.reserve(length);
  for (unsigned int i = 0; i < length; ++i) command += static_cast<char>(payload[i]);
  const int sep = command.indexOf('|');
  if (sep <= 0) return;
  String lockerId = command.substring(0, sep); lockerId.trim(); lockerId.toUpperCase();
  bool topicMatches = false;
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
    char expected[64]; snprintf(expected, sizeof(expected), "%s%s/command", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[i]);
    if (lockerId == Cfg::LOCKER_IDS[i] && strcmp(topic, expected) == 0) topicMatches = true;
  }
  if (topicMatches) _self->_commandHandler(command);
}

bool NetworkService::publishEvent(const String& payload) {
  return _mqtt.connected() && _mqtt.publish("smartlocker/demo/device/event", payload.c_str(), false);
}

bool NetworkService::publishStatus(uint8_t index, const char* state) {
  if (!_mqtt.connected() || index >= Cfg::LOCKER_COUNT) return false;
  char topic[64]; snprintf(topic, sizeof(topic), "%s%s/status", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[index]);
  return _mqtt.publish(topic, state, true);
}

bool NetworkService::publishAck(uint8_t index, const String& payload) {
  if (!_mqtt.connected() || index >= Cfg::LOCKER_COUNT) return false;
  char topic[64]; snprintf(topic, sizeof(topic), "%s%s/ack", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[index]);
  return _mqtt.publish(topic, payload.c_str(), false);
}

void NetworkService::setCommandHandler(void (*handler)(const String&)) { _commandHandler = handler; }
