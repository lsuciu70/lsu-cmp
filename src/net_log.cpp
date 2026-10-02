#include "net_log.h"

NetLog netLog;

void NetLog::begin(uint16_t port) {
  _server = new WiFiServer(port);
  _server->begin();
  _server->setNoDelay(true);
}

void NetLog::loop() {
  if (!_server || !_server->hasClient()) return;
  WiFiClient incoming = _server->available();
  xSemaphoreTake(_mutex, portMAX_DELAY);
  if (_client.connected()) {
    // One client at a time: the new connection replaces the old one.
    _client.stop();
  }
  _client = incoming;
  _client.setNoDelay(true);
  if (_full) _client.write(_buffer + _head, kBufferSize - _head);
  _client.write(_buffer, _head);
  xSemaphoreGive(_mutex);
}

size_t NetLog::write(const uint8_t* buf, size_t size) {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  Serial.write(buf, size);
  bufferAppend(buf, size);
  if (_client.connected()) _client.write(buf, size);
  xSemaphoreGive(_mutex);
  return size;
}

void NetLog::bufferAppend(const uint8_t* buf, size_t size) {
  while (size > 0) {
    size_t n = min(size, kBufferSize - _head);
    memcpy(_buffer + _head, buf, n);
    _head += n;
    buf += n;
    size -= n;
    if (_head == kBufferSize) {
      _head = 0;
      _full = true;
    }
  }
}
