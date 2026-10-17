// Recordings of the UDP traffic from the ESP32, as classic pcap files with
// nanosecond timestamps. They open directly in Wireshark (Decode As... UDP
// port 12670 -> ASAM CMP) and are played back by cmp_replay.
//
// PcapWriter writes LINKTYPE_RAW (bare IPv4 + UDP headers, built here).
// PcapReader returns the UDP datagrams of a file in LINKTYPE_RAW, IPV4,
// ETHERNET or LINUX_SLL, so a capture taken with Wireshark/tcpdump on the
// laptop plays back too. Non-UDP and fragmented packets are skipped.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct UdpRecord {
  uint64_t timestampNs;  // since the Unix epoch
  uint32_t srcIp;        // host byte order
  uint16_t srcPort;
  uint32_t dstIp;
  uint16_t dstPort;
  std::vector<uint8_t> payload;
};

class PcapWriter {
 public:
  PcapWriter() = default;
  ~PcapWriter() { close(); }
  PcapWriter(const PcapWriter&) = delete;
  PcapWriter& operator=(const PcapWriter&) = delete;

  bool open(const std::string& path, std::string* error = nullptr);
  bool write(const UdpRecord& record);  // false on a write error
  void flush();
  void close();
  bool isOpen() const { return _file != nullptr; }

 private:
  FILE* _file = nullptr;
  uint16_t _ipId = 0;
};

class PcapReader {
 public:
  PcapReader() = default;
  ~PcapReader() { close(); }
  PcapReader(const PcapReader&) = delete;
  PcapReader& operator=(const PcapReader&) = delete;

  bool open(const std::string& path, std::string* error = nullptr);
  // Next UDP datagram; false at the end of the file (or on a truncated
  // record, which sets error()).
  bool next(UdpRecord& out);
  const std::string& error() const { return _error; }
  void close();

 private:
  bool parseIpv4(const uint8_t* p, size_t n, UdpRecord& out);

  FILE* _file = nullptr;
  bool _swapped = false;  // file written on a host of the other byte order
  bool _nanos = false;    // timestamps in ns instead of us
  uint32_t _linkType = 0;
  std::string _error;
};
