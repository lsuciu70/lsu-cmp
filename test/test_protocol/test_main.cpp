// Round-trip and layout tests for the subscription protocol.
#include <lsu_protocol.h>
#include <unity.h>

namespace {

void test_subscribe_round_trip() {
  uint8_t buf[lsu::kSubscribeSize];
  lsu::encodeSubscribe(0x0102030405060708ULL, buf);
  const uint8_t expected[] = {'L', 'S', 'U', 'S', 1, 2, 3, 4, 5, 6, 7, 8};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, sizeof(expected));
  uint64_t t = 0;
  TEST_ASSERT_TRUE(lsu::decodeSubscribe(buf, sizeof(buf), t));
  TEST_ASSERT_EQUAL_UINT64(0x0102030405060708ULL, t);
  TEST_ASSERT_FALSE(lsu::decodeSubscribe(buf, sizeof(buf) - 1, t));
}

void test_unsubscribe() {
  TEST_ASSERT_TRUE(lsu::isUnsubscribe(lsu::kMagicUnsubscribe, 4));
  TEST_ASSERT_FALSE(lsu::isUnsubscribe(lsu::kMagicSubscribe, 4));
}

void test_status_round_trip() {
  lsu::Status in = {lsu::kCanBusOff, 500, 100000, 99000, 7, 3, 12, 5, 1, 3600};
  uint8_t buf[lsu::kStatusSize];
  lsu::encodeStatus(in, buf);
  TEST_ASSERT_EQUAL_HEX8('A', buf[3]);
  TEST_ASSERT_EQUAL_HEX8(lsu::kStatusVersion, buf[4]);
  TEST_ASSERT_EQUAL_HEX8(0x01, buf[6]);  // 500 = 0x01F4
  TEST_ASSERT_EQUAL_HEX8(0xF4, buf[7]);
  lsu::Status out;
  TEST_ASSERT_TRUE(lsu::decodeStatus(buf, sizeof(buf), out));
  TEST_ASSERT_EQUAL(in.canState, out.canState);
  TEST_ASSERT_EQUAL(in.bitrateKbps, out.bitrateKbps);
  TEST_ASSERT_EQUAL(in.canRx, out.canRx);
  TEST_ASSERT_EQUAL(in.sent, out.sent);
  TEST_ASSERT_EQUAL(in.queueFull, out.queueFull);
  TEST_ASSERT_EQUAL(in.sendErrors, out.sendErrors);
  TEST_ASSERT_EQUAL(in.busErrors, out.busErrors);
  TEST_ASSERT_EQUAL(in.rxMissed, out.rxMissed);
  TEST_ASSERT_EQUAL(in.rxOverrun, out.rxOverrun);
  TEST_ASSERT_EQUAL(in.uptimeS, out.uptimeS);
  TEST_ASSERT_EQUAL_STRING("bus-off", lsu::canStateName(out.canState));
}

void test_status_not_confused_with_cmp() {
  const uint8_t cmpStart[] = {0x01, 0x00, 0x00, 0x01};
  TEST_ASSERT_FALSE(lsu::isStatus(cmpStart, sizeof(cmpStart)));
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_subscribe_round_trip);
  RUN_TEST(test_unsubscribe);
  RUN_TEST(test_status_round_trip);
  RUN_TEST(test_status_not_confused_with_cmp);
  return UNITY_END();
}
