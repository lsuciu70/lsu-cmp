// OBD2 request whitelist, request frame and ISO-TP flow control.
#include <lsu_obd.h>
#include <unity.h>

namespace {

void test_whitelist() {
  TEST_ASSERT_TRUE(obd::isAllowedRequest(0x01, 0x0C));
  TEST_ASSERT_TRUE(obd::isAllowedRequest(0x09, 0x02));
  TEST_ASSERT_FALSE(obd::isAllowedRequest(0x03, 0x00));  // read DTCs: later
  TEST_ASSERT_FALSE(obd::isAllowedRequest(0x04, 0x00));  // clear DTCs
  TEST_ASSERT_FALSE(obd::isAllowedRequest(0x10, 0x03));  // UDS session control
  TEST_ASSERT_FALSE(obd::isAllowedRequest(0x11, 0x01));  // UDS ECU reset
  TEST_ASSERT_FALSE(obd::isAllowedRequest(0x22, 0xF1));  // UDS read by id
}

void test_request_frame() {
  uint8_t data[8];
  obd::buildRequest(0x01, 0x0C, data);
  const uint8_t expected[] = {0x02, 0x01, 0x0C, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, data, 8);
}

void test_response_ids() {
  TEST_ASSERT_FALSE(obd::isResponseId(0x7E7, false));
  TEST_ASSERT_TRUE(obd::isResponseId(0x7E8, false));
  TEST_ASSERT_TRUE(obd::isResponseId(0x7EF, false));
  TEST_ASSERT_FALSE(obd::isResponseId(0x7F0, false));
  TEST_ASSERT_FALSE(obd::isResponseId(0x7E8, true));
  TEST_ASSERT_FALSE(obd::isResponseId(0x17F00010, true));
}

void test_first_frame() {
  // VIN: first frame, 20 bytes: 49 02 01 + 17 characters.
  const uint8_t ff[] = {0x10, 0x14, 0x49, 0x02, 0x01, 'W', 'V', 'W'};
  TEST_ASSERT_TRUE(obd::isFirstFrame(0x7E8, false, 8, ff));
  TEST_ASSERT_FALSE(obd::isFirstFrame(0x7DF, false, 8, ff));  // not a response
  TEST_ASSERT_FALSE(obd::isFirstFrame(0x7E8, true, 8, ff));
  TEST_ASSERT_FALSE(obd::isFirstFrame(0x7E8, false, 1, ff));
  const uint8_t sf[] = {0x04, 0x41, 0x0C, 0x0C, 0x80, 0xAA, 0xAA, 0xAA};
  TEST_ASSERT_FALSE(obd::isFirstFrame(0x7E8, false, 8, sf));
  const uint8_t cf[] = {0x21, 'W', 'Z', 'Z', 'Z', 'A', 'U', 'Z'};
  TEST_ASSERT_FALSE(obd::isFirstFrame(0x7E8, false, 8, cf));
}

void test_flow_control() {
  TEST_ASSERT_EQUAL_HEX32(0x7E0, obd::flowControlId(0x7E8));
  TEST_ASSERT_EQUAL_HEX32(0x7E7, obd::flowControlId(0x7EF));
  uint8_t data[8];
  obd::buildFlowControl(data);
  const uint8_t expected[] = {0x30, 0x00, 0x00, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, data, 8);
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_whitelist);
  RUN_TEST(test_request_frame);
  RUN_TEST(test_response_ids);
  RUN_TEST(test_first_frame);
  RUN_TEST(test_flow_control);
  return UNITY_END();
}
