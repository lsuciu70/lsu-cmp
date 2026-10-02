// Test client for the lsu-cmp ESP32: subscribes and prints CAN frames.
//
// Sends SUBSCRIBE ("LSUS" + this machine's time, ns since the Unix epoch)
// every second, which also keeps the ESP32 clock in sync with this machine,
// and UNSUBSCRIBE ("LSUU") on Ctrl+C. Decodes the ASAM CMP data packets,
// reports sequenceCounter gaps, and prints a status line when nothing arrives.
//
// Usage: cmp_client [host] [--port 12670] [--quiet]

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "cmp_encoder.h"

namespace {

constexpr int kSubscribeIntervalMs = 1000;
constexpr int kIdleReportMs = 5000;

volatile std::sig_atomic_t stopRequested = 0;

void onSignal(int) { stopRequested = 1; }

int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t monoMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint16_t get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t* p) { return uint32_t(get16(p)) << 16 | get16(p + 2); }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) << 32 | get32(p + 4); }

struct Frame {
  uint16_t seq;
  uint64_t timestampNs;
  uint8_t commonFlags;
  uint32_t id;
  bool ext;
  bool rtr;
  uint8_t dlc;
  uint8_t dataLength;
  const uint8_t* data;
};

// Returns false if the packet is not a CMP data message with a CAN payload.
bool decode(const uint8_t* p, size_t n, Frame& f) {
  constexpr size_t kHeaders = cmp::kCmpHeaderSize + cmp::kMessageHeaderSize;
  if (n < kHeaders + cmp::kCanHeaderSize) return false;
  if (p[0] != cmp::kVersion || p[4] != cmp::kMessageTypeData) return false;
  const uint8_t* m = p + cmp::kCmpHeaderSize;
  if (m[13] != cmp::kPayloadTypeCan) return false;
  const uint8_t* c = m + cmp::kMessageHeaderSize;
  uint32_t idWord = get32(c + 4);
  f.seq = get16(p + 6);
  f.timestampNs = get64(m);
  f.commonFlags = m[12];
  f.id = idWord & cmp::kIdMask;
  f.ext = idWord & cmp::kIdIde;
  f.rtr = idWord & cmp::kIdRtr;
  f.dlc = c[14];
  f.dataLength = c[15];
  f.data = c + cmp::kCanHeaderSize;
  return f.dataLength <= cmp::kMaxCanData && kHeaders + cmp::kCanHeaderSize + f.dataLength <= n;
}

void printFrame(const Frame& f) {
  time_t secs = f.timestampNs / 1000000000;
  tm local;
  localtime_r(&secs, &local);
  char when[16];
  strftime(when, sizeof(when), "%H:%M:%S", &local);
  // Age = arrival here - capture on the ESP32, both in this machine's clock:
  // WiFi latency + queueing, plus the sync error (a few ms).
  double ageMs = (nowNs() - int64_t(f.timestampNs)) / 1e6;

  char id[16];
  if (f.ext)
    snprintf(id, sizeof(id), "%08X", f.id);
  else
    snprintf(id, sizeof(id), "     %03X", f.id);

  std::string body = f.rtr ? "RTR" : "";
  for (int i = 0; !f.rtr && i < f.dataLength; i++) {
    char byte[4];
    snprintf(byte, sizeof(byte), i ? " %02X" : "%02X", f.data[i]);
    body += byte;
  }

  printf("%s.%03u  seq %5u  age %6.1f ms  %s  [%u]  %-23s%s%s\n", when,
         unsigned(f.timestampNs / 1000000 % 1000), f.seq, ageMs, id, f.dlc, body.c_str(),
         (f.commonFlags & cmp::kFlagInsync) ? "" : "  !insync",
         (f.commonFlags & cmp::kFlagOverflow) ? "  OVERFLOW" : "");
}

void usage(const char* prog) {
  fprintf(stderr, "Usage: %s [host] [--port N] [--quiet]\n"
                  "  host     ESP32 IP (default 10.10.10.10, AP mode)\n"
                  "  --port   UDP port (default %u)\n"
                  "  --quiet  only a summary line every second\n",
          prog, 12670);
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "10.10.10.10";
  std::string port = "12670";
  bool quiet = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) {
      port = argv[++i];
    } else if (a == "--quiet") {
      quiet = true;
    } else if (a == "-h" || a == "--help") {
      usage(argv[0]);
      return 0;
    } else if (a[0] != '-') {
      host = a;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* esp = nullptr;
  if (int err = getaddrinfo(host.c_str(), port.c_str(), &hints, &esp)) {
    fprintf(stderr, "cannot resolve %s: %s\n", host.c_str(), gai_strerror(err));
    return 1;
  }
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("socket");
    return 1;
  }
  // Only accept packets from the ESP32.
  if (connect(sock, esp->ai_addr, esp->ai_addrlen) < 0) {
    perror("connect");
    return 1;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("subscribing to %s:%s (Ctrl+C to stop)\n", host.c_str(), port.c_str());

  uint64_t total = 0, lost = 0, bad = 0, perSecond = 0;
  bool haveSeq = false;
  uint16_t expectedSeq = 0;
  int64_t nextSubscribe = 0;
  int64_t lastPacket = monoMs();
  int64_t lastReport = monoMs();
  int64_t lastIdle = monoMs();

  while (!stopRequested) {
    int64_t now = monoMs();
    if (now >= nextSubscribe) {
      uint8_t msg[12] = {'L', 'S', 'U', 'S'};
      uint64_t t = nowNs();
      for (int i = 0; i < 8; i++) msg[4 + i] = uint8_t(t >> (56 - 8 * i));
      if (send(sock, msg, sizeof(msg), 0) < 0) perror("send SUBSCRIBE");
      nextSubscribe = now + kSubscribeIntervalMs;
    }

    pollfd pfd = {sock, POLLIN, 0};
    int wait = int(nextSubscribe - now);
    if (poll(&pfd, 1, wait < 100 ? wait : 100) > 0) {
      uint8_t buf[2048];
      ssize_t n = recv(sock, buf, sizeof(buf), 0);
      if (n < 0) {
        // ECONNREFUSED: ICMP port unreachable, nothing listening yet.
        if (errno != ECONNREFUSED) perror("recv");
      } else {
        Frame f;
        if (!decode(buf, size_t(n), f)) {
          bad++;
          printf("? unexpected packet (%zd bytes)\n", n);
        } else {
          if (total == 0) printf("receiving CMP data\n");
          total++;
          perSecond++;
          lastPacket = monoMs();
          if (haveSeq && f.seq != expectedSeq) {
            unsigned gap = uint16_t(f.seq - expectedSeq);
            lost += gap;
            printf("! sequence gap: %u packet(s) lost\n", gap);
          }
          haveSeq = true;
          expectedSeq = f.seq + 1;
          if (!quiet) printFrame(f);
        }
      }
    }

    now = monoMs();
    if (quiet && now - lastReport >= 1000) {
      printf("%llu frames/s, %llu total, %llu lost\n", (unsigned long long)perSecond,
             (unsigned long long)total, (unsigned long long)lost);
      perSecond = 0;
      lastReport = now;
    }
    if (now - lastPacket >= kIdleReportMs && now - lastIdle >= kIdleReportMs) {
      printf("... no data for %lld s (%llu frames so far): ESP32 unreachable, "
             "or no CAN traffic\n",
             (long long)((now - lastPacket) / 1000), (unsigned long long)total);
      lastIdle = now;
    }
  }

  send(sock, "LSUU", 4, 0);
  printf("\nunsubscribed. %llu frames, %llu lost, %llu bad packets\n",
         (unsigned long long)total, (unsigned long long)lost, (unsigned long long)bad);
  close(sock);
  freeaddrinfo(esp);
  return 0;
}
