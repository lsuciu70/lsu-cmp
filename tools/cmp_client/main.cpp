// Test client for the lsu-cmp ESP32: subscribes and prints what happens.
//
// All protocol work is in CmpSubscriber; this file only parses the command
// line and prints the callbacks: each CAN frame, sequence gaps, the ESP32
// status at subscription and whenever it changes (CAN state, bus errors,
// drops, restarts), and idle periods. With --obd it also asks the ESP32
// for OBD2 requests, in turn, and prints the decoded answers (ObdDecoder).
// With --record it also writes every datagram from the ESP32 to a pcap file,
// for Wireshark and for playback with cmp_replay.
//
// Usage: cmp_client [host] [--port 12670] [--quiet]
//                   [--obd MODE:PID[,MODE:PID...]] [--interval MS]
//                   [--record FILE.pcap]

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "cmp_subscriber.h"
#include "obd_decoder.h"
#include "pcap_file.h"

namespace {

constexpr int kIdleReportMs = 5000;

volatile std::sig_atomic_t stopRequested = 0;

void onSignal(int) { stopRequested = 1; }

int64_t monoMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t unixNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void printFrame(const cmp::DecodedCanPacket& p) {
  time_t secs = p.timestampNs / 1000000000;
  tm local;
  localtime_r(&secs, &local);
  char when[16];
  strftime(when, sizeof(when), "%H:%M:%S", &local);
  // Age = arrival here - capture on the ESP32, both in this machine's clock:
  // WiFi latency + queueing, plus the sync error (a few ms).
  double ageMs = (unixNs() - int64_t(p.timestampNs)) / 1e6;

  const cmp::CanFrame& f = p.frame;
  char id[16];
  if (f.extended)
    snprintf(id, sizeof(id), "%08X", f.id);
  else
    snprintf(id, sizeof(id), "     %03X", f.id);

  std::string body = f.rtr ? "RTR" : "";
  for (int i = 0; !f.rtr && i < p.dataLength; i++) {
    char byte[4];
    snprintf(byte, sizeof(byte), i ? " %02X" : "%02X", f.data[i]);
    body += byte;
  }

  printf("%s.%03u  seq %5u  age %6.1f ms  %s  [%u]  %-23s%s%s\n", when,
         unsigned(p.timestampNs / 1000000 % 1000), p.sequenceCounter, ageMs, id, f.dlc,
         body.c_str(), (p.commonFlags & cmp::kFlagInsync) ? "" : "  !insync",
         (p.commonFlags & cmp::kFlagOverflow) ? "  OVERFLOW" : "");
}

// Prints what changed since the previous STATUS (nullptr: the first one).
void printStatusChange(const lsu::Status* old, const lsu::Status& st) {
  if (!old) {
    printf("subscribed: ESP32 up %us, CAN %s at %u kbit/s, %u frames received, "
           "%u bus errors\n",
           st.uptimeS, lsu::canStateName(st.canState), st.bitrateKbps, st.canRx, st.busErrors);
    return;
  }
  if (st.uptimeS < old->uptimeS) {
    printf("! ESP32 restarted (up %us)\n", st.uptimeS);
    return;
  }
  if (st.canState != old->canState)
    printf("! CAN state: %s -> %s\n", lsu::canStateName(old->canState),
           lsu::canStateName(st.canState));
  if (st.busErrors > old->busErrors)
    printf("! CAN bus errors: +%u (total %u)\n", st.busErrors - old->busErrors, st.busErrors);
  uint32_t dropped = (st.rxMissed + st.rxOverrun + st.queueFull) -
                     (old->rxMissed + old->rxOverrun + old->queueFull);
  if (dropped) printf("! ESP32 dropped %u frame(s) (receive queues full)\n", dropped);
  if (st.sendErrors > old->sendErrors)
    printf("! ESP32 UDP send errors: +%u\n", st.sendErrors - old->sendErrors);
}

// "01:0C,09:02" -> {{0x01, 0x0C}, {0x09, 0x02}}. False on a syntax error.
bool parseObdList(const std::string& s, std::vector<std::pair<uint8_t, uint8_t>>& out) {
  size_t start = 0;
  while (start <= s.size()) {
    size_t end = s.find(',', start);
    if (end == std::string::npos) end = s.size();
    unsigned mode, pid;
    char extra;
    if (sscanf(s.substr(start, end - start).c_str(), "%x:%x%c", &mode, &pid, &extra) != 2 ||
        mode > 0xFF || pid > 0xFF)
      return false;
    out.emplace_back(uint8_t(mode), uint8_t(pid));
    start = end + 1;
  }
  return !out.empty();
}

void usage(const char* prog) {
  fprintf(stderr, "Usage: %s [host] [--port N] [--quiet] [--obd LIST] [--interval MS]\n"
                  "       [--record FILE.pcap]\n"
                  "  host        ESP32 IP (default 10.10.10.10, AP mode)\n"
                  "  --port      UDP port (default %u)\n"
                  "  --quiet     only a summary line every second (and OBD2 answers)\n"
                  "  --obd       OBD2 requests to send in turn, hex MODE:PID, comma\n"
                  "              separated, e.g. 01:0C,01:0D,09:02 (modes 01 and 09)\n"
                  "  --interval  ms between two requests (default 500, min 50)\n"
                  "  --record    also write everything received to a pcap file\n"
                  "              (Wireshark, cmp_replay)\n",
          prog, lsu::kDefaultPort);
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "10.10.10.10";
  uint16_t port = lsu::kDefaultPort;
  bool quiet = false;
  std::vector<std::pair<uint8_t, uint8_t>> requests;
  int intervalMs = 500;
  std::string recordPath;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) {
      port = uint16_t(std::stoi(argv[++i]));
    } else if (a == "--obd" && i + 1 < argc) {
      if (!parseObdList(argv[++i], requests)) {
        usage(argv[0]);
        return 2;
      }
    } else if (a == "--interval" && i + 1 < argc) {
      intervalMs = std::max(50, std::stoi(argv[++i]));
    } else if (a == "--record" && i + 1 < argc) {
      recordPath = argv[++i];
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

  uint64_t perSecond = 0;
  int64_t lastFrameMs = monoMs();
  bool haveStatus = false;
  lsu::Status prevStatus{};

  ObdDecoder obdDecoder;
  PcapWriter recorder;
  UdpRecord record{};  // addresses filled in once the socket is open
  uint64_t recorded = 0;

  CmpSubscriber::Callbacks cb;
  cb.onPacket = [&](const uint8_t* data, size_t size) {
    if (!recorder.isOpen()) return;
    record.timestampNs = uint64_t(unixNs());
    record.payload.assign(data, data + size);
    if (recorder.write(record)) {
      recorded++;
    } else {
      fprintf(stderr, "! write to %s failed, recording stopped\n", recordPath.c_str());
      recorder.close();
    }
  };
  cb.onFrame = [&](const cmp::DecodedCanPacket& p) {
    perSecond++;
    lastFrameMs = monoMs();
    if (!quiet) printFrame(p);
    std::string answer = obdDecoder.feed(p.frame, p.dataLength);
    if (!answer.empty()) printf("= %s\n", answer.c_str());
  };
  cb.onGap = [](unsigned lost) { printf("! sequence gap: %u packet(s) lost\n", lost); };
  cb.onStatus = [&](const lsu::Status& st) {
    printStatusChange(haveStatus ? &prevStatus : nullptr, st);
    prevStatus = st;
    haveStatus = true;
  };
  cb.onUnreachable = [] {
    printf("! no reply from ESP32 for 3 s: unreachable or not running\n");
  };
  cb.onReachable = [] { printf("ESP32 reachable again\n"); };
  cb.onRequestResult = [&](uint8_t mode, uint8_t pid, uint8_t result) {
    if (quiet && result == lsu::kRequestSent) return;
    std::string name = ObdDecoder::pidName(mode, pid);
    printf("> request %02X %02X%s%s: %s\n", mode, pid, name.empty() ? "" : " ",
           name.c_str(), lsu::requestResultName(result));
  };
  cb.onUnknownPacket = [](const uint8_t*, size_t size) {
    printf("? unexpected packet (%zu bytes)\n", size);
  };

  CmpSubscriber sub(cb);
  std::string error;
  if (!sub.open(host, port, &error)) {
    fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  if (!recordPath.empty()) {
    // As seen on the wire: from the ESP32 to this socket.
    if (!sub.endpoints(record.dstIp, record.dstPort, record.srcIp, record.srcPort) ||
        !recorder.open(recordPath, &error)) {
      fprintf(stderr, "%s\n", error.empty() ? "cannot read socket addresses" : error.c_str());
      return 1;
    }
    printf("recording to %s\n", recordPath.c_str());
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("subscribing to %s:%u (Ctrl+C to stop)\n", host.c_str(), port);

  int64_t lastReport = monoMs();
  int64_t lastIdle = monoMs();
  int64_t lastFlush = monoMs();
  int64_t nextRequestMs = 0;
  size_t nextRequest = 0;
  while (!stopRequested) {
    sub.poll(requests.empty() ? 100 : 10);

    int64_t now = monoMs();
    if (now - lastFlush >= 1000) {  // lose at most a second on a crash
      recorder.flush();
      lastFlush = now;
    }
    // Requests only once subscribed: the ESP32 refuses them otherwise.
    if (!requests.empty() && sub.hasStatus() && sub.reachable() && now >= nextRequestMs) {
      sub.sendRequest(requests[nextRequest].first, requests[nextRequest].second);
      nextRequest = (nextRequest + 1) % requests.size();
      nextRequestMs = now + intervalMs;
    }
    if (quiet && now - lastReport >= 1000) {
      const auto& c = sub.counters();
      printf("%llu frames/s, %llu total, %llu lost | CAN %s, bus errors %u\n",
             (unsigned long long)perSecond, (unsigned long long)c.frames,
             (unsigned long long)c.lost,
             sub.hasStatus() ? lsu::canStateName(sub.lastStatus().canState) : "?",
             sub.lastStatus().busErrors);
      perSecond = 0;
      lastReport = now;
    }
    if (now - lastFrameMs >= kIdleReportMs && now - lastIdle >= kIdleReportMs) {
      if (sub.reachable() && sub.hasStatus()) {
        const lsu::Status& st = sub.lastStatus();
        printf("... no CAN frames for %lld s; ESP32 ok, CAN %s, %u frames received "
               "since boot, %u bus errors\n",
               (long long)((now - lastFrameMs) / 1000), lsu::canStateName(st.canState), st.canRx,
               st.busErrors);
      }
      lastIdle = now;
    }
  }

  sub.close();
  const auto& c = sub.counters();
  printf("\nunsubscribed. %llu frames, %llu lost, %llu unexpected packets\n",
         (unsigned long long)c.frames, (unsigned long long)c.lost,
         (unsigned long long)c.unknown);
  if (!recordPath.empty())
    printf("recorded %llu packets to %s\n", (unsigned long long)recorded, recordPath.c_str());
  return 0;
}
