// Turns the OBD2 responses in the CMP stream back into readable values:
// single frames, and multi-frame (ISO-TP) responses such as the VIN,
// reassembled per ECU. Response layout and PID formulas from SAE J1979.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cmp_decoder.h"

class ObdDecoder {
 public:
  // Feeds one CAN frame. Returns one line per complete OBD2 response, e.g.
  // "ECU 7E8  01 0C  engine speed = 812.5 rpm", or an empty string if the
  // frame is not an OBD2 response or the response is still incomplete.
  std::string feed(const cmp::CanFrame& frame, uint8_t dataLength);

  // "01 0C" -> engine speed, ...; empty if this PID has no formula here.
  static std::string pidName(uint8_t mode, uint8_t pid);

 private:
  struct Pending {
    size_t length = 0;  // announced by the first frame
    uint8_t nextSeq = 1;
    std::vector<uint8_t> data;
  };
  static std::string describe(uint32_t ecu, const std::vector<uint8_t>& payload);

  std::map<uint32_t, Pending> _pending;  // by response ID
};
