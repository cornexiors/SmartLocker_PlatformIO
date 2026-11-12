#include "App.h"

bool Storage::begin() {
  if (!_prefs.begin("lockerdata", false)) return false;
  if (!_queue.begin("lockersync", false)) { _prefs.end(); return false; }
  _head = _queue.getUChar("head", 0) % Cfg::SYNC_QUEUE_CAPACITY;
  _tail = _queue.getUChar("tail", 0) % Cfg::SYNC_QUEUE_CAPACITY;
  // SD is optional; the device remains fully functional without a card.
  _sdReady = SD.begin(Cfg::SD_CS, SPI, Cfg::SD_SPI_HZ);
  return true;
}

void Storage::load(LockerRecord records[Cfg::LOCKER_COUNT]) {
  for (uint8_t i = 0; i < Cfg::LOCKER_COUNT; ++i) {
    char key[4]; snprintf(key, sizeof(key), "L%u", i);
    if (_prefs.getBytesLength(key) == sizeof(LockerRecord))
      _prefs.getBytes(key, &records[i], sizeof(LockerRecord));
    else records[i] = LockerRecord{};
  }
}

void Storage::save(uint8_t index, const LockerRecord& record) {
  if (index >= Cfg::LOCKER_COUNT) return;
  char key[4]; snprintf(key, sizeof(key), "L%u", index);
  _prefs.putBytes(key, &record, sizeof(record));
}

bool Storage::enqueue(const String& payload) {
  const uint8_t next = static_cast<uint8_t>((_tail + 1) % Cfg::SYNC_QUEUE_CAPACITY);
  if (next == _head) return false;
  char key[5]; snprintf(key, sizeof(key), "Q%02u", _tail);
  if (_queue.putString(key, payload) == 0) return false;
  _tail = next;
  _queue.putUChar("tail", _tail);
  return true;
}

bool Storage::peek(String& payload) {
  if (_head == _tail) return false;
  char key[5]; snprintf(key, sizeof(key), "Q%02u", _head);
  payload = _queue.getString(key, "");
  return !payload.isEmpty();
}

void Storage::pop() {
  if (_head == _tail) return;
  char key[5]; snprintf(key, sizeof(key), "Q%02u", _head);
  _queue.remove(key);
  _head = static_cast<uint8_t>((_head + 1) % Cfg::SYNC_QUEUE_CAPACITY);
  _queue.putUChar("head", _head);
}

void Storage::logEvent(const String& payload) {
  if (!_sdReady) return;
  File file = SD.open("/locker.log", FILE_APPEND);
  if (!file) return;
  file.println(payload);
  file.close();
}
