// Plays a recording (pcap, see pcap_file.h) back as if it were the ESP32,
// so any client works unchanged against it: cmp_client 127.0.0.1.
//
// Listens for SUBSCRIBE / UNSUBSCRIBE / REQUEST like the firmware, answers
// SUBSCRIBE with the recorded STATUS for the current position, and sends
// the recorded CMP packets to the subscriber with their original spacing
// (scaled by --speed). Playback runs only while someone is subscribed.
// Every CMP timestamp is moved to the present, keeping the frame's
// original age (capture on the ESP32 -> arrival on the laptop), unless
// --keep-time. REQUESTs are refused (transmit disabled): the answers to the
// recorded requests are already in the recording. With --loop the sequence
// counter keeps counting up across passes, and after the first pass the
// last STATUS stays, so the client sees neither gaps nor a restart.
//
// Usage: cmp_replay FILE.pcap [--bind 127.0.0.1] [--port 12670] [--speed X] [--loop]
//                   [--keep-time]

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cmp_decoder.h"
#include "lsu_protocol.h"
#include "pcap_file.h"

namespace {

constexpr int64_t kSubscriberTimeoutMs = 5000;  // as the firmware
constexpr size_t kCmpSequenceOffset = 6;       // CmpHeader.sequenceCounter
constexpr size_t kCmpTimestampOffset = 8;      // MessageHeader.timestamp

volatile std::sig_atomic_t stopRequested = 0;
void onSignal(int) { stopRequested = 1; }

int64_t monoNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint64_t unixNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

uint64_t get64be(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = v << 8 | p[i];
  return v;
}

void put64be(uint8_t* p, uint64_t v) {
  for (int i = 7; i >= 0; i--, v >>= 8) p[i] = uint8_t(v);
}

struct Event {
  uint64_t atNs;       // arrival on the laptop, relative to the first event
  uint64_t arrivalNs;  // the same, since the Unix epoch
  bool isStatus;
  std::vector<uint8_t> data;
};

bool sameAddr(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void usage(const char* prog) {
  fprintf(stderr,
          "Usage: %s FILE.pcap [--bind ADDR] [--port N] [--speed X] [--loop] [--keep-time]\n"
          "  --bind       address to listen on (default 127.0.0.1)\n"
          "  --port       UDP port (default %u)\n"
          "  --speed      playback speed factor (default 1)\n"
          "  --loop       start again at the end\n"
          "  --keep-time  send the recorded CMP timestamps unchanged\n",
          prog, lsu::kDefaultPort);
}

// The CMP packets and STATUS messages the ESP32 sent in the recording.
bool loadRecording(const std::string& path, uint16_t port, std::vector<Event>& events) {
  PcapReader reader;
  std::string error;
  if (!reader.open(path, &error)) {
    fprintf(stderr, "%s\n", error.c_str());
    return false;
  }
  UdpRecord r;
  uint64_t first = 0;
  while (reader.next(r)) {
    if (r.srcPort != port) continue;  // laptop -> ESP32, or other traffic
    bool isStatus = lsu::isStatus(r.payload.data(), r.payload.size());
    cmp::DecodedCanPacket pkt;
    if (!isStatus && !cmp::decodeCanPacket(r.payload.data(), r.payload.size(), pkt))
      continue;  // REQUEST_RESULT, or anything else
    if (events.empty()) first = r.timestampNs;
    events.push_back({r.timestampNs - first, r.timestampNs, isStatus, std::move(r.payload)});
  }
  if (!reader.error().empty())
    fprintf(stderr, "%s: %s, playing what was read\n", path.c_str(), reader.error().c_str());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string path, bindAddr = "127.0.0.1";
  uint16_t port = lsu::kDefaultPort;
  double speed = 1;
  bool loop = false, keepTime = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--bind" && i + 1 < argc) {
      bindAddr = argv[++i];
    } else if (a == "--port" && i + 1 < argc) {
      port = uint16_t(std::stoi(argv[++i]));
    } else if (a == "--speed" && i + 1 < argc) {
      speed = std::stod(argv[++i]);
    } else if (a == "--loop") {
      loop = true;
    } else if (a == "--keep-time") {
      keepTime = true;
    } else if (a == "-h" || a == "--help") {
      usage(argv[0]);
      return 0;
    } else if (a[0] != '-' && path.empty()) {
      path = a;
    } else {
      usage(argv[0]);
      return 2;
    }
  }
  if (path.empty() || !(speed > 0)) {
    usage(argv[0]);
    return 2;
  }

  std::vector<Event> events;
  if (!loadRecording(path, port, events)) return 1;
  size_t frames = 0, statuses = 0;
  for (const Event& e : events) (e.isStatus ? statuses : frames)++;
  if (frames == 0) {
    fprintf(stderr, "%s: no CMP packets from port %u\n", path.c_str(), port);
    return 1;
  }
  double durationS = events.back().atNs / 1e9;
  // Sequence numbers one pass covers, to continue them in the next pass.
  uint16_t firstSeq = 0, lastSeq = 0;
  bool haveSeq = false;
  for (const Event& e : events) {
    if (e.isStatus) continue;
    uint16_t seq = uint16_t(e.data[kCmpSequenceOffset] << 8 | e.data[kCmpSequenceOffset + 1]);
    if (!haveSeq) firstSeq = seq;
    lastSeq = seq;
    haveSeq = true;
  }
  uint16_t seqSpan = uint16_t(lastSeq - firstSeq + 1);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("%s: %zu CMP packets, %zu STATUS, %.1f s\n", path.c_str(), frames, statuses, durationS);

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(port);
  if (inet_pton(AF_INET, bindAddr.c_str(), &local.sin_addr) != 1) {
    fprintf(stderr, "bad address %s\n", bindAddr.c_str());
    return 2;
  }
  if (sock < 0 || bind(sock, (sockaddr*)&local, sizeof(local)) < 0) {
    fprintf(stderr, "cannot listen on %s:%u: %s\n", bindAddr.c_str(), port, strerror(errno));
    return 1;
  }
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  printf("playing as ESP32 on %s:%u, speed %g%s (Ctrl+C to stop); waiting for a subscriber\n",
         bindAddr.c_str(), port, speed, loop ? ", loop" : "");

  // Recording time played so far; advances only while subscribed.
  double playNs = 0;
  size_t next = 0;               // next event to play
  unsigned pass = 0;             // completed passes (--loop)
  int latestStatus = -1;         // last STATUS event already played
  bool active = false;
  sockaddr_in subscriber{};
  int64_t lastSubscribeNs = 0;
  int64_t lastTickNs = monoNs();
  int64_t lastProgressNs = 0;
  uint64_t sent = 0;

  auto statusToSend = [&]() -> const std::vector<uint8_t>* {
    if (latestStatus >= 0) return &events[latestStatus].data;
    for (const Event& e : events)  // before the first one: use the first
      if (e.isStatus) return &e.data;
    return nullptr;
  };

  while (!stopRequested) {
    int64_t now = monoNs();
    if (active) playNs += double(now - lastTickNs) * speed;
    lastTickNs = now;

    // Send everything that is due.
    while (active && next < events.size() && events[next].atNs <= playNs) {
      Event& e = events[next];
      if (e.isStatus) {
        if (pass == 0) latestStatus = int(next);
      } else {
        std::vector<uint8_t> pkt = e.data;
        if (pass > 0) {
          uint8_t* p = &pkt[kCmpSequenceOffset];
          uint16_t seq = uint16_t((p[0] << 8 | p[1]) + pass * seqSpan);
          p[0] = uint8_t(seq >> 8);
          p[1] = uint8_t(seq);
        }
        if (!keepTime && pkt.size() >= kCmpTimestampOffset + 8) {
          // Now minus the original age (arrival on the laptop - capture).
          int64_t age = int64_t(e.arrivalNs - get64be(&pkt[kCmpTimestampOffset]));
          put64be(&pkt[kCmpTimestampOffset], unixNs() - age);
        }
        sendto(sock, pkt.data(), pkt.size(), 0, (sockaddr*)&subscriber, sizeof(subscriber));
        sent++;
      }
      next++;
    }
    if (active && next >= events.size()) {
      if (!loop) {
        printf("end of recording: %llu packets sent\n", (unsigned long long)sent);
        break;
      }
      printf("end of recording, starting again\n");
      next = 0;
      playNs = 0;
      pass++;
    }
    if (active && now - lastProgressNs >= 5000000000LL) {
      printf("... %.1f / %.1f s\n", playNs / 1e9, durationS);
      lastProgressNs = now;
    }

    // Wait for a request, or until the next event is due.
    int waitMs = 100;
    if (active && next < events.size()) {
      double dueInMs = (double(events[next].atNs) - playNs) / speed / 1e6;
      waitMs = int(std::ceil(std::max(0.0, std::min(dueInMs, 100.0))));
    }
    pollfd pfd = {sock, POLLIN, 0};
    if (::poll(&pfd, 1, waitMs) <= 0) {
      if (active && monoNs() - lastSubscribeNs > kSubscriberTimeoutMs * 1000000) {
        active = false;
        printf("subscriber timed out, paused at %.1f s\n", playNs / 1e9);
      }
      continue;
    }

    uint8_t buf[64];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    if (n <= 0) continue;
    uint64_t laptopNs;
    uint8_t mode, pid;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    if (lsu::decodeSubscribe(buf, size_t(n), laptopNs)) {
      if (!active || !sameAddr(subscriber, from))
        printf("subscriber %s:%u, playing from %.1f s\n", ip, ntohs(from.sin_port), playNs / 1e9);
      active = true;
      subscriber = from;
      lastSubscribeNs = monoNs();
      if (const std::vector<uint8_t>* st = statusToSend())
        sendto(sock, st->data(), st->size(), 0, (sockaddr*)&from, sizeof(from));
    } else if (lsu::isUnsubscribe(buf, size_t(n))) {
      if (active && sameAddr(subscriber, from)) {
        active = false;
        printf("unsubscribed, paused at %.1f s\n", playNs / 1e9);
      }
    } else if (lsu::decodeRequest(buf, size_t(n), mode, pid)) {
      uint8_t reply[lsu::kRequestResultSize];
      lsu::encodeRequestResult(mode, pid, lsu::kRequestTxDisabled, reply);
      sendto(sock, reply, sizeof(reply), 0, (sockaddr*)&from, sizeof(from));
    }
  }
  close(sock);
  return 0;
}
