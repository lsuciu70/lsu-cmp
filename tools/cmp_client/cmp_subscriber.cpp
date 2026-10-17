#include "cmp_subscriber.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace {

int64_t monoMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint64_t unixNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

CmpSubscriber::CmpSubscriber(Callbacks callbacks) : CmpSubscriber(std::move(callbacks), Options{}) {}

CmpSubscriber::CmpSubscriber(Callbacks callbacks, Options options)
    : _cb(std::move(callbacks)), _opt(options) {}

CmpSubscriber::~CmpSubscriber() { close(); }

bool CmpSubscriber::open(const std::string& host, uint16_t port, std::string* error) {
  close();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* addr = nullptr;
  if (int err = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addr)) {
    if (error) *error = "cannot resolve " + host + ": " + gai_strerror(err);
    return false;
  }
  _sock = socket(AF_INET, SOCK_DGRAM, 0);
  // connect(): send to the ESP32 with send(), and receive only its packets.
  if (_sock < 0 || connect(_sock, addr->ai_addr, addr->ai_addrlen) < 0) {
    if (error) *error = std::string("socket: ") + strerror(errno);
    freeaddrinfo(addr);
    close();
    return false;
  }
  freeaddrinfo(addr);

  _nextSubscribeMs = 0;
  _lastStatusMs = monoMs();
  _reachable = true;
  _hasStatus = false;
  _haveSeq = false;
  _counters = {};
  return true;
}

void CmpSubscriber::close() {
  if (_sock < 0) return;
  send(_sock, lsu::kMagicUnsubscribe, lsu::kUnsubscribeSize, 0);
  ::close(_sock);
  _sock = -1;
}

bool CmpSubscriber::poll(int timeoutMs) {
  if (_sock < 0) return false;
  int64_t now = monoMs();
  if (now >= _nextSubscribeMs) sendSubscribe(now);

  // Wake up in time for the next SUBSCRIBE even with a long timeout.
  int untilSubscribe = int(_nextSubscribeMs - now);
  int wait = timeoutMs < untilSubscribe ? timeoutMs : untilSubscribe;
  pollfd pfd = {_sock, POLLIN, 0};
  if (::poll(&pfd, 1, wait < 0 ? 0 : wait) > 0) receive();

  checkTimeout(monoMs());
  return true;
}

bool CmpSubscriber::sendRequest(uint8_t mode, uint8_t pid) {
  if (_sock < 0) return false;
  uint8_t msg[lsu::kRequestSize];
  lsu::encodeRequest(mode, pid, msg);
  return send(_sock, msg, sizeof(msg), 0) == ssize_t(sizeof(msg));
}

bool CmpSubscriber::endpoints(uint32_t& localIp, uint16_t& localPort, uint32_t& peerIp,
                              uint16_t& peerPort) const {
  sockaddr_in local{}, peer{};
  socklen_t localLen = sizeof(local), peerLen = sizeof(peer);
  if (_sock < 0 || getsockname(_sock, (sockaddr*)&local, &localLen) < 0 ||
      getpeername(_sock, (sockaddr*)&peer, &peerLen) < 0)
    return false;
  localIp = ntohl(local.sin_addr.s_addr);
  localPort = ntohs(local.sin_port);
  peerIp = ntohl(peer.sin_addr.s_addr);
  peerPort = ntohs(peer.sin_port);
  return true;
}

void CmpSubscriber::sendSubscribe(int64_t nowMs) {
  uint8_t msg[lsu::kSubscribeSize];
  lsu::encodeSubscribe(unixNs(), msg);
  // Failures (e.g. network down) show up as onUnreachable.
  send(_sock, msg, sizeof(msg), 0);
  _nextSubscribeMs = nowMs + _opt.subscribeIntervalMs;
}

void CmpSubscriber::receive() {
  // Drain everything queued, so a burst of frames is handled in one poll().
  uint8_t buf[2048];
  for (;;) {
    ssize_t n = recv(_sock, buf, sizeof(buf), MSG_DONTWAIT);
    if (n < 0) return;  // EAGAIN, or ECONNREFUSED (nothing listening yet)
    handlePacket(buf, size_t(n));
  }
}

void CmpSubscriber::handlePacket(const uint8_t* data, size_t size) {
  if (_cb.onPacket) _cb.onPacket(data, size);
  uint8_t mode, pid, result;
  if (lsu::decodeRequestResult(data, size, mode, pid, result)) {
    if (_cb.onRequestResult) _cb.onRequestResult(mode, pid, result);
    return;
  }
  if (lsu::isStatus(data, size)) {
    lsu::Status st;
    if (!lsu::decodeStatus(data, size, st)) {
      _counters.unknown++;
      if (_cb.onUnknownPacket) _cb.onUnknownPacket(data, size);
      return;
    }
    _lastStatusMs = monoMs();
    _lastStatus = st;
    _hasStatus = true;
    if (!_reachable) {
      _reachable = true;
      if (_cb.onReachable) _cb.onReachable();
    }
    if (_cb.onStatus) _cb.onStatus(st);
    return;
  }

  cmp::DecodedCanPacket pkt;
  if (!cmp::decodeCanPacket(data, size, pkt)) {
    _counters.unknown++;
    if (_cb.onUnknownPacket) _cb.onUnknownPacket(data, size);
    return;
  }
  if (_haveSeq && pkt.sequenceCounter != _expectedSeq) {
    unsigned lost = uint16_t(pkt.sequenceCounter - _expectedSeq);
    _counters.lost += lost;
    if (_cb.onGap) _cb.onGap(lost);
  }
  _haveSeq = true;
  _expectedSeq = pkt.sequenceCounter + 1;
  _counters.frames++;
  if (_cb.onFrame) _cb.onFrame(pkt);
}

void CmpSubscriber::checkTimeout(int64_t nowMs) {
  if (_reachable && nowMs - _lastStatusMs >= _opt.statusTimeoutMs) {
    _reachable = false;
    if (_cb.onUnreachable) _cb.onUnreachable();
  }
}
