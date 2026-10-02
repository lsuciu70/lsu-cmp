// Project configuration. Decisions from SPEC.md §6, recorded in README.md.
#pragma once

#include <stdint.h>

// --- CAN / TWAI (SPEC.md §2, §3) ---
constexpr int kCanTxGpio = 1;  // WCMCU-230 CTX (unused: listen-only)
constexpr int kCanRxGpio = 3;  // WCMCU-230 CRX
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
// Arbitrary port, NOT an IANA-registered CMP port (none is known). Used both
// for incoming subscriptions and as the source port of the CMP packets.
// In Wireshark: Decode As... UDP port 12670 -> ASAM CMP.
constexpr uint16_t kUdpPort = 12670;

// --- Subscription protocol (own protocol, not part of ASAM CMP) ---
// The PC resends SUBSCRIBE every second; without one for this long the
// subscriber expires and sending stops.
constexpr uint32_t kSubscriberTimeoutMs = 5000;

// --- OTA (ArduinoOTA, works in station and AP mode) ---
// Optional password: build_flags = -DOTA_PASSWORD=\"...\"
constexpr const char* kOtaHostname = "lsu-cmp";

// --- TCP log (net_log.h): pio device monitor --port socket://<ip>:23 ---
constexpr uint16_t kLogTcpPort = 23;

// --- Buffers ---
constexpr uint32_t kTwaiRxQueueLen = 64;   // TWAI driver queue
constexpr uint32_t kFrameQueueLen = 256;   // capture task -> sender task
constexpr uint32_t kStatsIntervalMs = 5000;
