// ASAM CMP encoder for classic CAN data messages (SPEC.md §4).
//
// Layout verified against openDAQ/ASAM-CMP-Library (Apache-2.0) source:
// cmp_header.h, message_header.h, can_payload_base.{h,cpp}, payload_type.h.
// All multi-byte fields are big-endian. Platform independent (no Arduino
// headers) so it can be unit tested on the host (`pio test -e native`).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cmp {

constexpr size_t kCmpHeaderSize = 8;
constexpr size_t kMessageHeaderSize = 16;
constexpr size_t kCanHeaderSize = 16;
constexpr size_t kMaxCanData = 8;
constexpr size_t kMaxPacketSize =
    kCmpHeaderSize + kMessageHeaderSize + kCanHeaderSize + kMaxCanData;  // 48

constexpr uint8_t kVersion = 1;
constexpr uint8_t kMessageTypeData = 0x01;
// Raw 1-byte payload type. The library's 16-bit PayloadType::can (0x0101)
// packs messageType in the high byte; only the low byte goes on the wire.
constexpr uint8_t kPayloadTypeCan = 0x01;

// MessageHeader commonFlags
constexpr uint8_t kFlagRecalc = 0x01;
constexpr uint8_t kFlagInsync = 0x02;
constexpr uint8_t kFlagOverflow = 0x20;
constexpr uint8_t kFlagErrorInPayload = 0x40;

// CAN payload `id` field, as a big-endian 32-bit value on the wire. The
// library writes these masks on the raw (byte-swapped) word of a
// little-endian host: idMask 0xFFFFFF1F, rsvd 0x20, rtr 0x40, ide 0x80.
// Translated to wire order: ID in bits 0..28, then rsvd, RTR/RRS, IDE.
constexpr uint32_t kIdMask = 0x1FFFFFFF;
constexpr uint32_t kIdRsvd = 1u << 29;
constexpr uint32_t kIdRtr = 1u << 30;
constexpr uint32_t kIdIde = 1u << 31;

struct CanFrame {
  uint32_t id;      // 11 or 29 bit identifier
  bool extended;    // IDE: 29-bit ID
  bool rtr;         // remote frame
  uint8_t dlc;      // raw DLC as received (0..15)
  uint8_t data[kMaxCanData];
};

struct StreamConfig {
  uint16_t deviceId;
  uint8_t streamId;
  uint32_t interfaceId;
};

// Writes one CMP packet (CmpHeader + MessageHeader + CAN payload) for
// `frame` into `out` (at least kMaxPacketSize bytes). Returns its length
// (40..48).
//
// The CAN payload's error flags, errorPosition and crc field are 0: TWAI
// delivers only valid frames and does not expose the received CRC or stuff
// bit count, so crcSupport/sbcSupport stay 0 ("not supported") rather than
// carrying invented values.
size_t encodeCanPacket(const StreamConfig& cfg, uint16_t sequenceCounter,
                       uint64_t timestampNs, uint8_t commonFlags,
                       const CanFrame& frame, uint8_t* out);

}  // namespace cmp
