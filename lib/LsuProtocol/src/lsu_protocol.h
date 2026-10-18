// Own subscription protocol between the laptop and the ESP32 (NOT part of
// ASAM CMP). Shared by the firmware and tools/cmp_client. All multi-byte
// fields are big-endian.
//
//   laptop -> ESP32  SUBSCRIBE    "LSUS" + uint64 laptop time, ns since epoch
//   laptop -> ESP32  UNSUBSCRIBE  "LSUU"
//   ESP32 -> laptop  STATUS       reply to every SUBSCRIBE (layout below)
//   laptop -> ESP32  REQUEST      "LSUR" + OBD2 mode + PID: put one OBD2
//                                 request on the bus (lsu_obd.h, SPEC.md §9)
//   ESP32 -> laptop  REQUEST_RESULT  "LSUK" + mode + PID + RequestResult,
//                                 reply to every REQUEST
//   laptop -> ESP32  WATCH        "LSUW" + count + count x (mode, PID): the
//                                 mode 01 PIDs this client wants polled;
//                                 resent every kWatchRenewMs, replaces the
//                                 client's previous list, no reply
//
// REQUEST is for one-off requests (e.g. the VIN). For live values a client
// sends WATCH instead: the ESP32 polls the union of all clients' lists on
// the bus itself, at its own steady rate, and drops a client's list
// kWatchTimeoutMs after its last WATCH (SPEC.md §9).
//
// The answers of the ECUs, and the request itself, arrive as ordinary CAN
// frames in the CMP stream.
//
// Two UDP ports on the ESP32: all messages above go to and come from the
// control port; the CMP packets come from the data port, to the address
// the SUBSCRIBE came from. So the data port carries nothing but CMP and
// Wireshark decodes it cleanly (until 2026-10-17 everything shared 12670).
// STATUS and REQUEST_RESULT start with "LSU" and CMP packets with version
// 0x01, so the content alone also tells them apart (old recordings).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace lsu {

constexpr uint16_t kDataPort = 12670;     // CMP packets, ESP32 -> laptop
constexpr uint16_t kControlPort = 12671;  // this protocol, both ways

constexpr uint8_t kMagicSubscribe[4] = {'L', 'S', 'U', 'S'};
constexpr uint8_t kMagicUnsubscribe[4] = {'L', 'S', 'U', 'U'};
constexpr uint8_t kMagicStatus[4] = {'L', 'S', 'U', 'A'};
constexpr uint8_t kMagicRequest[4] = {'L', 'S', 'U', 'R'};
constexpr uint8_t kMagicRequestResult[4] = {'L', 'S', 'U', 'K'};
constexpr uint8_t kMagicWatch[4] = {'L', 'S', 'U', 'W'};

constexpr size_t kSubscribeSize = 12;
constexpr size_t kUnsubscribeSize = 4;
constexpr size_t kStatusSize = 40;
constexpr uint8_t kStatusVersion = 1;
constexpr size_t kRequestSize = 6;
constexpr size_t kRequestResultSize = 7;
constexpr int kMaxWatch = 16;                        // (mode, PID) pairs per WATCH
constexpr size_t kWatchMaxSize = 5 + 2 * kMaxWatch;  // 37
constexpr uint32_t kWatchRenewMs = 4000;             // client resends WATCH
constexpr uint32_t kWatchTimeoutMs = 10000;          // ESP32 drops the list

enum RequestResult : uint8_t {
  kRequestSent = 0,           // accepted: queued for the bus (or already queued)
  kRequestNotAllowed = 1,     // not in the whitelist (lsu_obd.h)
  kRequestNotSubscribed = 2,  // only the current subscriber may request
  kRequestBusy = 3,           // request queue full
  kRequestTxDisabled = 4,     // firmware built listen-only (CAN_ACK=0)
  kRequestTxFailed = 5,       // TWAI refused it (not returned since requests are queued)
};

inline const char* requestResultName(uint8_t r) {
  static const char* kNames[] = {"sent", "not allowed", "not subscribed",
                                 "busy", "transmit disabled", "transmit failed"};
  return r <= kRequestTxFailed ? kNames[r] : "?";
}

// Same values as twai_state_t.
enum CanState : uint8_t { kCanStopped = 0, kCanRunning = 1, kCanBusOff = 2, kCanRecovering = 3 };

inline const char* canStateName(uint8_t s) {
  static const char* kNames[] = {"stopped", "running", "bus-off", "recovering"};
  return s <= kCanRecovering ? kNames[s] : "?";
}

// STATUS layout:
//   0  4  "LSUA"
//   4  1  version (1)
//   5  1  CAN state (CanState)
//   6  2  CAN bitrate, kbit/s
//   8  4  CAN frames received since boot
//  12  4  CMP packets sent since boot
//  16  4  frames dropped, internal queue full
//  20  4  UDP send errors
//  24  4  TWAI bus errors
//  28  4  TWAI frames missed (driver RX queue full)
//  32  4  TWAI frames lost to RX FIFO overrun
//  36  4  uptime, s
struct Status {
  uint8_t canState;
  uint16_t bitrateKbps;
  uint32_t canRx;
  uint32_t sent;
  uint32_t queueFull;
  uint32_t sendErrors;
  uint32_t busErrors;
  uint32_t rxMissed;
  uint32_t rxOverrun;
  uint32_t uptimeS;
};

namespace detail {

inline void put16(uint8_t* p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v;
}

inline void put32(uint8_t* p, uint32_t v) {
  put16(p, v >> 16);
  put16(p + 2, v);
}

inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t get32(const uint8_t* p) { return uint32_t(get16(p)) << 16 | get16(p + 2); }

}  // namespace detail

inline void encodeSubscribe(uint64_t laptopTimeNs, uint8_t* out) {
  memcpy(out, kMagicSubscribe, 4);
  detail::put32(out + 4, uint32_t(laptopTimeNs >> 32));
  detail::put32(out + 8, uint32_t(laptopTimeNs));
}

inline bool decodeSubscribe(const uint8_t* p, size_t n, uint64_t& laptopTimeNs) {
  if (n != kSubscribeSize || memcmp(p, kMagicSubscribe, 4) != 0) return false;
  laptopTimeNs = uint64_t(detail::get32(p + 4)) << 32 | detail::get32(p + 8);
  return true;
}

inline bool isUnsubscribe(const uint8_t* p, size_t n) {
  return n == kUnsubscribeSize && memcmp(p, kMagicUnsubscribe, 4) == 0;
}

inline void encodeStatus(const Status& s, uint8_t* out) {
  using namespace detail;
  memcpy(out, kMagicStatus, 4);
  out[4] = kStatusVersion;
  out[5] = s.canState;
  put16(out + 6, s.bitrateKbps);
  put32(out + 8, s.canRx);
  put32(out + 12, s.sent);
  put32(out + 16, s.queueFull);
  put32(out + 20, s.sendErrors);
  put32(out + 24, s.busErrors);
  put32(out + 28, s.rxMissed);
  put32(out + 32, s.rxOverrun);
  put32(out + 36, s.uptimeS);
}

inline void encodeRequest(uint8_t mode, uint8_t pid, uint8_t* out) {
  memcpy(out, kMagicRequest, 4);
  out[4] = mode;
  out[5] = pid;
}

inline bool decodeRequest(const uint8_t* p, size_t n, uint8_t& mode, uint8_t& pid) {
  if (n != kRequestSize || memcmp(p, kMagicRequest, 4) != 0) return false;
  mode = p[4];
  pid = p[5];
  return true;
}

inline void encodeRequestResult(uint8_t mode, uint8_t pid, uint8_t result, uint8_t* out) {
  memcpy(out, kMagicRequestResult, 4);
  out[4] = mode;
  out[5] = pid;
  out[6] = result;
}

inline bool decodeRequestResult(const uint8_t* p, size_t n, uint8_t& mode, uint8_t& pid,
                                uint8_t& result) {
  if (n != kRequestResultSize || memcmp(p, kMagicRequestResult, 4) != 0) return false;
  mode = p[4];
  pid = p[5];
  result = p[6];
  return true;
}

struct WatchEntry {
  uint8_t mode, pid;
};

// Writes WATCH for up to kMaxWatch entries; returns its size.
inline size_t encodeWatch(const WatchEntry* entries, int count, uint8_t* out) {
  if (count > kMaxWatch) count = kMaxWatch;
  memcpy(out, kMagicWatch, 4);
  out[4] = uint8_t(count);
  for (int i = 0; i < count; i++) {
    out[5 + 2 * i] = entries[i].mode;
    out[6 + 2 * i] = entries[i].pid;
  }
  return 5 + 2 * size_t(count);
}

// Reads WATCH into out[kMaxWatch]; false if malformed.
inline bool decodeWatch(const uint8_t* p, size_t n, WatchEntry* out, int& count) {
  if (n < 5 || memcmp(p, kMagicWatch, 4) != 0) return false;
  count = p[4];
  if (count > kMaxWatch || n != 5 + 2 * size_t(count)) return false;
  for (int i = 0; i < count; i++) out[i] = {p[5 + 2 * i], p[6 + 2 * i]};
  return true;
}

inline bool isStatus(const uint8_t* p, size_t n) {
  return n >= 4 && memcmp(p, kMagicStatus, 4) == 0;
}

inline bool decodeStatus(const uint8_t* p, size_t n, Status& s) {
  using namespace detail;
  if (n != kStatusSize || !isStatus(p, n) || p[4] != kStatusVersion) return false;
  s.canState = p[5];
  s.bitrateKbps = get16(p + 6);
  s.canRx = get32(p + 8);
  s.sent = get32(p + 12);
  s.queueFull = get32(p + 16);
  s.sendErrors = get32(p + 20);
  s.busErrors = get32(p + 24);
  s.rxMissed = get32(p + 28);
  s.rxOverrun = get32(p + 32);
  s.uptimeS = get32(p + 36);
  return true;
}

}  // namespace lsu
