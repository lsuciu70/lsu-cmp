#include "cmp_decoder.h"

#include <string.h>

namespace cmp {

namespace {

uint16_t get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t* p) { return uint32_t(get16(p)) << 16 | get16(p + 2); }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) << 32 | get32(p + 4); }

}  // namespace

bool decodeCanPacket(const uint8_t* packet, size_t size, DecodedCanPacket& out) {
  constexpr size_t kMinSize = kCmpHeaderSize + kMessageHeaderSize + kCanHeaderSize;
  if (size < kMinSize) return false;

  const uint8_t* h = packet;
  if (h[0] != kVersion || h[4] != kMessageTypeData) return false;

  const uint8_t* m = packet + kCmpHeaderSize;
  if (m[13] != kPayloadTypeCan) return false;
  uint16_t payloadLength = get16(m + 14);

  const uint8_t* c = m + kMessageHeaderSize;
  uint8_t dataLength = c[15];
  if (dataLength > kMaxCanData || payloadLength != kCanHeaderSize + dataLength ||
      size < kMinSize + dataLength) {
    return false;
  }

  out.deviceId = get16(h + 2);
  out.streamId = h[5];
  out.sequenceCounter = get16(h + 6);
  out.timestampNs = get64(m);
  out.interfaceId = get32(m + 8);
  out.commonFlags = m[12];
  out.canFlags = get16(c);
  uint32_t id = get32(c + 4);
  out.frame.id = id & kIdMask;
  out.frame.extended = (id & kIdIde) != 0;
  out.frame.rtr = (id & kIdRtr) != 0;
  out.frame.dlc = c[14];
  memset(out.frame.data, 0, sizeof(out.frame.data));
  memcpy(out.frame.data, c + kCanHeaderSize, dataLength);
  out.dataLength = dataLength;
  return true;
}

}  // namespace cmp
