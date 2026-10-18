#include "obd_decoder.h"

#include <cstdio>

#include "lsu_obd.h"

namespace {

// Mode 01 PIDs with a formula: value = raw * scale + offset, where raw is
// A (1 byte) or 256A + B (2 bytes).
struct PidFormula {
  uint8_t pid;
  const char* name;
  int bytes;
  double scale;
  double offset;
  const char* unit;
};

constexpr PidFormula kMode01[] = {
    {0x04, "engine load", 1, 100.0 / 255, 0, "%"},
    {0x05, "coolant temperature", 1, 1, -40, "°C"},
    {0x0B, "intake manifold pressure", 1, 1, 0, "kPa"},
    {0x0C, "engine speed", 2, 0.25, 0, "rpm"},
    {0x0D, "vehicle speed", 1, 1, 0, "km/h"},
    {0x0F, "intake air temperature", 1, 1, -40, "°C"},
    {0x10, "MAF air flow", 2, 0.01, 0, "g/s"},
    {0x11, "throttle position", 1, 100.0 / 255, 0, "%"},
    {0x1F, "run time since start", 2, 1, 0, "s"},
    {0x2F, "fuel level", 1, 100.0 / 255, 0, "%"},
    {0x33, "barometric pressure", 1, 1, 0, "kPa"},
    {0x42, "control module voltage", 2, 0.001, 0, "V"},
    {0x46, "ambient air temperature", 1, 1, -40, "°C"},
    {0x49, "accelerator pedal D", 1, 100.0 / 255, 0, "%"},
    {0x4A, "accelerator pedal E", 1, 100.0 / 255, 0, "%"},
    {0x5C, "engine oil temperature", 1, 1, -40, "°C"},
};

const PidFormula* findMode01(uint8_t pid) {
  for (const auto& f : kMode01)
    if (f.pid == pid) return &f;
  return nullptr;
}

std::string hex(const uint8_t* p, size_t n) {
  std::string s;
  char b[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof(b), i ? " %02X" : "%02X", p[i]);
    s += b;
  }
  return s;
}

std::string ascii(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; i++)
    if (p[i] >= 0x20 && p[i] < 0x7F) s += char(p[i]);
  return s;
}

}  // namespace

std::string ObdDecoder::pidName(uint8_t mode, uint8_t pid) {
  if (mode == obd::kModeCurrentData) {
    if (const PidFormula* f = findMode01(pid)) return f->name;
    if (pid % 0x20 == 0) return "supported PIDs";
  }
  if (mode == obd::kModeVehicleInfo && pid == 0x02) return "VIN";
  if (mode == obd::kModeVehicleInfo && pid == 0x0A) return "ECU name";
  return "";
}

std::string ObdDecoder::feed(const cmp::CanFrame& f, uint8_t n) {
  if (!obd::isResponseId(f.id, f.extended) || f.rtr || n < 1) return "";
  const uint8_t* d = f.data;
  switch (d[0] >> 4) {
    case obd::kPciSingle: {
      size_t len = d[0] & 0x0F;
      if (len == 0 || len + 1 > n) return "";
      _pending.erase(f.id);
      return describe(f.id, std::vector<uint8_t>(d + 1, d + 1 + len));
    }
    case obd::kPciFirst: {
      if (n < 2) return "";
      Pending& p = _pending[f.id];
      p = Pending{};
      p.length = size_t(d[0] & 0x0F) << 8 | d[1];
      p.data.assign(d + 2, d + n);
      return "";
    }
    case obd::kPciConsecutive: {
      auto it = _pending.find(f.id);
      if (it == _pending.end()) return "";
      Pending& p = it->second;
      if ((d[0] & 0x0F) != p.nextSeq) {  // lost a frame: give up on it
        _pending.erase(it);
        return "";
      }
      p.nextSeq = (p.nextSeq + 1) & 0x0F;
      p.data.insert(p.data.end(), d + 1, d + n);
      if (p.data.size() < p.length) return "";
      p.data.resize(p.length);
      std::string line = describe(f.id, p.data);
      _pending.erase(it);
      return line;
    }
    default:
      return "";
  }
}

std::string ObdDecoder::describe(uint32_t ecu, const std::vector<uint8_t>& r) {
  char head[32];
  snprintf(head, sizeof(head), "ECU %03X  ", ecu);
  std::string line = head;
  if (r.size() >= 3 && r[0] == obd::kNegativeResponse) {
    char b[64];
    snprintf(b, sizeof(b), "%02X     negative response, code %02X", r[1], r[2]);
    return line + b;
  }
  if (r.size() < 2 || r[0] < obd::kPositiveResponse) return line + "? " + hex(r.data(), r.size());

  uint8_t mode = r[0] - obd::kPositiveResponse;
  uint8_t pid = r[1];
  const uint8_t* v = r.data() + 2;
  size_t nv = r.size() - 2;
  char b[160];
  snprintf(b, sizeof(b), "%02X %02X  ", mode, pid);
  line += b;

  if (mode == obd::kModeCurrentData) {
    if (const PidFormula* f = findMode01(pid); f && nv >= size_t(f->bytes)) {
      double raw = f->bytes == 1 ? v[0] : v[0] * 256.0 + v[1];
      snprintf(b, sizeof(b), "%s = %g %s", f->name, raw * f->scale + f->offset, f->unit);
      return line + b;
    }
    if (pid % 0x20 == 0 && nv >= 4) {  // bit 31 = PID pid+1 ... bit 0 = pid+32
      std::string list;
      uint32_t bits = uint32_t(v[0]) << 24 | v[1] << 16 | v[2] << 8 | v[3];
      for (int i = 0; i < 32; i++) {
        if (!(bits & (0x80000000u >> i))) continue;
        snprintf(b, sizeof(b), list.empty() ? "%02X" : " %02X", pid + 1 + i);
        list += b;
      }
      return line + "supported PIDs: " + (list.empty() ? "none" : list);
    }
  }
  if (mode == obd::kModeVehicleInfo && nv >= 1 && (pid == 0x02 || pid == 0x0A)) {
    // First value byte: number of data items.
    return line + (pid == 0x02 ? "VIN: " : "ECU name: ") + ascii(v + 1, nv - 1);
  }
  return line + hex(v, nv);
}
