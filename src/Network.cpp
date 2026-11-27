#include "App.h"

NetworkService* NetworkService::_self = nullptr;

void NetworkService::begin(const char* ssid, const char* password) {
    _self = this;
    _netMutex = xSemaphoreCreateMutex();
    _inQueue = xQueueCreate(12, sizeof(MqttInMsg));

    _ssid = ssid ? ssid : Cfg::WIFI_SSID;
    _password = password ? password : Cfg::WIFI_PASSWORD;
    _mqttBackoffMs = Cfg::MQTT_RETRY_MS;

    if (_ssid.isEmpty() || Cfg::MQTT_HOST[0] == '\0' ||
        Cfg::MQTT_USERNAME[0] == '\0' || Cfg::MQTT_PASSWORD[0] == '\0' ||
        Cfg::HMAC_SECRET[0] == '\0' || Cfg::MQTT_ROOT_CA[0] == '\0') {
        appLog("ERROR", "Missing Wi-Fi/MQTT/TLS/HMAC secrets in Secrets.h");
        return;
    }

    _tlsClient.setCACert(Cfg::MQTT_ROOT_CA);
    _tlsClient.setHandshakeTimeout(5);
    _mqtt.setClient(_tlsClient);

    _mqtt.setServer(Cfg::MQTT_HOST, Cfg::MQTT_PORT);
    _mqtt.setCallback(NetworkService::callback);
    _mqtt.setBufferSize(Cfg::MQTT_BUFFER_SIZE);
    _mqtt.setSocketTimeout(2);
    _mqtt.setKeepAlive(20);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(_ssid.c_str(), _password.c_str());
    _lastWifiAttempt = millis();
}

void NetworkService::setWifiCredentials(const String& ssid, const String& password) {
    xSemaphoreTake(_netMutex, portMAX_DELAY);
    _ssid = ssid;
    _password = password;
    _wifiCredsDirty = true;
    xSemaphoreGive(_netMutex);
}

String NetworkService::currentSsid() const {
    xSemaphoreTake(_netMutex, portMAX_DELAY);
    String copy = _ssid;
    xSemaphoreGive(_netMutex);
    return copy;
}

bool NetworkService::wifiConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

bool NetworkService::mqttConnected() {
    if (!wifiConnected()) return false;
    xSemaphoreTake(_netMutex, portMAX_DELAY);
    const bool conn = _mqtt.connected();
    xSemaphoreGive(_netMutex);
    return conn;
}

bool NetworkService::online() {
    return mqttConnected();
}

void NetworkService::tick() {
    const uint32_t now = millis();

    xSemaphoreTake(_netMutex, portMAX_DELAY);
    if (_wifiCredsDirty) {
        _wifiCredsDirty = false;
        if (_mqtt.connected()) _mqtt.disconnect();
        WiFi.disconnect(true);
        vTaskDelay(pdMS_TO_TICKS(50));
        WiFi.begin(_ssid.c_str(), _password.c_str());
        _lastWifiAttempt = now;
        xSemaphoreGive(_netMutex);
        return;
    }
    const String ssidCopy = _ssid;
    const String passCopy = _password;
    xSemaphoreGive(_netMutex);

    if (!wifiConnected()) {
        if (now - _lastWifiAttempt >= Cfg::WIFI_RETRY_MS) {
            _lastWifiAttempt = now;
            WiFi.begin(ssidCopy.c_str(), passCopy.c_str());
        }
        return;
    }

    // Gate TLS handshake until SNTP has set valid epoch for X.509 certificate verification
    if (!gClock.valid()) {
        return;
    }

    xSemaphoreTake(_netMutex, portMAX_DELAY);
    if (!_mqtt.connected()) {
        if (now - _lastMqttAttempt >= _mqttBackoffMs) {
            _lastMqttAttempt = now;
            connectMqtt();
        }
    } else {
        _mqtt.loop();
    }
    xSemaphoreGive(_netMutex);
}

void NetworkService::connectMqtt() {
    _tlsClient.stop();

    char clientId[48];
    snprintf(clientId, sizeof(clientId), "%s-%llX",
             Cfg::DEVICE_ID, static_cast<unsigned long long>(ESP.getEfuseMac()));

    char willTopic[64];
    snprintf(willTopic, sizeof(willTopic), "%sdevice/status", Cfg::MQTT_BASE);

    if (!_mqtt.connect(clientId, Cfg::MQTT_USERNAME, Cfg::MQTT_PASSWORD,
                       willTopic, 1, true, "OFFLINE")) {
        appLog("WARN", "MQTT connect failed (%d), retry in %lus",
               _mqtt.state(), static_cast<unsigned long>(_mqttBackoffMs / 1000));
        _mqttBackoffMs = min<uint32_t>(_mqttBackoffMs * 2, 30000);
        return;
    }

    _mqttBackoffMs = Cfg::MQTT_RETRY_MS;
    appLog("INFO", "TLS MQTT connected (%s)", Cfg::LOCATION_ID);
    _mqtt.publish(willTopic, "ONLINE", true);

    for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
        char topic[64];
        snprintf(topic, sizeof(topic), "%s%s/command", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[i]);
        _mqtt.subscribe(topic, 1);
    }

    char syncTopic[64];
    snprintf(syncTopic, sizeof(syncTopic), "%susers/sync", Cfg::MQTT_BASE);
    _mqtt.subscribe(syncTopic, 1);
    _userSyncPending = true;
}

void NetworkService::callback(char* topic, byte* payload, unsigned int length) {
    if (!_self || !_self->_inQueue || length == 0 || length >= Cfg::MQTT_BUFFER_SIZE) return;

    MqttInMsg msg{};
    strncpy(msg.topic, topic, sizeof(msg.topic) - 1);
    memcpy(msg.payload, payload, length);
    msg.payload[length] = '\0';

    xQueueSend(_self->_inQueue, &msg, 0);
}

bool NetworkService::dequeueIncomingMqtt(MqttInMsg& out) {
    if (!_inQueue) return false;
    return xQueueReceive(_inQueue, &out, 0) == pdTRUE;
}

bool NetworkService::publishEvent(const String& payload) {
    if (!wifiConnected()) return false;
    xSemaphoreTake(_netMutex, portMAX_DELAY);
    bool ok = false;
    if (_mqtt.connected()) {
        char topic[64];
        snprintf(topic, sizeof(topic), "%sdevice/event", Cfg::MQTT_BASE);
        ok = _mqtt.publish(topic, payload.c_str(), false);
    }
    xSemaphoreGive(_netMutex);
    return ok;
}

bool NetworkService::publishLockerState(uint8_t index, bool locked, bool assigned,
                                        bool expired, const char* userId, const char* method) {
    if (!wifiConnected() || index >= Cfg::LOCKER_COUNT) return false;
    xSemaphoreTake(_netMutex, portMAX_DELAY);
    bool ok = false;
    if (_mqtt.connected()) {
        char topic[64];
        snprintf(topic, sizeof(topic), "%s%s/state", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[index]);

        char json[260];
        snprintf(json, sizeof(json),
                 "{\"location_id\":\"%s\",\"device_id\":\"%s\",\"locker_id\":\"%s\","
                 "\"locked\":%s,\"assigned\":%s,\"expired\":%s,"
                 "\"user_id\":\"%s\",\"method\":\"%s\"}",
                 Cfg::LOCATION_ID,
                 Cfg::DEVICE_ID,
                 Cfg::LOCKER_IDS[index],
                 locked ? "true" : "false",
                 assigned ? "true" : "false",
                 expired ? "true" : "false",
                 userId ? userId : "",
                 method ? method : "SYSTEM");

        ok = _mqtt.publish(topic, json, true);
    }
    xSemaphoreGive(_netMutex);
    return ok;
}

bool NetworkService::publishAck(uint8_t index, const String& payload) {
    if (!wifiConnected() || index >= Cfg::LOCKER_COUNT) return false;
    xSemaphoreTake(_netMutex, portMAX_DELAY);
    bool ok = false;
    if (_mqtt.connected()) {
        char topic[64];
        snprintf(topic, sizeof(topic), "%s%s/ack", Cfg::MQTT_BASE, Cfg::LOCKER_IDS[index]);
        ok = _mqtt.publish(topic, payload.c_str(), false);
    }
    xSemaphoreGive(_netMutex);
    return ok;
}

bool NetworkService::publishRfidRegAck(const String& userId, const String& uid,
                                       bool success, const char* reason) {
    if (!wifiConnected()) return false;
    xSemaphoreTake(_netMutex, portMAX_DELAY);
    bool ok = false;
    if (_mqtt.connected()) {
        char topic[64];
        snprintf(topic, sizeof(topic), "%susers/ack", Cfg::MQTT_BASE);

        char payload[128];
        snprintf(payload, sizeof(payload),
                 "RFID_REG_ACK|%s|%s|%s|%s",
                 userId.c_str(),
                 uid.c_str(),
                 success ? "ACCEPTED" : "REJECTED",
                 reason ? reason : "");

        ok = _mqtt.publish(topic, payload, false);
    }
    xSemaphoreGive(_netMutex);
    return ok;
}

bool NetworkService::publishUserSyncRequest(uint8_t userCount) {
    if (!wifiConnected()) return false;
    xSemaphoreTake(_netMutex, portMAX_DELAY);
    bool published = false;
    if (_mqtt.connected()) {
        char topic[64];
        snprintf(topic, sizeof(topic), "%susers/request", Cfg::MQTT_BASE);

        char json[160];
        snprintf(json, sizeof(json),
                 "{\"location_id\":\"%s\",\"device_id\":\"%s\",\"action\":\"SYNC_REQUEST\",\"user_count\":%u}",
                 Cfg::LOCATION_ID, Cfg::DEVICE_ID, userCount);

        published = _mqtt.publish(topic, json, false);
        if (published) _userSyncPending = false;
    }
    xSemaphoreGive(_netMutex);
    return published;
}
