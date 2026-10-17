#include "pcap_file.h"

#include <cerrno>
#include <cstring>

namespace {

// pcap file header magic, as read on the writing host.
constexpr uint32_t kMagicMicros = 0xA1B2C3D4;
constexpr uint32_t kMagicNanos = 0xA1B23C4D;

constexpr uint32_t kLinkEthernet = 1;
constexpr uint32_t kLinkRaw = 101;
constexpr uint32_t kLinkLinuxSll = 113;
constexpr uint32_t kLinkIpv4 = 228;

constexpr uint32_t kSnapLen = 65535;
constexpr size_t kIpHeaderSize = 20;
constexpr size_t kUdpHeaderSize = 8;
constexpr uint8_t kProtoUdp = 17;

uint32_t swap32(uint32_t v) {
  return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24;
}

uint16_t get16be(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t get32be(const uint8_t* p) { return uint32_t(get16be(p)) << 16 | get16be(p + 2); }
void put16be(uint8_t* p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v;
}
void put32be(uint8_t* p, uint32_t v) {
  put16be(p, v >> 16);
  put16be(p + 2, v);
}

uint16_t ipChecksum(const uint8_t* p, size_t n) {
  uint32_t sum = 0;
  for (size_t i = 0; i + 1 < n; i += 2) sum += get16be(p + i);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return uint16_t(~sum);
}

}  // namespace

// --- PcapWriter ---

bool PcapWriter::open(const std::string& path, std::string* error) {
  close();
  _file = fopen(path.c_str(), "wb");
  if (!_file) {
    if (error) *error = "cannot create " + path + ": " + strerror(errno);
    return false;
  }
  // Host byte order, as the format intends; readers detect it by the magic.
  struct {
    uint32_t magic;
    uint16_t major, minor;
    int32_t thiszone;
    uint32_t sigfigs, snaplen, linktype;
  } header = {kMagicNanos, 2, 4, 0, 0, kSnapLen, kLinkRaw};
  if (fwrite(&header, sizeof(header), 1, _file) != 1) {
    if (error) *error = "cannot write " + path + ": " + strerror(errno);
    close();
    return false;
  }
  return true;
}

bool PcapWriter::write(const UdpRecord& r) {
  if (!_file) return false;
  size_t total = kIpHeaderSize + kUdpHeaderSize + r.payload.size();
  if (total > kSnapLen) return false;

  uint8_t hdr[kIpHeaderSize + kUdpHeaderSize] = {};
  uint8_t* ip = hdr;
  ip[0] = 0x45;  // IPv4, 20-byte header
  put16be(ip + 2, uint16_t(total));
  put16be(ip + 4, _ipId++);
  ip[8] = 64;  // TTL
  ip[9] = kProtoUdp;
  put32be(ip + 12, r.srcIp);
  put32be(ip + 16, r.dstIp);
  put16be(ip + 10, ipChecksum(ip, kIpHeaderSize));
  uint8_t* udp = hdr + kIpHeaderSize;
  put16be(udp, r.srcPort);
  put16be(udp + 2, r.dstPort);
  put16be(udp + 4, uint16_t(kUdpHeaderSize + r.payload.size()));
  // UDP checksum 0: "not computed", valid for IPv4.

  struct {
    uint32_t sec, frac, incl, orig;
  } rec = {uint32_t(r.timestampNs / 1000000000), uint32_t(r.timestampNs % 1000000000),
           uint32_t(total), uint32_t(total)};
  return fwrite(&rec, sizeof(rec), 1, _file) == 1 && fwrite(hdr, sizeof(hdr), 1, _file) == 1 &&
         (r.payload.empty() || fwrite(r.payload.data(), r.payload.size(), 1, _file) == 1);
}

void PcapWriter::flush() {
  if (_file) fflush(_file);
}

void PcapWriter::close() {
  if (_file) fclose(_file);
  _file = nullptr;
}

// --- PcapReader ---

bool PcapReader::open(const std::string& path, std::string* error) {
  close();
  _error.clear();
  _file = fopen(path.c_str(), "rb");
  if (!_file) {
    if (error) *error = "cannot open " + path + ": " + strerror(errno);
    return false;
  }
  uint32_t h[6];
  if (fread(h, sizeof(h), 1, _file) != 1) {
    if (error) *error = path + ": not a pcap file (too short)";
    close();
    return false;
  }
  uint32_t magic = h[0];
  _swapped = magic == swap32(kMagicMicros) || magic == swap32(kMagicNanos);
  if (_swapped) magic = swap32(magic);
  if (magic != kMagicMicros && magic != kMagicNanos) {
    // pcapng starts with 0x0A0D0D0A.
    if (error) *error = path + ": not a classic pcap file (pcapng is not supported)";
    close();
    return false;
  }
  _nanos = magic == kMagicNanos;
  _linkType = (_swapped ? swap32(h[5]) : h[5]) & 0xFFFF;
  if (_linkType != kLinkRaw && _linkType != kLinkIpv4 && _linkType != kLinkEthernet &&
      _linkType != kLinkLinuxSll) {
    if (error) *error = path + ": unsupported link type " + std::to_string(_linkType);
    close();
    return false;
  }
  return true;
}

bool PcapReader::next(UdpRecord& out) {
  std::vector<uint8_t> buf;
  while (_file) {
    uint32_t rec[4];
    size_t got = fread(rec, 1, sizeof(rec), _file);
    if (got == 0) return false;  // clean end of file
    if (got != sizeof(rec)) {
      _error = "truncated record header";
      return false;
    }
    if (_swapped)
      for (uint32_t& v : rec) v = swap32(v);
    uint32_t incl = rec[2];
    if (incl > kSnapLen * 4) {
      _error = "corrupt record length";
      return false;
    }
    buf.resize(incl);
    if (incl && fread(buf.data(), incl, 1, _file) != 1) {
      _error = "truncated record";
      return false;
    }
    out.timestampNs = uint64_t(rec[0]) * 1000000000 + uint64_t(rec[1]) * (_nanos ? 1 : 1000);

    const uint8_t* p = buf.data();
    size_t n = buf.size();
    if (_linkType == kLinkEthernet) {
      if (n < 14 || get16be(p + 12) != 0x0800) continue;  // only plain IPv4
      p += 14, n -= 14;
    } else if (_linkType == kLinkLinuxSll) {
      if (n < 16 || get16be(p + 14) != 0x0800) continue;
      p += 16, n -= 16;
    }
    if (parseIpv4(p, n, out)) return true;
  }
  return false;
}

bool PcapReader::parseIpv4(const uint8_t* p, size_t n, UdpRecord& out) {
  if (n < kIpHeaderSize || p[0] >> 4 != 4) return false;
  size_t ihl = size_t(p[0] & 0x0F) * 4;
  size_t total = get16be(p + 2);
  bool fragment = (get16be(p + 6) & 0x3FFF) != 0;  // MF flag or offset
  if (ihl < kIpHeaderSize || total > n || total < ihl + kUdpHeaderSize || p[9] != kProtoUdp ||
      fragment)
    return false;
  const uint8_t* udp = p + ihl;
  size_t udpLen = get16be(udp + 4);
  if (udpLen < kUdpHeaderSize || udpLen > total - ihl) return false;
  out.srcIp = get32be(p + 12);
  out.dstIp = get32be(p + 16);
  out.srcPort = get16be(udp);
  out.dstPort = get16be(udp + 2);
  out.payload.assign(udp + kUdpHeaderSize, udp + udpLen);
  return true;
}

void PcapReader::close() {
  if (_file) fclose(_file);
  _file = nullptr;
}
