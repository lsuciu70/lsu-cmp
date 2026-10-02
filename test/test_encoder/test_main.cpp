// Golden-byte tests for the CMP encoder. The expected bytes were also
// checked against packets built with openDAQ/ASAM-CMP-Library.
#include <cmp_encoder.h>
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
      0x00, 0x00, 0x07, 0xE8,                          // id
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

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_standard_frame);
  RUN_TEST(test_extended_rtr_frame);
  RUN_TEST(test_dlc_above_8_clamps_data);
  return UNITY_END();
}
