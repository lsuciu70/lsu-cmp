// Subscribes to an lsu-cmp ESP32 and delivers its CAN frames and status as
// callbacks. The reusable part of the client: no output of its own, no
// threads. POSIX sockets (Linux).
//
// Typical use:
//   CmpSubscriber sub(callbacks);
//   if (!sub.open("192.168.100.240", lsu::kDefaultPort, &err)) ...
//   while (running) sub.poll(100);
//   sub.close();  // sends UNSUBSCRIBE (also done by the destructor)
//
// poll() does all the work in the calling thread: it resends SUBSCRIBE
// (with this machine's time, which keeps the ESP32 clock in sync) every
// subscribeIntervalMs, waits up to timeoutMs for packets and dispatches them.
// To drive it from another event loop, wait for fd() to become readable and
// call poll(0).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "cmp_decoder.h"
#include "lsu_protocol.h"

class CmpSubscriber {
 public:
  struct Callbacks {
    // One CAN frame (one CMP packet), in arrival order.
    std::function<void(const cmp::DecodedCanPacket&)> onFrame;
    // sequenceCounter jumped: `lost` packets were not received.
    std::function<void(unsigned lost)> onGap;
    // STATUS reply to a SUBSCRIBE, about once per subscribeIntervalMs.
    std::function<void(const lsu::Status&)> onStatus;
    // No STATUS for statusTimeoutMs (also at start, if the ESP32 never
    // answers), and the first STATUS after that.
    std::function<void()> onUnreachable;
    std::function<void()> onReachable;
    // Neither a CMP CAN packet nor a valid STATUS.
    std::function<void(const uint8_t* data, size_t size)> onUnknownPacket;
  };

  struct Options {
    int subscribeIntervalMs = 1000;  // the ESP32 drops a subscriber after 5 s
    int statusTimeoutMs = 3000;
  };

  struct Counters {
    uint64_t frames = 0;
    uint64_t lost = 0;     // from sequenceCounter gaps
    uint64_t unknown = 0;  // packets passed to onUnknownPacket
  };

  explicit CmpSubscriber(Callbacks callbacks);
  CmpSubscriber(Callbacks callbacks, Options options);
  ~CmpSubscriber();
  CmpSubscriber(const CmpSubscriber&) = delete;
  CmpSubscriber& operator=(const CmpSubscriber&) = delete;

  // Resolves host and opens the socket. SUBSCRIBE goes out on the first
  // poll(). On failure returns false and sets *error if given.
  bool open(const std::string& host, uint16_t port, std::string* error = nullptr);
  bool poll(int timeoutMs);  // false if not open
  void close();              // sends UNSUBSCRIBE, closes the socket

  int fd() const { return _sock; }
  bool reachable() const { return _reachable; }
  bool hasStatus() const { return _hasStatus; }
  const lsu::Status& lastStatus() const { return _lastStatus; }
  const Counters& counters() const { return _counters; }

 private:
  void sendSubscribe(int64_t nowMs);
  void receive();
  void handlePacket(const uint8_t* data, size_t size);
  void checkTimeout(int64_t nowMs);

  Callbacks _cb;
  Options _opt;
  int _sock = -1;
  int64_t _nextSubscribeMs = 0;
  int64_t _lastStatusMs = 0;  // or open() time, before the first STATUS
  bool _reachable = true;     // until statusTimeoutMs passes without STATUS
  bool _hasStatus = false;
  lsu::Status _lastStatus{};
  bool _haveSeq = false;
  uint16_t _expectedSeq = 0;
  Counters _counters;
};
