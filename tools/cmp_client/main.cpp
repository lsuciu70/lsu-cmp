// Test client for the lsu-cmp ESP32: subscribes and prints what happens.
//
// All protocol work is in CmpSubscriber; this file only parses the command
// line and prints the callbacks: each CAN frame, sequence gaps, the ESP32
// status at subscription and whenever it changes (CAN state, bus errors,
// drops, restarts), and idle periods.
//
// Usage: cmp_client [host] [--port 12670] [--quiet]

#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <string>

#include "cmp_subscriber.h"

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
  uint16_t port = lsu::kDefaultPort;
  bool quiet = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) {
      port = uint16_t(std::stoi(argv[++i]));
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

  CmpSubscriber::Callbacks cb;
  cb.onFrame = [&](const cmp::DecodedCanPacket& p) {
    perSecond++;
    lastFrameMs = monoMs();
    if (!quiet) printFrame(p);
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
  cb.onUnknownPacket = [](const uint8_t*, size_t size) {
    printf("? unexpected packet (%zu bytes)\n", size);
  };

  CmpSubscriber sub(cb);
  std::string error;
  if (!sub.open(host, port, &error)) {
    fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("subscribing to %s:%u (Ctrl+C to stop)\n", host.c_str(), port);

  int64_t lastReport = monoMs();
  int64_t lastIdle = monoMs();
  while (!stopRequested) {
    sub.poll(100);

    int64_t now = monoMs();
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
  return 0;
}
