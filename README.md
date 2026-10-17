# lsu-cmp

ESP32-C3 firmware that listens to the CAN bus on the OBD2 connector and
sends every frame, wrapped in ASAM CMP, over UDP and WiFi to a laptop. On
the laptop's request it also puts standard OBD2 requests (modes 01 and 09)
on the bus; it transmits nothing else, apart from ACKs (see `CAN_ACK`
below). Full specification: [SPEC.md](SPEC.md).

## Hardware

- ESP32-C3-MINI dev board, flashed and logged over the ESP32-C3's native USB
  (USB ID 303A:1001, no separate USB-serial chip), hence
  `ARDUINO_USB_MODE=1` / `ARDUINO_USB_CDC_ON_BOOT=1` in
  [platformio.ini](platformio.ini).
- WCMCU-230 CAN transceiver (SN65HVD230), powered from the ESP32's 3V3.
- MP1584EN step-down, 12 V from OBD2 pin 16 to 5 V for the ESP32.
- No 120 Ω termination on this node (the vehicle bus is already terminated).

Pin-by-pin wiring: [SPEC.md §2](SPEC.md#2-hardware-confirmed-wired). The
CAN GPIOs are in [src/config.h](src/config.h): TX = GPIO1 (CTX: ACKs,
error frames, OBD2 requests), RX = GPIO3 (CRX).

Things to keep in mind (not yet measured or verified):

- **OBD2 pin 16 is permanent +12 V**, also with the ignition off. The board
  stays powered with WiFi on, estimated at 30–50 mA from the 12 V battery,
  about 1 Ah per day. Fine for days, not for weeks: unplug it, or add a
  low-power mode for when the bus is silent, if it stays installed.
- **The MQB gateway (J533) does not forward the car's normal traffic to
  OBD2 pins 6/14.** First in-car test (2026-10-17, Golf 7, listen-only):
  wiring, supply and 500 kbit/s confirmed, but with ignition on the only
  traffic was ID `0x17F00010` (`20 10 00 00 00 00 00 00`) repeated back to
  back at 100% bus load: no node ACKed it, so its sender retransmitted it
  endlessly. Hence the default `CAN_ACK=1` (TWAI normal mode: ACKs, never
  transmits a frame). `-DCAN_ACK=0` goes back to listen-only, which on this
  bus only makes sense with a tester (VCDS, OBD Eleven, ...) connected at
  the same time to provide the ACKs. With ignition off the bus is silent.
  Second test (same day, `CAN_ACK=1`): the same frame arrives once every
  ~500 ms, from ignition on until ~20 s after ignition (or engine) off, with
  0 losses and 0 bus errors, and nothing else, engine running or not. The
  OBD2 bus carries no vehicle data unless a tester requests it.
  Third test (OBD2 requests, SPEC.md §9): the engine ECU answers on `0x7E8`
  within 3–6 ms, the VIN arrives through the ISO-TP flow control, and live
  values (rpm, boost, MAF, voltage, ...) follow the engine; 864 requests
  with 0 transmit errors and 0 bus errors.

## Decisions (SPEC.md §6)

| Decision | Choice |
|---|---|
| Toolchain | PlatformIO, Arduino framework; TWAI through ESP-IDF's `driver/twai.h` |
| TWAI mode | normal (`CAN_ACK=1`): ACKs frames, transmits only OBD2 requests and their flow control; `CAN_ACK=0` = listen-only, no requests |
| deviceId / streamId | 1 / 1, chosen locally, **not** from an official ASAM registry |
| UDP port | 12670, arbitrary, **not** an IANA-registered port for CMP |
| Destination | subscription model: the laptop subscribes to the ESP32, which sends unicast to the subscriber |
| WiFi | [LsuWifi](README-WIFI.md) library; default AP `lsu-cmp` / `12345678` at `10.10.10.10` |
| interfaceId | 0, arbitrary, a single physical CAN channel |
| Timestamp | laptop time, sent in every SUBSCRIBE |

The configuration is in [src/config.h](src/config.h).

## Subscription protocol (own, not part of ASAM CMP)

UDP port 12670, big-endian. One definition shared by the firmware and the
C++ client: [lib/LsuProtocol/src/lsu_protocol.h](lib/LsuProtocol/src/lsu_protocol.h).

| Message | Direction | Content |
|---|---|---|
| SUBSCRIBE | laptop → ESP32 | `"LSUS"` + uint64 laptop time, ns since the Unix epoch (12 bytes) |
| UNSUBSCRIBE | laptop → ESP32 | `"LSUU"` (4 bytes) |
| STATUS | ESP32 → laptop | `"LSUA"` + version + CAN state and counters (40 bytes), reply to every SUBSCRIBE |
| REQUEST | laptop → ESP32 | `"LSUR"` + OBD2 mode + PID (6 bytes): put one OBD2 request on the bus |
| REQUEST_RESULT | ESP32 → laptop | `"LSUK"` + mode + PID + result (7 bytes), reply to every REQUEST |

- The latest sender of a SUBSCRIBE becomes the only subscriber.
- The laptop resends SUBSCRIBE every second. After 5 s without one the
  subscriber expires and the ESP32 stops sending.
- On every SUBSCRIBE the ESP32 recomputes its clock offset. The CMP timestamp
  is the moment the CAN frame was received, in laptop time, accurate to about
  the WiFi latency (a few ms, no compensation). Nothing is sent without a
  subscriber, so every packet carries the `insync` flag.
- CMP packets are sent from port 12670 to the subscriber's IP:port.
- STATUS holds the CAN controller state (running / bus-off / ...), bitrate,
  frames received, packets sent, frames dropped (full queues, overrun), UDP
  send errors, bus errors and uptime. This lets the client tell an
  unreachable ESP32 from a silent bus.

## OBD2 requests (SPEC.md §9)

- The ESP32 accepts a REQUEST only from the current subscriber, only for
  mode 01 (current data) or 09 (vehicle information), at most one every
  50 ms, and only when built with `CAN_ACK=1`. Anything else is refused
  with a REQUEST_RESULT saying why; nothing goes on the bus.
- It sends the request as a single frame on `0x7DF` (functional, all
  emission ECUs): `02 <mode> <PID>`, padded to 8 bytes with `0x55`.
- ECUs answer on `0x7E8`..`0x7EF`. For a multi-frame answer (e.g. the VIN)
  the ESP32 sends the ISO-TP flow control to `0x7E0`..`0x7E7` itself, but
  only within 1 s of its own request, so it never answers for another
  tester.
- The request and the flow control are transmitted single shot (no
  automatic retransmission: a frame nobody ACKs, e.g. with the ignition
  off, is not repeated on the bus) and with self reception (they appear in
  the CMP stream next to the answers, with their real time).
- After a bus-off the ESP32 recovers and restarts the controller by itself.

## CMP format

One CAN frame = one UDP datagram of 40–48 bytes (CmpHeader + MessageHeader +
CAN payload), as in SPEC.md §4, with one clarification: the `0xFFFFFF1F` mask
in the openDAQ library is applied to the raw word as read on a little-endian
host. On the wire (big-endian) the `id` field holds the ID in bits 0–28,
`rsvd` in bit 29, `RTR` in bit 30 and `IDE` in bit 31. The encoder
([lib/CmpCodec](lib/CmpCodec/src/cmp_encoder.h)) was checked byte for byte
against packets built with openDAQ/ASAM-CMP-Library.

The `crc` field is 0, with `crcSupport`/`sbcSupport` = 0 (not supported):
TWAI does not expose the CRC or the stuff bit count of a received frame. The
`overflow` flag is set on the first packet after frames were dropped from
the internal queue.

## Usage

1. Build and upload over WiFi (OTA, to 192.168.100.240 by default):
   `pio run -t upload`. In AP mode: `--upload-port 10.10.10.10`. Over USB
   (first flash or recovery): `pio run -e usb -t upload`. CMP streaming
   stops while an OTA update is in progress.
2. At home the board connects as a station to the saved router (DHCP
   reservation 192.168.100.240). Where no saved network is in range it
   starts the `lsu-cmp` AP. Connect the laptop to it; more networks can be
   added at `http://10.10.10.10` (see [README-WIFI.md](README-WIFI.md)).
3. On the laptop, the C++ test client (Linux):
   ```
   cmake -S tools/cmp_client -B tools/cmp_client/build && cmake --build tools/cmp_client/build
   tools/cmp_client/build/cmp_client 192.168.100.240
   ```
   Without an IP it uses `10.10.10.10` (AP mode); `--quiet` prints only
   frames/s, losses and the CAN state. It prints every CAN frame (capture
   time, seq, `age` = time from capture on the ESP32 to arrival on the
   laptop, ID, DLC, data), sequence gaps, the ESP32 state at subscription
   and every change (CAN state, bus errors, dropped frames, restart), "no
   reply from ESP32" after 3 s without STATUS, and a summary every 5 s while
   no CAN frames arrive.

   OBD2: `--obd 01:0C,01:0D,09:02` sends these requests in turn (hex
   MODE:PID), one every `--interval` ms (default 500). Answers are decoded
   on lines starting with `=` (`= ECU 7E8  01 0C  engine speed = 800.5
   rpm`), refused requests on lines starting with `>`. Decoded: supported
   PIDs (01 00, 20, 40, ...), a dozen common mode 01 PIDs, VIN and ECU
   name; anything else is shown in hex.
4. Validation (SPEC.md §7): Wireshark on the same interface, *Decode As…*
   UDP port 12670 → ASAM CMP.

Log: `pio device monitor` over WiFi (TCP port 23, 192.168.100.240 by
default; in AP mode `--port socket://10.10.10.10:23`), or
`pio device monitor -e usb` over USB. A TCP client gets the last 4 KB of log
on connect (including the boot messages); one TCP client at a time, no
password. LsuWifi's own messages only go to USB. Every 5 s the log prints
the statistics: frames received, dropped and sent, send errors, the TWAI
counters (`bus_err`) and the OBD2 requests, flow controls and transmit
errors. If `bus_err` grows a lot, try 250 kbit/s:
`build_flags = -DCAN_BITRATE_KBPS=250`.

## Test client and base for later clients

[tools/cmp_client/](tools/cmp_client/) (C++17, Linux, CMake):

| File | Role |
|---|---|
| [cmp_subscriber.h](tools/cmp_client/cmp_subscriber.h) / `.cpp` | The reusable part: socket, periodic SUBSCRIBE, STATUS, sequence tracking, unreachable detection, `sendRequest()`. No output, no threads; events through callbacks (`onFrame`, `onGap`, `onStatus`, `onUnreachable`, `onReachable`, `onRequestResult`, `onUnknownPacket`). |
| [obd_decoder.h](tools/cmp_client/obd_decoder.h) / `.cpp` | OBD2 answers from the CAN frames: ISO-TP reassembly per ECU, values for common PIDs. |
| [main.cpp](tools/cmp_client/main.cpp) | The test client: command line and printing of the callbacks. |
| [lib/CmpCodec](lib/CmpCodec/src/) | CMP encoder (firmware) and decoder (client), shared. |
| [lib/LsuProtocol](lib/LsuProtocol/src/lsu_protocol.h) | The subscription protocol, shared. |
| [lib/LsuObd](lib/LsuObd/src/lsu_obd.h) | OBD2 IDs, request whitelist, request and flow control frames, shared. |

CMake builds the reusable part as the static library `cmp_subscriber`
(CmpSubscriber + decoder), which `cmp_client` links against. A new client
uses the same sources and writes its own `main`.

Host tests (encoder, decoder, protocol, OBD2 frames): `pio test -e native`.

## Credits

Firmware, test client and documentation were developed together with
[Claude Code](https://claude.com/claude-code) (Claude Opus 5.5 by Anthropic).
Commits made this way carry a `Co-Authored-By: Claude` trailer.
