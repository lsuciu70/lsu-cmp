// ESP32-C3 CAN -> ASAM CMP UDP sender (SPEC.md).
//
// Tasks:
//   capture  - TWAI receive only (ACKs unless CAN_ACK=0, config.h),
//              timestamps frames, queues them
//              while a subscriber is active (SPEC.md §5.1)
//   sender   - one CMP packet per CAN frame, UDP to the subscriber (§5.2)
//   control  - receives SUBSCRIBE / UNSUBSCRIBE on the UDP port, replies
//              STATUS to every SUBSCRIBE
//   loop()   - LsuWifi web UI, ArduinoOTA, TCP log client and a periodic
//              stats log (net_log.h: USB serial + TCP port kLogTcpPort)
//
// Subscription protocol (own, not part of ASAM CMP): lsu_protocol.h.
// The sender of the latest SUBSCRIBE becomes the only subscriber, and the
// clock offset is reset from its time on every SUBSCRIBE. CMP timestamps are
// therefore laptop time, accurate to about the WiFi latency (a few ms; no
// latency compensation). Nothing is sent without a subscriber, so every
// packet sent carries the insync flag.

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <LsuWifi.h>
#include <driver/twai.h>
#include <esp_timer.h>
#include <lwip/sockets.h>

#include "cmp_encoder.h"
#include "config.h"
#include "lsu_protocol.h"
#include "net_log.h"

namespace {

struct QueuedFrame {
  int64_t rxUs;  // esp_timer time at reception
  cmp::CanFrame frame;
};

struct Subscriber {
  bool active = false;
  sockaddr_in addr{};
  int64_t offsetNs = 0;  // laptop time - esp_timer time
  int64_t lastSeenUs = 0;
};

const cmp::StreamConfig kStream = {kCmpDeviceId, kCmpStreamId, kCmpInterfaceId};

QueueHandle_t frameQueue;
int udpSocket = -1;

portMUX_TYPE subMux = portMUX_INITIALIZER_UNLOCKED;
Subscriber subscriber;  // guarded by subMux

volatile bool overflowPending = false;  // frames dropped since last packet
volatile bool otaInProgress = false;    // streaming paused during an update

// Counters for the stats log.
volatile uint32_t statRx = 0;
volatile uint32_t statNoSubscriber = 0;
volatile uint32_t statQueueFull = 0;
volatile uint32_t statSent = 0;
volatile uint32_t statSendErr = 0;

Subscriber getSubscriber() {
  portENTER_CRITICAL(&subMux);
  Subscriber s = subscriber;
  portEXIT_CRITICAL(&subMux);
  return s;
}

bool subscriberActive() {
  portENTER_CRITICAL(&subMux);
  bool active = subscriber.active;
  portEXIT_CRITICAL(&subMux);
  return active;
}

bool sameAddr(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void startTwai() {
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)kCanTxGpio, (gpio_num_t)kCanRxGpio,
      CAN_ACK ? TWAI_MODE_NORMAL : TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len = kTwaiRxQueueLen;
#if CAN_BITRATE_KBPS == 500
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
#elif CAN_BITRATE_KBPS == 250
  twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
#else
#error "CAN_BITRATE_KBPS must be 500 or 250"
#endif
  // Open filter: 11- and 29-bit IDs, no masking (SPEC.md §3).
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g, &t, &f);
  if (err == ESP_OK) err = twai_start();
  if (err != ESP_OK) {
    netLog.printf("[can] TWAI start failed: %s\n", esp_err_to_name(err));
    return;
  }
  netLog.printf("[can] TWAI %s, %d kbit/s, RX GPIO%d\n",
                CAN_ACK ? "normal (ACK, no transmit)" : "listen-only",
                CAN_BITRATE_KBPS, kCanRxGpio);
}

bool openSocket() {
  udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udpSocket < 0) {
    netLog.printf("[udp] socket() failed: errno %d\n", errno);
    return false;
  }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(kUdpPort);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(udpSocket, (sockaddr*)&local, sizeof(local)) < 0) {
    netLog.printf("[udp] bind(%u) failed: errno %d\n", kUdpPort, errno);
    close(udpSocket);
    udpSocket = -1;
    return false;
  }
  // Lets the control task wake up to expire a silent subscriber.
  timeval tv = {0, 200 * 1000};
  setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return true;
}

void captureTask(void*) {
  QueuedFrame q;
  twai_message_t msg;
  for (;;) {
    if (twai_receive(&msg, portMAX_DELAY) != ESP_OK) continue;
    q.rxUs = esp_timer_get_time();
    statRx++;
    if (!subscriberActive()) {
      statNoSubscriber++;
      continue;
    }
    q.frame.id = msg.identifier;
    q.frame.extended = msg.extd;
    q.frame.rtr = msg.rtr;
    q.frame.dlc = msg.data_length_code;
    memcpy(q.frame.data, msg.data, sizeof(q.frame.data));
    if (xQueueSend(frameQueue, &q, 0) != pdTRUE) {
      statQueueFull++;
      overflowPending = true;
    }
  }
}

void senderTask(void*) {
  QueuedFrame q;
  uint8_t packet[cmp::kMaxPacketSize];
  uint16_t sequenceCounter = 0;
  for (;;) {
    if (xQueueReceive(frameQueue, &q, portMAX_DELAY) != pdTRUE) continue;
    Subscriber s = getSubscriber();
    if (!s.active) continue;

    uint8_t flags = cmp::kFlagInsync;
    if (overflowPending) {
      overflowPending = false;
      flags |= cmp::kFlagOverflow;
    }
    uint64_t ts = (uint64_t)(q.rxUs * 1000 + s.offsetNs);
    size_t len = cmp::encodeCanPacket(kStream, sequenceCounter++, ts, flags,
                                      q.frame, packet);
    if (sendto(udpSocket, packet, len, 0, (sockaddr*)&s.addr,
               sizeof(s.addr)) == (int)len) {
      statSent++;
    } else {
      statSendErr++;  // shows as a sequenceCounter gap on the PC
    }
  }
}

lsu::Status currentStatus() {
  twai_status_info_t st{};
  twai_get_status_info(&st);
  lsu::Status s;
  s.canState = st.state;
  s.bitrateKbps = CAN_BITRATE_KBPS;
  s.canRx = statRx;
  s.sent = statSent;
  s.queueFull = statQueueFull;
  s.sendErrors = statSendErr;
  s.busErrors = st.bus_error_count;
  s.rxMissed = st.rx_missed_count;
  s.rxOverrun = st.rx_overrun_count;
  s.uptimeS = esp_timer_get_time() / 1000000;
  return s;
}

void sendStatus(const sockaddr_in& to) {
  uint8_t buf[lsu::kStatusSize];
  lsu::encodeStatus(currentStatus(), buf);
  sendto(udpSocket, buf, sizeof(buf), 0, (const sockaddr*)&to, sizeof(to));
}

void controlTask(void*) {
  uint8_t buf[32];
  for (;;) {
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    int n = recvfrom(udpSocket, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    int64_t nowUs = esp_timer_get_time();
    char ip[16];

    uint64_t laptopNs;
    if (n > 0 && lsu::decodeSubscribe(buf, n, laptopNs) && !otaInProgress) {
      portENTER_CRITICAL(&subMux);
      bool isNew = !subscriber.active || !sameAddr(subscriber.addr, from);
      subscriber.active = true;
      subscriber.addr = from;
      subscriber.offsetNs = (int64_t)laptopNs - nowUs * 1000;
      subscriber.lastSeenUs = nowUs;
      portEXIT_CRITICAL(&subMux);
      if (isNew) {
        inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
        netLog.printf("[sub] subscriber %s:%u\n", ip, ntohs(from.sin_port));
      }
      sendStatus(from);
    } else if (n > 0 && lsu::isUnsubscribe(buf, n)) {
      portENTER_CRITICAL(&subMux);
      bool removed = subscriber.active && sameAddr(subscriber.addr, from);
      if (removed) subscriber.active = false;
      portEXIT_CRITICAL(&subMux);
      if (removed) netLog.print("[sub] unsubscribed\n");
    }

    portENTER_CRITICAL(&subMux);
    bool expired = subscriber.active &&
                   nowUs - subscriber.lastSeenUs > (int64_t)kSubscriberTimeoutMs * 1000;
    if (expired) subscriber.active = false;
    portEXIT_CRITICAL(&subMux);
    if (expired) netLog.print("[sub] subscriber timed out\n");
  }
}

void logStats() {
  lsu::Status st = currentStatus();
  Subscriber sub = getSubscriber();
  char ip[16] = "-";
  if (sub.active) inet_ntoa_r(sub.addr.sin_addr, ip, sizeof(ip));
  netLog.printf(
      "[stats] can=%s rx=%lu no_sub=%lu q_full=%lu sent=%lu send_err=%lu | "
      "twai missed=%lu overrun=%lu bus_err=%lu | sub=%s\n",
      lsu::canStateName(st.canState), (unsigned long)st.canRx,
      (unsigned long)statNoSubscriber, (unsigned long)st.queueFull,
      (unsigned long)st.sent, (unsigned long)st.sendErrors,
      (unsigned long)st.rxMissed, (unsigned long)st.rxOverrun,
      (unsigned long)st.busErrors, ip);
}

void startOta() {
  ArduinoOTA.setHostname(kOtaHostname);
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
  ArduinoOTA.onStart([] {
    // Free the WiFi link for the upload: drop the subscriber and ignore new
    // SUBSCRIBEs until the board restarts.
    otaInProgress = true;
    portENTER_CRITICAL(&subMux);
    subscriber.active = false;
    portEXIT_CRITICAL(&subMux);
    netLog.print("[ota] update started, streaming stopped\n");
  });
  ArduinoOTA.onEnd([] { netLog.print("[ota] update done, restarting\n"); });
  ArduinoOTA.onError([](ota_error_t e) {
    netLog.printf("[ota] error %u\n", e);
    otaInProgress = false;
  });
  ArduinoOTA.begin();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  netLog.printf("[boot] lsu-cmp, reset reason %d\n", (int)esp_reset_reason());
  frameQueue = xQueueCreate(kFrameQueueLen, sizeof(QueuedFrame));
  startTwai();  // never transmits frames (at most ACK bits), safe before WiFi

  LsuWifi.begin();  // blocks until connected or the AP is up
  netLog.printf("[net] %s, IP %s, UDP port %u\n",
                LsuWifi.isApMode() ? "AP mode" : "station mode",
                LsuWifi.ip().toString().c_str(), kUdpPort);
  netLog.begin(kLogTcpPort);
  startOta();  // before the socket, so a bind failure can't block updates

  if (!openSocket()) return;
  xTaskCreate(captureTask, "capture", 3072, nullptr, 5, nullptr);
  xTaskCreate(senderTask, "sender", 4096, nullptr, 4, nullptr);
  xTaskCreate(controlTask, "control", 4096, nullptr, 3, nullptr);
}

void loop() {
  LsuWifi.loop();
  ArduinoOTA.handle();
  netLog.loop();
  static uint32_t lastStats = 0;
  if (millis() - lastStats >= kStatsIntervalMs) {
    lastStats = millis();
    logStats();
  }
  delay(2);
}
