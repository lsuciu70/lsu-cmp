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
// subscriber expires and sending stops.
constexpr uint32_t kSubscriberTimeoutMs = 5000;

// --- OBD2 requests (lsu_obd.h, SPEC.md §9) ---
// A REQUEST arriving sooner than this after the previous one is refused
// (busy). ECUs answer within 50 ms (ISO 15765-4 P2).
constexpr uint32_t kMinRequestIntervalMs = 50;
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
