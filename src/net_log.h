// Log output to Serial (USB) and to one TCP client, so the log can be read
// over WiFi with: pio device monitor --port socket://<ip>:23
//
// The last kNetLogBufferSize bytes are kept and replayed to a client when it
// connects, so boot messages written before the connection are not lost.
// Messages printed by libraries straight to Serial (LsuWifi) only go to USB.
#pragma once

#include <Arduino.h>
#include <WiFi.h>

class NetLog : public Print {
 public:
  void begin(uint16_t port);  // after the network is up
  void loop();                // accepts a client; call from loop()

  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t size) override;

 private:
  static constexpr size_t kBufferSize = 4096;

  void bufferAppend(const uint8_t* buf, size_t size);

  SemaphoreHandle_t _mutex = xSemaphoreCreateMutex();
  WiFiServer* _server = nullptr;
  WiFiClient _client;
  uint8_t _buffer[kBufferSize];
  size_t _head = 0;   // next write position
  bool _full = false; // buffer has wrapped
};

extern NetLog netLog;
