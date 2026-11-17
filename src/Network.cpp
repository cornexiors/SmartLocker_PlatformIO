#include "App.h"

NetworkService* NetworkService::_self = nullptr;

void NetworkService::begin(const char* ssid, const char* password) {
    _self = this;
    _ssid = ssid ? ssid : Cfg::WIFI_SSID;
    _password = password ? password : Cfg::WIFI_PASSWORD;

    if (Cfg::MQTT_USE_TLS) {
        if (Cfg::MQTT_ROOT_CA[0] != '\0') {
            _tlsClient.setCACert(Cfg::MQTT_ROOT_CA);
        } else {
            _tlsClient.setInsecure();
        }
        _mqtt.setClient(_tlsClient);
    } else {
        _mqtt.setClient(_plainClient);
    }

    _mqtt.setServer(Cfg::MQTT_HOST, Cfg::MQTT_PORT);
    _mqtt.setCallback(NetworkService::callback);
    _mqtt.setBufferSize(Cfg::MQTT_BUFFER_SIZE);
    _mqtt.setSocketTimeout(3);
    _mqtt.setKeepAlive(20);

    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid.c_str(), _password.c_str());
    _lastWifiAttempt = millis();
}

void NetworkService::setWifiCredentials(const String& ssid, const String& password) {
    _ssid = ssid;
    _password = password;
    if (_mqtt.connected()) {
        _mqtt.disconnect();
    }
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(_ssid.c_str(), _password.c_str());
    _lastWifiAttempt = millis();
}

bool NetworkService::wifiConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

bool NetworkService::mqttConnected() {
    return wifiConnected() && _mqtt.connected();
}

bool NetworkService::online() {
    return mqttConnected();
}

void NetworkService::tick() {
    const uint32_t now = millis();
    if (!wifiConnected()) {
        if (now - _lastWifiAttempt >= Cfg::WIFI_RETRY_MS) {
            _lastWifiAttempt = now;
            WiFi.begin(_ssid.c_str(), _password.c_str());
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
    snprintf(clientId, sizeof(clientId), "locker-%llX",
             static_cast<unsigned long long>(ESP.getEfuseMac()));

    char willTopic[64];
    snprintf(willTopic, sizeof(willTopic), "%sdevice/status", Cfg::MQTT_BASE);

    if (!_mqtt.connect(clientId, willTopic, 1, true, "OFFLINE")) {
        appLog("WARN", "MQTT connect failed: %d", _mqtt.state());
        return;
    }

    appLog("INFO", "MQTT connected");
    _mqtt.publish(willTopic, "ONLINE", true);

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        char topic[64];
        snprintf(topic, sizeof(topic), "%s%s/command", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[i]);
        _mqtt.subscribe(topic, 1);
    }

    char syncTopic[64];
    snprintf(syncTopic, sizeof(syncTopic), "%susers/sync", Cfg::MQTT_BASE);
    _mqtt.subscribe(syncTopic, 1);

    publishUserSyncRequest(_cachedUserCount);
}

void NetworkService::callback(char* topic, byte* payload, unsigned int length) {
    if (!_self || length == 0 || length >= Cfg::MQTT_BUFFER_SIZE) return;

    String message;
    message.reserve(length);
    for (unsigned int i = 0; i < length; ++i) {
        message += static_cast<char>(payload[i]);
    }

    char syncTopic[64];
    snprintf(syncTopic, sizeof(syncTopic), "%susers/sync", Cfg::MQTT_BASE);
    if (strcmp(topic, syncTopic) == 0) {
        if (_self->_userSyncHandler) _self->_userSyncHandler(message);
        return;
    }

    const int sep = message.indexOf('|');
    if (sep <= 0) return;

    String lockerId = message.substring(0, sep);
    lockerId.trim();
    lockerId.toUpperCase();

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        char expected[64];
        snprintf(expected, sizeof(expected), "%s%s/command", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[i]);
        if (lockerId == Cfg::LOCKER_IDS[i] && strcmp(topic, expected) == 0) {
            if (_self->_commandHandler) _self->_commandHandler(message);
            return;
        }
    }
}

bool NetworkService::publishEvent(const String& payload) {
    if (!mqttConnected()) return false;
    char topic[64];
    snprintf(topic, sizeof(topic), "%sdevice/event", Cfg::MQTT_BASE);
    return _mqtt.publish(topic, payload.c_str(), false);
}

bool NetworkService::publishLockerState(uint8_t index, bool locked, bool assigned,
                                        bool expired, const char* userId, const char* method) {
    if (!mqttConnected() || index >= Cfg::LOCKER_COUNT) return false;
    char topic[64];
    snprintf(topic, sizeof(topic), "%s%s/state", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[index]);

    char json[220];
    snprintf(json, sizeof(json),
             "{\"locker_id\":\"%s\",\"locked\":%s,\"assigned\":%s,\"expired\":%s,"
             "\"user_id\":\"%s\",\"method\":\"%s\"}",
             Cfg::LOCKER_IDS[index],
             locked ? "true" : "false",
             assigned ? "true" : "false",
             expired ? "true" : "false",
             userId ? userId : "",
             method ? method : "SYSTEM");

    return _mqtt.publish(topic, json, true);
}

bool NetworkService::publishAck(uint8_t index, const String& payload) {
    if (!mqttConnected() || index >= Cfg::LOCKER_COUNT) return false;
    char topic[64];
    snprintf(topic, sizeof(topic), "%s%s/ack", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[index]);
    return _mqtt.publish(topic, payload.c_str(), false);
}

bool NetworkService::publishRfidRegAck(const String& userId, const String& uid,
                                       bool success, const char* reason) {
    if (!mqttConnected()) return false;
    char topic[64];
    snprintf(topic, sizeof(topic), "%susers/ack", Cfg::MQTT_BASE);

    char payload[128];
    snprintf(payload, sizeof(payload),
             "RFID_REG_ACK|%s|%s|%s|%s",
             userId.c_str(),
             uid.c_str(),
             success ? "ACCEPTED" : "REJECTED",
             reason ? reason : "");

    return _mqtt.publish(topic, payload, false);
}

bool NetworkService::publishUserSyncRequest(uint8_t userCount) {
    _cachedUserCount = userCount;
    if (!mqttConnected()) return false;
    char topic[64];
    snprintf(topic, sizeof(topic), "%susers/request", Cfg::MQTT_BASE);

    char json[128];
    snprintf(json, sizeof(json),
             "{\"device_id\":\"%s\",\"action\":\"SYNC_REQUEST\",\"user_count\":%u}",
             Cfg::DEVICE_ID, userCount);

    return _mqtt.publish(topic, json, false);
}

void NetworkService::setCommandHandler(void (*handler)(const String&)) {
    _commandHandler = handler;
}

void NetworkService::setUserSyncHandler(void (*handler)(const String&)) {
    _userSyncHandler = handler;
}