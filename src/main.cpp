// ESP32-C3 CAN -> ASAM CMP UDP sender (SPEC.md).
//
// Tasks:
//   capture  - TWAI receive (ACKs unless CAN_ACK=0, config.h), timestamps
//              frames, queues them while a subscriber is active (SPEC.md
//              §5.1); sends the ISO-TP flow control for multi-frame OBD2
//              responses (lsu_obd.h)
//   sender   - one CMP packet per CAN frame, UDP from the data port to the
//              subscriber (§5.2)
//   control  - receives SUBSCRIBE / UNSUBSCRIBE / REQUEST on the control
//              port, replies STATUS / REQUEST_RESULT; queues whitelisted
//              OBD2 requests (SPEC.md §9)
//   requests - puts the queued OBD2 requests on the bus, spaced out
//   loop()   - LsuWifi web UI, ArduinoOTA, TCP log client and a periodic
//              stats log (net_log.h: USB serial + TCP port kLogTcpPort)
//
// Subscription protocol (own, not part of ASAM CMP): lsu_protocol.h.
// Up to kMaxSubscribers clients at once; each gets every CMP packet, with
// its own sequence counter, and timestamps in its own clock: the offset is
// reset from the client's time on each of its SUBSCRIBEs, so CMP timestamps
// are that client's time, accurate to about the WiFi latency (a few ms; no
// latency compensation). Nothing is sent without a subscriber, so every
// packet sent carries the insync flag.

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <LsuWifi.h>
#include <driver/twai.h>
#include <esp_timer.h>
#include <lwip/sockets.h>

#include <atomic>

#include "cmp_encoder.h"
#include "config.h"
#include "lsu_obd.h"
#include "lsu_protocol.h"
#include "net_log.h"

namespace {

struct QueuedFrame {
  int64_t rxUs;  // esp_timer time at reception
  cmp::CanFrame frame;
};

struct Subscriber {
  bool active = false;
  uint32_t id = 0;  // new for each subscription: the sender restarts its sequence
  sockaddr_in addr{};
  int64_t offsetNs = 0;  // laptop time - esp_timer time
  int64_t lastSeenUs = 0;
};

const cmp::StreamConfig kStream = {kCmpDeviceId, kCmpStreamId, kCmpInterfaceId};

QueueHandle_t frameQueue;
int dataSocket = -1;     // sends CMP from kDataPort
int controlSocket = -1;  // SUBSCRIBE / REQUEST in, STATUS / REQUEST_RESULT out

portMUX_TYPE subMux = portMUX_INITIALIZER_UNLOCKED;
Subscriber subscribers[kMaxSubscribers];  // guarded by subMux
uint32_t nextSubscriberId = 1;           // guarded by subMux

// Frames dropped since boot: each subscriber's next packet gets the overflow
// flag when this changed since its previous one.
std::atomic<uint32_t> dropCount{0};
volatile bool otaInProgress = false;    // streaming paused during an update

// Counters for the stats log.
volatile uint32_t statRx = 0;
volatile uint32_t statNoSubscriber = 0;
volatile uint32_t statQueueFull = 0;
volatile uint32_t statSent = 0;
volatile uint32_t statSendErr = 0;
volatile uint32_t statObdRequests = 0;
volatile uint32_t statFlowControls = 0;
volatile uint32_t statTxErr = 0;
volatile uint32_t statObdBusy = 0;

// Accepted OBD2 requests waiting for the bus (ring buffer, guarded by
// requestMux); requestTask sends them.
struct ObdRequest {
  uint8_t mode, pid;
};
portMUX_TYPE requestMux = portMUX_INITIALIZER_UNLOCKED;
ObdRequest requestQueue[kRequestQueueLen];
int requestHead = 0, requestCount = 0;
TaskHandle_t requestTaskHandle = nullptr;

// esp_timer ms of the last OBD2 request put on the bus, 0 = none yet.
std::atomic<uint32_t> lastRequestMs{0};

uint32_t nowMs() { return uint32_t(esp_timer_get_time() / 1000); }

bool sameAddr(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void getSubscribers(Subscriber out[kMaxSubscribers]) {
  portENTER_CRITICAL(&subMux);
  for (int i = 0; i < kMaxSubscribers; i++) out[i] = subscribers[i];
  portEXIT_CRITICAL(&subMux);
}

bool anySubscriber() {
  bool any = false;
  portENTER_CRITICAL(&subMux);
  for (int i = 0; i < kMaxSubscribers && !any; i++) any = subscribers[i].active;
  portEXIT_CRITICAL(&subMux);
  return any;
}

// Slot of the active subscriber at `addr`, or -1. Call inside subMux.
int findSubscriber(const sockaddr_in& addr) {
  for (int i = 0; i < kMaxSubscribers; i++)
    if (subscribers[i].active && sameAddr(subscribers[i].addr, addr)) return i;
  return -1;
}

bool isSubscriber(const sockaddr_in& addr) {
  portENTER_CRITICAL(&subMux);
  bool found = findSubscriber(addr) >= 0;
  portEXIT_CRITICAL(&subMux);
  return found;
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
                CAN_ACK ? "normal (ACK, OBD2 requests)" : "listen-only",
                CAN_BITRATE_KBPS, kCanRxGpio);
}

int openUdpSocket(uint16_t port) {
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) {
    netLog.printf("[udp] socket() failed: errno %d\n", errno);
    return -1;
  }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(port);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (sockaddr*)&local, sizeof(local)) < 0) {
    netLog.printf("[udp] bind(%u) failed: errno %d\n", port, errno);
    close(s);
    return -1;
  }
  return s;
}

bool openSockets() {
  dataSocket = openUdpSocket(kDataPort);
  controlSocket = openUdpSocket(kControlPort);
  if (dataSocket < 0 || controlSocket < 0) return false;
  // Lets the control task wake up to expire a silent subscriber.
  timeval tv = {0, 200 * 1000};
  setsockopt(controlSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return true;
}

// Queues one standard 11-bit frame for transmission. Self reception puts
// it into our own receive queue too, so it shows up in the CMP stream.
// Single shot: no automatic retransmission, so a frame nobody ACKs (bus
// asleep, ignition off) is not repeated back to back on the bus.
bool transmit(uint32_t id, const uint8_t data[8]) {
  twai_message_t m{};
  m.identifier = id;
  m.data_length_code = 8;
  m.self = 1;
  m.ss = 1;
  memcpy(m.data, data, 8);
  if (twai_transmit(&m, pdMS_TO_TICKS(10)) == ESP_OK) return true;
  statTxErr++;
  return false;
}

// Multi-frame OBD2 response to our request: tell the ECU to continue.
void maybeSendFlowControl(const twai_message_t& msg) {
  if (!CAN_ACK || !obd::isFirstFrame(msg.identifier, msg.extd, msg.data_length_code, msg.data))
    return;
  uint32_t last = lastRequestMs;
  if (last == 0 || nowMs() - last > kFlowControlWindowMs) return;
  uint8_t fc[8];
  obd::buildFlowControl(fc);
  if (transmit(obd::flowControlId(msg.identifier), fc)) statFlowControls++;
}

// Queues an accepted request, unless the same one is already waiting (its
// answer reaches every subscriber anyway). False if the queue is full.
bool queueRequest(uint8_t mode, uint8_t pid) {
  portENTER_CRITICAL(&requestMux);
  bool queued = false;
  for (int i = 0; i < requestCount && !queued; i++) {
    const ObdRequest& r = requestQueue[(requestHead + i) % kRequestQueueLen];
    queued = r.mode == mode && r.pid == pid;
  }
  bool ok = queued || requestCount < kRequestQueueLen;
  if (!queued && ok) {
    requestQueue[(requestHead + requestCount) % kRequestQueueLen] = {mode, pid};
    requestCount++;
  }
  portEXIT_CRITICAL(&requestMux);
  if (ok) xTaskNotifyGive(requestTaskHandle);
  return ok;
}

bool popRequest(ObdRequest& out) {
  portENTER_CRITICAL(&requestMux);
  bool any = requestCount > 0;
  if (any) {
    out = requestQueue[requestHead];
    requestHead = (requestHead + 1) % kRequestQueueLen;
    requestCount--;
  }
  portEXIT_CRITICAL(&requestMux);
  return any;
}

uint8_t handleRequest(const sockaddr_in& from, uint8_t mode, uint8_t pid) {
  if (!isSubscriber(from)) return lsu::kRequestNotSubscribed;
  if (!obd::isAllowedRequest(mode, pid)) return lsu::kRequestNotAllowed;
  if (!CAN_ACK) return lsu::kRequestTxDisabled;
  if (!queueRequest(mode, pid)) {
    statObdBusy++;
    return lsu::kRequestBusy;
  }
  return lsu::kRequestSent;
}

// Puts the queued requests on the bus, kMinRequestIntervalMs apart.
void requestTask(void*) {
  ObdRequest r;
  for (;;) {
    if (!popRequest(r)) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    uint8_t data[8];
    obd::buildRequest(r.mode, r.pid, data);
    // Before transmit: the first frame of the answer may come back quickly.
    uint32_t now = nowMs();
    lastRequestMs = now ? now : 1;
    if (transmit(obd::kFunctionalRequestId, data)) statObdRequests++;
    vTaskDelay(pdMS_TO_TICKS(kMinRequestIntervalMs));
  }
}

void captureTask(void*) {
  QueuedFrame q;
  twai_message_t msg;
  for (;;) {
    if (twai_receive(&msg, portMAX_DELAY) != ESP_OK) continue;
    q.rxUs = esp_timer_get_time();
    statRx++;
    maybeSendFlowControl(msg);
    if (!anySubscriber()) {
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
      dropCount++;
    }
  }
}

void senderTask(void*) {
  // Per slot, owned by this task: which subscription it is serving, its
  // next sequence number and the drop count its last packet reflected.
  struct Stream {
    uint32_t id = 0;
    uint16_t sequence = 0;
    uint32_t drops = 0;
  };
  Stream streams[kMaxSubscribers];
  Subscriber subs[kMaxSubscribers];
  QueuedFrame q;
  uint8_t packet[cmp::kMaxPacketSize];
  for (;;) {
    if (xQueueReceive(frameQueue, &q, portMAX_DELAY) != pdTRUE) continue;
    getSubscribers(subs);
    uint32_t drops = dropCount;
    for (int i = 0; i < kMaxSubscribers; i++) {
      if (!subs[i].active) continue;
      Stream& st = streams[i];
      if (st.id != subs[i].id) {  // new subscriber in this slot
        st.id = subs[i].id;
        st.sequence = 0;
        st.drops = drops;
      }

      uint8_t flags = cmp::kFlagInsync;
      if (st.drops != drops) flags |= cmp::kFlagOverflow;
      st.drops = drops;
      uint64_t ts = (uint64_t)(q.rxUs * 1000 + subs[i].offsetNs);
      size_t len = cmp::encodeCanPacket(kStream, st.sequence++, ts, flags, q.frame, packet);
      if (sendto(dataSocket, packet, len, 0, (sockaddr*)&subs[i].addr,
                 sizeof(subs[i].addr)) == (int)len) {
        statSent++;
      } else {
        statSendErr++;  // shows as a sequenceCounter gap on that client
      }
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
  sendto(controlSocket, buf, sizeof(buf), 0, (const sockaddr*)&to, sizeof(to));
}

void controlTask(void*) {
  uint8_t buf[32];
  for (;;) {
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    int n = recvfrom(controlSocket, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    int64_t nowUs = esp_timer_get_time();
    char ip[16];

    uint64_t laptopNs;
    uint8_t mode, pid;
    if (n > 0 && lsu::decodeRequest(buf, n, mode, pid)) {
      uint8_t reply[lsu::kRequestResultSize];
      lsu::encodeRequestResult(mode, pid, handleRequest(from, mode, pid), reply);
      sendto(controlSocket, reply, sizeof(reply), 0, (sockaddr*)&from, sizeof(from));
    } else if (n > 0 && lsu::decodeSubscribe(buf, n, laptopNs) && !otaInProgress) {
      portENTER_CRITICAL(&subMux);
      int slot = findSubscriber(from);
      bool isNew = slot < 0;
      for (int i = 0; i < kMaxSubscribers && slot < 0; i++)
        if (!subscribers[i].active) slot = i;
      if (slot >= 0) {
        Subscriber& s = subscribers[slot];
        if (isNew) {
          s.active = true;
          s.id = nextSubscriberId++;
          s.addr = from;
        }
        s.offsetNs = (int64_t)laptopNs - nowUs * 1000;
        s.lastSeenUs = nowUs;
      }
      portEXIT_CRITICAL(&subMux);
      inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
      if (slot < 0) {
        netLog.printf("[sub] %s:%u ignored, all %d slots taken\n", ip, ntohs(from.sin_port),
                      kMaxSubscribers);
      } else {
        if (isNew) netLog.printf("[sub] subscriber %s:%u\n", ip, ntohs(from.sin_port));
        sendStatus(from);
      }
    } else if (n > 0 && lsu::isUnsubscribe(buf, n)) {
      portENTER_CRITICAL(&subMux);
      int slot = findSubscriber(from);
      if (slot >= 0) subscribers[slot].active = false;
      portEXIT_CRITICAL(&subMux);
      if (slot >= 0) {
        inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
        netLog.printf("[sub] %s:%u unsubscribed\n", ip, ntohs(from.sin_port));
      }
    }

    for (int i = 0; i < kMaxSubscribers; i++) {
      portENTER_CRITICAL(&subMux);
      Subscriber& s = subscribers[i];
      bool expired = s.active && nowUs - s.lastSeenUs > (int64_t)kSubscriberTimeoutMs * 1000;
      if (expired) s.active = false;
      sockaddr_in addr = s.addr;
      portEXIT_CRITICAL(&subMux);
      if (expired) {
        inet_ntoa_r(addr.sin_addr, ip, sizeof(ip));
        netLog.printf("[sub] %s:%u timed out\n", ip, ntohs(addr.sin_port));
      }
    }
  }
}

void logStats() {
  lsu::Status st = currentStatus();
  Subscriber subs[kMaxSubscribers];
  getSubscribers(subs);
  char list[kMaxSubscribers * 16 + 1] = "";
  for (int i = 0; i < kMaxSubscribers; i++) {
    if (!subs[i].active) continue;
    char ip[16];
    inet_ntoa_r(subs[i].addr.sin_addr, ip, sizeof(ip));
    if (list[0]) strcat(list, ",");
    strcat(list, ip);
  }
  const char* ip = list[0] ? list : "-";
  netLog.printf(
      "[stats] can=%s rx=%lu no_sub=%lu q_full=%lu sent=%lu send_err=%lu | "
      "twai missed=%lu overrun=%lu bus_err=%lu | obd req=%lu busy=%lu fc=%lu tx_err=%lu | "
      "sub=%s\n",
      lsu::canStateName(st.canState), (unsigned long)st.canRx,
      (unsigned long)statNoSubscriber, (unsigned long)st.queueFull,
      (unsigned long)st.sent, (unsigned long)st.sendErrors,
      (unsigned long)st.rxMissed, (unsigned long)st.rxOverrun,
      (unsigned long)st.busErrors, (unsigned long)statObdRequests, (unsigned long)statObdBusy,
      (unsigned long)statFlowControls, (unsigned long)statTxErr, ip);
}

// Since we transmit, the controller can end up bus-off (e.g. requests with
// no other node to ACK them). Recover, then restart once it has.
void checkBusOff() {
  twai_status_info_t st{};
  if (twai_get_status_info(&st) != ESP_OK) return;
  if (st.state == TWAI_STATE_BUS_OFF) {
    if (twai_initiate_recovery() == ESP_OK) netLog.print("[can] bus-off, recovering\n");
  } else if (st.state == TWAI_STATE_STOPPED) {
    if (twai_start() == ESP_OK) netLog.print("[can] recovered, restarted\n");
  }
}

// LsuWifi follows the board between networks (e.g. home router -> phone
// hotspot) but logs only to USB; report each change on the TCP log too.
void logNetworkChanges() {
  static bool wasAp = false, wasConnected = false;
  static IPAddress lastIp;
  bool ap = LsuWifi.isApMode(), connected = LsuWifi.isConnected();
  IPAddress ip = LsuWifi.ip();
  if (ap == wasAp && connected == wasConnected && ip == lastIp) return;
  if (ap)
    netLog.printf("[net] AP mode, IP %s\n", ip.toString().c_str());
  else if (connected)
    netLog.printf("[net] connected to '%s', IP %s\n", LsuWifi.ssid().c_str(), ip.toString().c_str());
  else
    netLog.printf("[net] lost '%s'\n", LsuWifi.ssid().c_str());
  wasAp = ap;
  wasConnected = connected;
  lastIp = ip;
}

void startOta() {
  ArduinoOTA.setHostname(kOtaHostname);
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
  ArduinoOTA.onStart([] {
    // Free the WiFi link for the upload: drop the subscribers and ignore new
    // SUBSCRIBEs until the board restarts.
    otaInProgress = true;
    portENTER_CRITICAL(&subMux);
    for (Subscriber& s : subscribers) s.active = false;
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
  startTwai();  // transmits only on a REQUEST, which needs WiFi: safe before it

  LsuWifi.begin();  // blocks until connected or the AP is up
  netLog.printf("[net] %s, IP %s, UDP data port %u, control port %u\n",
                LsuWifi.isApMode() ? "AP mode" : "station mode",
                LsuWifi.ip().toString().c_str(), kDataPort, kControlPort);
  netLog.begin(kLogTcpPort);
  startOta();  // before the socket, so a bind failure can't block updates

  if (!openSockets()) return;
  xTaskCreate(captureTask, "capture", 3072, nullptr, 5, nullptr);
  xTaskCreate(senderTask, "sender", 4096, nullptr, 4, nullptr);
  xTaskCreate(requestTask, "requests", 3072, nullptr, 4, &requestTaskHandle);
  xTaskCreate(controlTask, "control", 4096, nullptr, 3, nullptr);
}

void loop() {
  LsuWifi.loop();
  logNetworkChanges();
  ArduinoOTA.handle();
  netLog.loop();
  static uint32_t lastStats = 0;
  if (millis() - lastStats >= kStatsIntervalMs) {
    lastStats = millis();
    logStats();
  }
  static uint32_t lastBusCheck = 0;
  if (CAN_ACK && millis() - lastBusCheck >= 100) {
    lastBusCheck = millis();
    checkBusOff();
  }
  delay(2);
}
