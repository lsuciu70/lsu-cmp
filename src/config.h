// Project configuration. Decisions from SPEC.md §6, recorded in README.md.
#pragma once

#include <stdint.h>

#include "lsu_protocol.h"

// --- CAN / TWAI (SPEC.md §2, §3) ---
constexpr int kCanTxGpio = 1;  // WCMCU-230 CTX (see CAN_ACK)
constexpr int kCanRxGpio = 3;  // WCMCU-230 CRX
// 1: TWAI normal mode. The controller ACKs every valid frame and sends
//    error frames; the firmware transmits only OBD2 requests and their
//    flow control (SPEC.md §9).
//    Needed on the Golf 7 OBD2 bus: without any ACKing node the sender
//    retransmits the same frame back to back (in-car test, 2026-10-17).
// 0: listen-only, the node never drives the bus.
// Normal mode is only safe with the right bitrate: a wrong one makes this
// node send error frames on the live bus.
#ifndef CAN_ACK
#define CAN_ACK 1
#endif
// 500 kbit/s first (ISO 15765-4). If the stats log shows many bus errors,
// try 250.
#ifndef CAN_BITRATE_KBPS
#define CAN_BITRATE_KBPS 500
#endif

// --- ASAM CMP identifiers ---
// Chosen locally for this project, NOT taken from any official ASAM registry
// (there is none for hobby projects).
constexpr uint16_t kCmpDeviceId = 1;
constexpr uint8_t kCmpStreamId = 1;
// Arbitrary fixed value while there is a single physical CAN channel.
constexpr uint32_t kCmpInterfaceId = 0;

// --- UDP ---
// Arbitrary ports, NOT IANA-registered (none is known for CMP). The data
// port is only the source port of the CMP packets; in Wireshark: Decode
// As... UDP port 12670 -> ASAM CMP. The control port carries the own
// protocol (lsu_protocol.h).
constexpr uint16_t kDataPort = lsu::kDataPort;        // 12670
constexpr uint16_t kControlPort = lsu::kControlPort;  // 12671

// --- Subscription protocol (own protocol, not part of ASAM CMP) ---
// The PC resends SUBSCRIBE every second; without one for this long the
// subscriber expires and sending to it stops.
constexpr uint32_t kSubscriberTimeoutMs = 5000;
// Clients served at once (e.g. the phone app and a laptop recording); each
// gets every CMP packet. A SUBSCRIBE from a new client is ignored while all
// slots are taken.
constexpr int kMaxSubscribers = 4;

// --- OBD2 requests (lsu_obd.h, SPEC.md §9) ---
// One request goes on the bus at most every kMinRequestIntervalMs, for all
// subscribers together: first the queued one-off REQUESTs, otherwise the
// next PID of the watched list (union of the clients' WATCH lists, round
// robin). The Golf 7 engine ECU answers in 3-6 ms (ISO 15765-4 allows up
// to 50 ms).
constexpr uint32_t kMinRequestIntervalMs = 20;
// One-off REQUESTs wait here; a duplicate is not queued twice, a full
// queue answers busy (WiFi delivers a client's REQUESTs in bunches).
constexpr int kRequestQueueLen = 8;
// No answer from any ECU for kEcuSilentMs (ignition off): poll the watched
// list only one PID per kSlowPollMs, so the board does not fire 50
// requests/s into a sleeping bus. Full rate again at the first answer.
constexpr uint32_t kEcuSilentMs = 2000;
constexpr uint32_t kSlowPollMs = 1000;
// A first frame on 0x7E8..0x7EF gets a flow control only this long after
// our last request, so we never answer for another tester on the bus.
constexpr uint32_t kFlowControlWindowMs = 1000;

// --- OTA (ArduinoOTA, works in station and AP mode) ---
// Optional password: build_flags = -DOTA_PASSWORD=\"...\"
constexpr const char* kOtaHostname = "lsu-cmp";

// --- TCP log (net_log.h): pio device monitor --port socket://<ip>:23 ---
constexpr uint16_t kLogTcpPort = 23;

// --- Buffers ---
constexpr uint32_t kTwaiRxQueueLen = 64;   // TWAI driver queue
constexpr uint32_t kFrameQueueLen = 256;   // capture task -> sender task
constexpr uint32_t kStatsIntervalMs = 5000;
