// PcapWriter -> PcapReader round trip, and reading an Ethernet capture as
// Wireshark/dumpcap writes it. Run: ctest --test-dir tools/cmp_client/build
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pcap_file.h"

namespace {

int failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      failures++;                                                \
    }                                                            \
  } while (0)

std::string tempPath(const char* name) {
  const char* dir = getenv("TMPDIR");
  return std::string(dir ? dir : "/tmp") + "/" + name;
}

void testRoundTrip() {
  std::string path = tempPath("lsu_cmp_test_round_trip.pcap");
  UdpRecord a{1760000000123456789ULL, 0xC0A864F0, 12670, 0xC0A86408, 40000, {1, 2, 3}};
  UdpRecord b{1760000001000000001ULL, 0xC0A864F0, 12670, 0xC0A86408, 40000, {}};
  PcapWriter w;
  CHECK(w.open(path));
  CHECK(w.write(a));
  CHECK(w.write(b));
  w.close();

  PcapReader r;
  CHECK(r.open(path));
  UdpRecord got;
  CHECK(r.next(got));
  CHECK(got.timestampNs == a.timestampNs);  // nanoseconds kept
  CHECK(got.srcIp == a.srcIp && got.srcPort == a.srcPort);
  CHECK(got.dstIp == a.dstIp && got.dstPort == a.dstPort);
  CHECK(got.payload == a.payload);
  CHECK(r.next(got));
  CHECK(got.timestampNs == b.timestampNs && got.payload.empty());
  CHECK(!r.next(got));
  CHECK(r.error().empty());
  remove(path.c_str());
}

// Microsecond pcap, LINKTYPE_ETHERNET: one ARP frame (skipped) and one
// UDP datagram 127.0.0.1:12670 -> 127.0.0.1:40000 with payload "hi".
void testEthernetCapture() {
  std::string path = tempPath("lsu_cmp_test_ethernet.pcap");
  FILE* f = fopen(path.c_str(), "wb");
  CHECK(f);
  if (!f) return;
  uint32_t header[6] = {0xA1B2C3D4, 0x00040002, 0, 0, 65535, 1};
  fwrite(header, sizeof(header), 1, f);

  uint8_t arp[42] = {};
  arp[12] = 0x08, arp[13] = 0x06;
  uint32_t rec1[4] = {100, 0, sizeof(arp), sizeof(arp)};
  fwrite(rec1, sizeof(rec1), 1, f);
  fwrite(arp, sizeof(arp), 1, f);

  uint8_t udp[14 + 20 + 8 + 2] = {};
  udp[12] = 0x08, udp[13] = 0x00;  // IPv4
  uint8_t* ip = udp + 14;
  ip[0] = 0x45, ip[3] = 30, ip[9] = 17;
  ip[12] = 127, ip[15] = 1, ip[16] = 127, ip[19] = 1;
  uint8_t* u = ip + 20;
  u[0] = 12670 >> 8, u[1] = 12670 & 0xFF, u[2] = 40000 >> 8, u[3] = 40000 & 0xFF, u[5] = 10;
  u[8] = 'h', u[9] = 'i';
  uint32_t rec2[4] = {101, 250000, sizeof(udp), sizeof(udp)};
  fwrite(rec2, sizeof(rec2), 1, f);
  fwrite(udp, sizeof(udp), 1, f);
  fclose(f);

  PcapReader r;
  CHECK(r.open(path));
  UdpRecord got;
  CHECK(r.next(got));
  CHECK(got.timestampNs == 101250000000ULL);  // microseconds converted
  CHECK(got.srcIp == 0x7F000001 && got.srcPort == 12670 && got.dstPort == 40000);
  CHECK(got.payload.size() == 2 && got.payload[0] == 'h' && got.payload[1] == 'i');
  CHECK(!r.next(got));
  remove(path.c_str());
}

void testRejectsOtherFiles() {
  std::string path = tempPath("lsu_cmp_test_pcapng.pcap");
  FILE* f = fopen(path.c_str(), "wb");
  CHECK(f);
  if (!f) return;
  uint32_t pcapng[6] = {0x0A0D0D0A, 28, 0x1A2B3C4D, 1, 0, 0};
  fwrite(pcapng, sizeof(pcapng), 1, f);
  fclose(f);
  PcapReader r;
  std::string error;
  CHECK(!r.open(path, &error));
  CHECK(error.find("pcapng") != std::string::npos);
  remove(path.c_str());
}

}  // namespace

int main() {
  testRoundTrip();
  testEthernetCapture();
  testRejectsOtherFiles();
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  puts("pcap tests passed");
  return 0;
}
