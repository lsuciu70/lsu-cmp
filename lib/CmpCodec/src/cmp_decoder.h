// ASAM CMP decoder for classic CAN data messages: the inverse of
// cmp_encoder.h, for the receiving side (tools/cmp_client and later clients).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cmp_encoder.h"

namespace cmp {

struct DecodedCanPacket {
  // CmpHeader
  uint16_t deviceId;
  uint8_t streamId;
  uint16_t sequenceCounter;
  // MessageHeader
  uint64_t timestampNs;
  uint32_t interfaceId;
  uint8_t commonFlags;
  // CAN payload
  uint16_t canFlags;   // error bits, 0 for frames from TWAI
  CanFrame frame;      // id, extended, rtr, raw dlc, data
  uint8_t dataLength;  // bytes of frame.data actually carried
};

// Decodes one packet holding a single CAN data message. Returns false if it
// is not a CMP v1 data message with a CAN payload, or is truncated.
bool decodeCanPacket(const uint8_t* packet, size_t size, DecodedCanPacket& out);

}  // namespace cmp
