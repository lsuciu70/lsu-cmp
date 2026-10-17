// Golden-byte tests for the CMP encoder (the expected bytes were also
// checked against packets built with openDAQ/ASAM-CMP-Library, except the
// 11-bit ID position, see cmp_encoder.h), and
// encode -> decode round trips for the decoder.
#include <cmp_decoder.h>
#include <cmp_encoder.h>
#include <string.h>
#include <unity.h>

namespace {

const cmp::StreamConfig kStream = {1, 1, 0};

void test_standard_frame() {
  cmp::CanFrame f = {0x7E8, false, false, 8, {0x10, 0x14, 0x49, 0x02, 0x01, 0x57, 0x56, 0x57}};
  uint8_t out[cmp::kMaxPacketSize];
  size_t n = cmp::encodeCanPacket(kStream, 0x1234, 0x0102030405060708ULL,
                                  cmp::kFlagInsync, f, out);
  const uint8_t expected[] = {
      // CmpHeader
      0x01, 0x00, 0x00, 0x01, 0x01, 0x01, 0x12, 0x34,
      // MessageHeader
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // timestamp
      0x00, 0x00, 0x00, 0x00,                          // interfaceId
      0x02, 0x01, 0x00, 0x18,                          // flags, type, length 24
      // CAN payload
      0x00, 0x00, 0x00, 0x00,                          // flags, reserved
      0x1F, 0xA0, 0x00, 0x00,                          // id 0x7E8 << 18
      0x00, 0x00, 0x00, 0x00,                          // crc
      0x00, 0x00, 0x08, 0x08,                          // errorPos, dlc, len
      0x10, 0x14, 0x49, 0x02, 0x01, 0x57, 0x56, 0x57};
  TEST_ASSERT_EQUAL(48, n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(expected));
}

void test_extended_rtr_frame() {
  cmp::CanFrame f = {0x18DAF110, true, true, 3, {0xAA, 0xBB, 0xCC}};
  uint8_t out[cmp::kMaxPacketSize];
  size_t n = cmp::encodeCanPacket(kStream, 0xFFFF, 0, cmp::kFlagInsync | cmp::kFlagOverflow, f, out);
  TEST_ASSERT_EQUAL(40, n);
  TEST_ASSERT_EQUAL_HEX8(0x22, out[20]);  // insync | overflow
  TEST_ASSERT_EQUAL_HEX8(0x10, out[23]);  // payload length 16
  const uint8_t id[] = {0xD8, 0xDA, 0xF1, 0x10};  // IDE | RTR | 0x18DAF110
  TEST_ASSERT_EQUAL_HEX8_ARRAY(id, out + 28, 4);
  TEST_ASSERT_EQUAL_HEX8(3, out[38]);  // raw DLC kept
  TEST_ASSERT_EQUAL_HEX8(0, out[39]);  // no data in a remote frame
}

void test_dlc_above_8_clamps_data() {
  cmp::CanFrame f = {0x123, false, false, 15, {1, 2, 3, 4, 5, 6, 7, 8}};
  uint8_t out[cmp::kMaxPacketSize];
  size_t n = cmp::encodeCanPacket(kStream, 0, 0, 0, f, out);
  TEST_ASSERT_EQUAL(48, n);
  TEST_ASSERT_EQUAL_HEX8(15, out[38]);
  TEST_ASSERT_EQUAL_HEX8(8, out[39]);
}

void assertRoundTrip(const cmp::CanFrame& f, uint8_t expectedDataLength) {
  const cmp::StreamConfig stream = {0xBEEF, 7, 0x01020304};
  uint8_t buf[cmp::kMaxPacketSize];
  size_t n = cmp::encodeCanPacket(stream, 0xABCD, 0x1122334455667788ULL,
                                  cmp::kFlagInsync | cmp::kFlagOverflow, f, buf);
  cmp::DecodedCanPacket d;
  TEST_ASSERT_TRUE(cmp::decodeCanPacket(buf, n, d));
  TEST_ASSERT_EQUAL_HEX16(0xBEEF, d.deviceId);
  TEST_ASSERT_EQUAL(7, d.streamId);
  TEST_ASSERT_EQUAL_HEX16(0xABCD, d.sequenceCounter);
  TEST_ASSERT_EQUAL_UINT64(0x1122334455667788ULL, d.timestampNs);
  TEST_ASSERT_EQUAL_HEX32(0x01020304, d.interfaceId);
  TEST_ASSERT_EQUAL_HEX8(cmp::kFlagInsync | cmp::kFlagOverflow, d.commonFlags);
  TEST_ASSERT_EQUAL_HEX16(0, d.canFlags);
  TEST_ASSERT_EQUAL_HEX32(f.id, d.frame.id);
  TEST_ASSERT_EQUAL(f.extended, d.frame.extended);
  TEST_ASSERT_EQUAL(f.rtr, d.frame.rtr);
  TEST_ASSERT_EQUAL(f.dlc, d.frame.dlc);
  TEST_ASSERT_EQUAL(expectedDataLength, d.dataLength);
  if (expectedDataLength) TEST_ASSERT_EQUAL_HEX8_ARRAY(f.data, d.frame.data, expectedDataLength);
}

void test_decode_round_trip() {
  assertRoundTrip({0x7E8, false, false, 8, {1, 2, 3, 4, 5, 6, 7, 8}}, 8);
  assertRoundTrip({0x18DAF110, true, false, 3, {0xAA, 0xBB, 0xCC}}, 3);
  assertRoundTrip({0x1FFFFFFF, true, true, 8, {}}, 0);
  assertRoundTrip({0x000, false, false, 0, {}}, 0);
  assertRoundTrip({0x123, false, false, 15, {1, 2, 3, 4, 5, 6, 7, 8}}, 8);
}

// Recordings made before 2026-10-17 carry 11-bit IDs in bits 0..10.
void test_decode_old_11bit_encoding() {
  cmp::CanFrame f = {0x7E8, false, false, 8, {1, 2, 3, 4, 5, 6, 7, 8}};
  uint8_t buf[cmp::kMaxPacketSize];
  size_t n = cmp::encodeCanPacket(kStream, 0, 0, 0, f, buf);
  const uint8_t oldId[] = {0x00, 0x00, 0x07, 0xE8};
  memcpy(buf + 28, oldId, 4);
  cmp::DecodedCanPacket d;
  TEST_ASSERT_TRUE(cmp::decodeCanPacket(buf, n, d));
  TEST_ASSERT_EQUAL_HEX32(0x7E8, d.frame.id);
  TEST_ASSERT_FALSE(d.frame.extended);
}

void test_decode_rejects_invalid() {
  cmp::CanFrame f = {0x7E8, false, false, 8, {1, 2, 3, 4, 5, 6, 7, 8}};
  uint8_t good[cmp::kMaxPacketSize];
  size_t n = cmp::encodeCanPacket(kStream, 0, 0, 0, f, good);
  uint8_t buf[cmp::kMaxPacketSize];
  cmp::DecodedCanPacket d;

  TEST_ASSERT_FALSE(cmp::decodeCanPacket(good, n - 1, d));  // truncated data
  TEST_ASSERT_FALSE(cmp::decodeCanPacket(good, 39, d));     // truncated header

  memcpy(buf, good, n);
  buf[0] = 2;  // version
  TEST_ASSERT_FALSE(cmp::decodeCanPacket(buf, n, d));

  memcpy(buf, good, n);
  buf[4] = 0x03;  // status message
  TEST_ASSERT_FALSE(cmp::decodeCanPacket(buf, n, d));

  memcpy(buf, good, n);
  buf[21] = 0x02;  // payload type CAN FD
  TEST_ASSERT_FALSE(cmp::decodeCanPacket(buf, n, d));

  memcpy(buf, good, n);
  buf[39] = 9;  // dataLength > 8
  TEST_ASSERT_FALSE(cmp::decodeCanPacket(buf, n, d));

  memcpy(buf, good, n);
  buf[23] = 0x17;  // payloadLength disagrees with dataLength
  TEST_ASSERT_FALSE(cmp::decodeCanPacket(buf, n, d));

  const uint8_t status[] = {'L', 'S', 'U', 'A'};
  TEST_ASSERT_FALSE(cmp::decodeCanPacket(status, sizeof(status), d));
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_standard_frame);
  RUN_TEST(test_extended_rtr_frame);
  RUN_TEST(test_dlc_above_8_clamps_data);
  RUN_TEST(test_decode_round_trip);
  RUN_TEST(test_decode_old_11bit_encoding);
  RUN_TEST(test_decode_rejects_invalid);
  return UNITY_END();
}
