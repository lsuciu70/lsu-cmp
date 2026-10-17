#include "cmp_encoder.h"

#include <string.h>

namespace cmp {

namespace {

void put16(uint8_t* p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v;
}

void put32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

void put64(uint8_t* p, uint64_t v) {
  put32(p, v >> 32);
  put32(p + 4, static_cast<uint32_t>(v));
}

}  // namespace

size_t encodeCanPacket(const StreamConfig& cfg, uint16_t sequenceCounter,
                       uint64_t timestampNs, uint8_t commonFlags,
                       const CanFrame& frame, uint8_t* out) {
  // Classic CAN: DLC 9..15 still means 8 data bytes; remote frames carry none.
  uint8_t dataLength = frame.rtr ? 0 : (frame.dlc > kMaxCanData ? kMaxCanData : frame.dlc);
  uint16_t payloadLength = kCanHeaderSize + dataLength;

  // CmpHeader
  uint8_t* h = out;
  h[0] = kVersion;
  h[1] = 0;  // reserved
  put16(h + 2, cfg.deviceId);
  h[4] = kMessageTypeData;
  h[5] = cfg.streamId;
  put16(h + 6, sequenceCounter);

  // MessageHeader
  uint8_t* m = out + kCmpHeaderSize;
  put64(m, timestampNs);
  put32(m + 8, cfg.interfaceId);
  m[12] = commonFlags;  // segment bits 0 = unsegmented
  m[13] = kPayloadTypeCan;
  put16(m + 14, payloadLength);

  // CAN payload header
  uint8_t* c = m + kMessageHeaderSize;
  put16(c, 0);      // flags: no error bits
  put16(c + 2, 0);  // reserved
  uint32_t id = frame.extended ? frame.id & kIdMask : (frame.id & 0x7FF) << kId11Shift;
  if (frame.rtr) id |= kIdRtr;
  if (frame.extended) id |= kIdIde;
  put32(c + 4, id);
  put32(c + 8, 0);   // crc + SBC: not available from TWAI, support bits 0
  put16(c + 12, 0);  // errorPosition
  c[14] = frame.dlc;
  c[15] = dataLength;
  memcpy(c + kCanHeaderSize, frame.data, dataLength);

  return kCmpHeaderSize + kMessageHeaderSize + payloadLength;
}

}  // namespace cmp
