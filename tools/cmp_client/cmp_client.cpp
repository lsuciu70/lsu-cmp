// Test client for the lsu-cmp ESP32: subscribes and prints CAN frames.
//
// Sends SUBSCRIBE (with this machine's time, which keeps the ESP32 clock in
// sync) every second and UNSUBSCRIBE on Ctrl+C; protocol in lsu_protocol.h.
// Decodes the ASAM CMP data packets and reports sequenceCounter gaps. The
// STATUS reply to each SUBSCRIBE shows whether the ESP32 is reachable and
// the state of its CAN controller; changes and idle periods are printed.
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
#include "lsu_protocol.h"

namespace {

constexpr int kSubscribeIntervalMs = 1000;
constexpr int kIdleReportMs = 5000;
// No STATUS for this long: the ESP32 is considered unreachable.
constexpr int kStatusTimeoutMs = 3000;

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

struct EspState {
  bool known = false;
  int64_t lastStatusMs = 0;
  lsu::Status st{};
};

// Prints what changed since the previous STATUS.
void handleStatus(EspState& esp, const lsu::Status& st) {
  const lsu::Status& old = esp.st;
  if (!esp.known) {
    printf("subscribed: ESP32 up %us, CAN %s at %u kbit/s, %u frames received, "
           "%u bus errors\n",
           st.uptimeS, lsu::canStateName(st.canState), st.bitrateKbps, st.canRx, st.busErrors);
  } else if (st.uptimeS < old.uptimeS) {
    printf("! ESP32 restarted (up %us)\n", st.uptimeS);
  } else {
    if (st.canState != old.canState)
      printf("! CAN state: %s -> %s\n", lsu::canStateName(old.canState),
             lsu::canStateName(st.canState));
    if (st.busErrors > old.busErrors)
      printf("! CAN bus errors: +%u (total %u)\n", st.busErrors - old.busErrors, st.busErrors);
    uint32_t dropped = (st.rxMissed + st.rxOverrun + st.queueFull) -
                       (old.rxMissed + old.rxOverrun + old.queueFull);
    if (dropped) printf("! ESP32 dropped %u frame(s) (receive queues full)\n", dropped);
    if (st.sendErrors > old.sendErrors)
      printf("! ESP32 UDP send errors: +%u\n", st.sendErrors - old.sendErrors);
  }
  esp.known = true;
  esp.st = st;
}

void usage(const char* prog) {
  fprintf(stderr, "Usage: %s [host] [--port N] [--quiet]\n"
                  "  host     ESP32 IP (default 10.10.10.10, AP mode)\n"
                  "  --port   UDP port (default %u)\n"
                  "  --quiet  only a summary line every second\n",
          prog, lsu::kDefaultPort);
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "10.10.10.10";
  std::string port = std::to_string(lsu::kDefaultPort);
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
  const int64_t startMs = monoMs();
  EspState espState;
  bool reportedUnreachable = false;

  while (!stopRequested) {
    int64_t now = monoMs();
    if (now >= nextSubscribe) {
      uint8_t msg[lsu::kSubscribeSize];
      lsu::encodeSubscribe(nowNs(), msg);
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
      } else if (lsu::isStatus(buf, size_t(n))) {
        lsu::Status st;
        if (lsu::decodeStatus(buf, size_t(n), st)) {
          if (reportedUnreachable) printf("ESP32 reachable again\n");
          reportedUnreachable = false;
          handleStatus(espState, st);
          espState.lastStatusMs = monoMs();
        } else {
          bad++;
          printf("? STATUS with unknown version/size (%zd bytes)\n", n);
        }
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
      printf("%llu frames/s, %llu total, %llu lost | CAN %s, bus errors %u\n",
             (unsigned long long)perSecond, (unsigned long long)total, (unsigned long long)lost,
             espState.known ? lsu::canStateName(espState.st.canState) : "?",
             espState.st.busErrors);
      perSecond = 0;
      lastReport = now;
    }
    int64_t sinceStatus = now - (espState.known ? espState.lastStatusMs : startMs);
    if (sinceStatus >= kStatusTimeoutMs && !reportedUnreachable) {
      printf("! no reply from ESP32 for %lld s: unreachable or not running\n",
             (long long)(sinceStatus / 1000));
      reportedUnreachable = true;
    }
    if (now - lastPacket >= kIdleReportMs && now - lastIdle >= kIdleReportMs) {
      if (!reportedUnreachable && espState.known)
        printf("... no CAN frames for %lld s; ESP32 ok, CAN %s, %u frames received "
               "since boot, %u bus errors\n",
               (long long)((now - lastPacket) / 1000), lsu::canStateName(espState.st.canState),
               espState.st.canRx, espState.st.busErrors);
      lastIdle = now;
    }
  }

  send(sock, lsu::kMagicUnsubscribe, lsu::kUnsubscribeSize, 0);
  printf("\nunsubscribed. %llu frames, %llu lost, %llu bad packets\n",
         (unsigned long long)total, (unsigned long long)lost, (unsigned long long)bad);
  close(sock);
  freeaddrinfo(esp);
  return 0;
}
