// OBD2 requests the ESP32 may put on the CAN bus, and the ISO-TP flow
// control it answers multi-frame responses with (SPEC.md §9). Shared by the
// firmware and the host tests; platform independent.
//
// Only standard OBD2 (SAE J1979 / ISO 15765-4, 11-bit IDs, 500 kbit/s):
//   request    0x7DF (functional, all emission-related ECUs)
//   responses  0x7E8..0x7EF (ECU n answers on 0x7E8 + n)
//   flow ctrl  0x7E0..0x7E7 (physical address of ECU n, 0x7E0 + n)
// Allowed: mode 01 (current data) and mode 09 (vehicle information), any
// PID. Both only read; nothing that clears codes, writes or resets.
#pragma once

#include <stdint.h>
#include <string.h>

namespace obd {

constexpr uint32_t kFunctionalRequestId = 0x7DF;
constexpr uint32_t kFirstResponseId = 0x7E8;
constexpr uint32_t kLastResponseId = 0x7EF;
constexpr uint32_t kResponseToPhysical = 8;  // 0x7E8 + n -> 0x7E0 + n

constexpr uint8_t kModeCurrentData = 0x01;
constexpr uint8_t kModeVehicleInfo = 0x09;
constexpr uint8_t kPositiveResponse = 0x40;  // added to the mode
constexpr uint8_t kNegativeResponse = 0x7F;

// ISO 15765-4 requires DLC 8; the value of unused bytes is not specified
// and receivers ignore it. 0xAA: what the Golf 7 engine ECU pads its own
// answers with (in-car test, 2026-10-17), not taken from any VW document.
constexpr uint8_t kPadding = 0xAA;

// ISO-TP protocol control information, high nibble of byte 0.
constexpr uint8_t kPciSingle = 0x0;
constexpr uint8_t kPciFirst = 0x1;
constexpr uint8_t kPciConsecutive = 0x2;
constexpr uint8_t kPciFlowControl = 0x3;

inline bool isAllowedRequest(uint8_t mode, uint8_t /*pid*/) {
  return mode == kModeCurrentData || mode == kModeVehicleInfo;
}

// Single frame: length 2, mode, PID, padding.
inline void buildRequest(uint8_t mode, uint8_t pid, uint8_t data[8]) {
  memset(data, kPadding, 8);
  data[0] = (kPciSingle << 4) | 2;
  data[1] = mode;
  data[2] = pid;
}

inline bool isResponseId(uint32_t id, bool extended) {
  return !extended && id >= kFirstResponseId && id <= kLastResponseId;
}

// First frame of a multi-frame response (e.g. the VIN, mode 09 PID 02),
// which the ECU only continues after a flow control from the tester.
inline bool isFirstFrame(uint32_t id, bool extended, uint8_t dlc, const uint8_t* data) {
  return isResponseId(id, extended) && dlc >= 2 && (data[0] >> 4) == kPciFirst;
}

inline uint32_t flowControlId(uint32_t responseId) { return responseId - kResponseToPhysical; }

// Flow control "continue to send": block size 0 (no further flow control),
// STmin 0 (consecutive frames back to back).
inline void buildFlowControl(uint8_t data[8]) {
  memset(data, kPadding, 8);
  data[0] = kPciFlowControl << 4;
  data[1] = 0;
  data[2] = 0;
}

}  // namespace obd
