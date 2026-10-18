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
| UDP ports | 12670 (data: CMP only) and 12671 (control: own protocol), arbitrary, **not** IANA-registered |
| Destination | subscription model: clients subscribe to the ESP32, which sends unicast to each subscriber (up to 4 at once) |
| WiFi | [LsuWifi](README-WIFI.md) library v0.2.0: saved networks in priority order, follows network changes; default AP `lsu-cmp` / `12345678` at `10.10.10.10` |
| interfaceId | 0, arbitrary, a single physical CAN channel |
| Timestamp | laptop time, sent in every SUBSCRIBE |

The configuration is in [src/config.h](src/config.h).

## Subscription protocol (own, not part of ASAM CMP)

Control port 12671, big-endian. One definition shared by the firmware and
the C++ client: [lib/LsuProtocol/src/lsu_protocol.h](lib/LsuProtocol/src/lsu_protocol.h).
All messages below go to and come from the ESP32's control port; the CMP
packets come from its data port 12670, which carries nothing else. Until
2026-10-17 everything shared 12670, and Wireshark showed the own messages
as malformed CMP packets.

| Message | Direction | Content |
|---|---|---|
| SUBSCRIBE | laptop → ESP32 | `"LSUS"` + uint64 laptop time, ns since the Unix epoch (12 bytes) |
| UNSUBSCRIBE | laptop → ESP32 | `"LSUU"` (4 bytes) |
| STATUS | ESP32 → laptop | `"LSUA"` + version + CAN state and counters (40 bytes), reply to every SUBSCRIBE |
| REQUEST | laptop → ESP32 | `"LSUR"` + OBD2 mode + PID (6 bytes): put one OBD2 request on the bus |
| REQUEST_RESULT | ESP32 → laptop | `"LSUK"` + mode + PID + result (7 bytes), reply to every REQUEST |
| WATCH | laptop → ESP32 | `"LSUW"` + count + count × (mode, PID), up to 16: the mode 01 PIDs to poll for this client; resent every 4 s, replaces the previous list, no reply |

- Up to 4 subscribers at once (`kMaxSubscribers`), e.g. the phone app and
  a laptop recording; each one gets every CMP packet, with its own
  sequence counter and timestamps in its own clock. A SUBSCRIBE from a new
  client is ignored while all slots are taken (logged on the board).
  Until 2026-10-18 the latest sender of a SUBSCRIBE became the only
  subscriber, so two clients took the stream away from each other.
- The laptop resends SUBSCRIBE every second. After 5 s without one the
  subscriber expires and the ESP32 stops sending.
- On every SUBSCRIBE the ESP32 recomputes its clock offset. The CMP timestamp
  is the moment the CAN frame was received, in laptop time, accurate to about
  the WiFi latency (a few ms, no compensation). Nothing is sent without a
  subscriber, so every packet carries the `insync` flag.
- CMP packets are sent from port 12670 to the IP:port the SUBSCRIBE came
  from.
- STATUS holds the CAN controller state (running / bus-off / ...), bitrate,
  frames received, packets sent, frames dropped (full queues, overrun), UDP
  send errors, bus errors and uptime. This lets the client tell an
  unreachable ESP32 from a silent bus.

## OBD2 requests (SPEC.md §9)

- The ESP32 accepts a REQUEST from any current subscriber, only for
  mode 01 (current data) or 09 (vehicle information), and only when built
  with `CAN_ACK=1`. Anything else is refused with a REQUEST_RESULT saying
  why; nothing goes on the bus.
- **Live values: WATCH.** A client says once every 4 s which mode 01 PIDs
  it wants; the board polls the union of all clients' lists itself, round
  robin, one request every 20 ms (each of 8 PIDs about every 0.16 s), and
  every answer reaches every subscriber. A client's list is dropped 10 s
  after its last WATCH. Only the WATCH crosses the WiFi, so the rate on
  the bus is steady whatever the clients' WiFi does.
- **One-off values: REQUEST** (e.g. the VIN, `09 02`). Accepted requests
  wait in a queue of 8 and go before the watched PIDs. A request already
  waiting is not queued twice; only a full queue refuses as busy.
  REQUEST_RESULT "sent" means accepted; a transmit error later only shows
  in the board's stats (`tx_err`).
- **ECUs silent** (no answer for 2 s, e.g. ignition off): the board polls
  one watched PID per second instead of 50, and goes back to full rate at
  the first answer.
- History: until 2026-10-18 clients sent every request themselves; WiFi
  delivered them in bunches, and refusing the bunched ones (no queue) or
  queueing them still left values stalling now and then.
- It sends the request as a single frame on `0x7DF` (functional, all
  emission ECUs): `02 <mode> <PID>`, padded to 8 bytes with `0xAA`, as the
  engine ECU pads its answers.
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
CAN payload), as in SPEC.md §4, with two clarifications on the `id` field
(big-endian on the wire): `rsvd` is bit 29, `RTR` bit 30, `IDE` bit 31
(the `0xFFFFFF1F` mask in the openDAQ library applies to the raw word as
read on a little-endian host), and

- a 29-bit ID fills bits 0–28;
- an 11-bit ID sits in **bits 18–28**, as Wireshark 4.6 decodes it by
  default. The openDAQ library puts it in bits 0–10, which Wireshark calls
  "the old encoding" (preference `asam-cmp.use_old_canid_11bit_format`);
  so did this firmware until 2026-10-17. We have no access to the ASAM
  standard itself, so which one it specifies is not verified; Wireshark's
  default was chosen. The decoder reads both, so older recordings still
  play back correctly.

Apart from that bit position the encoder
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

   In the car: save the phone hotspot on the board's `/wifi` page and move
   it to priority 1. Then the board picks the hotspot when it is on, and
   switches by itself when its network goes away (about 20 s after the
   loss: e.g. hotspot off at home -> back to the router, driving away from
   home -> hotspot). The laptop, on the same hotspot, reaches the board by
   name: `cmp_client lsu-cmp.local` (mDNS, announced by ArduinoOTA; once,
   the name did not resolve on the first try). Tested 2026-10-18.
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

   OBD2: `--obd 01:0C,01:0D,09:02` (hex MODE:PID): the mode 01 PIDs go to
   the board as WATCH (renewed every 4 s), the others as REQUEST at the
   same pace. Answers are decoded
   on lines starting with `=` (`= ECU 7E8  01 0C  engine speed = 800.5
   rpm`), refused requests on lines starting with `>`. Decoded: supported
   PIDs (01 00, 20, 40, ...), a dozen common mode 01 PIDs, VIN and ECU
   name; anything else is shown in hex.

   Recording: `--record drive.pcap` also writes every datagram from the
   ESP32 (CMP, STATUS, REQUEST_RESULT) to a pcap file, with its arrival
   time in ns (see "Recording and playback" below).
4. Validation (SPEC.md §7): Wireshark 4.6 decodes port 12670 as ASAM CMP
   (verified with 4.6.6 on a recording). Wireshark 4.2.2 did not offer
   ASAM CMP for UDP at all.

## Recording and playback

```
tools/cmp_client/build/cmp_client 192.168.100.240 --obd 01:0C,01:0D --record drive.pcap
tools/cmp_client/build/cmp_replay drive.pcap [--speed 4] [--loop]
tools/cmp_client/build/cmp_client 127.0.0.1
```

`cmp_replay` plays the recording back as if it were the ESP32, on
`127.0.0.1`, data port 12670 and control port 12671 (`--bind`,
`--data-port`, `--control-port`), so any client works against it
unchanged, at home, without the car:

- It answers SUBSCRIBE with the recorded STATUS for the current position
  and sends the recorded CMP packets with their original spacing, scaled
  by `--speed`. Playback runs only while someone is subscribed, and pauses
  when the subscriber leaves.
- CMP timestamps are moved to the present, keeping each frame's original
  age (capture on the ESP32 → arrival on the laptop); `--keep-time` sends
  them unchanged.
- REQUESTs are refused with "transmit disabled": the requests and answers
  of the drive are already in the recording.
- `--loop` starts again at the end; sequence numbers keep counting up and
  the last STATUS stays, so the client sees no gap or restart.

Recordings are classic pcap with nanosecond timestamps and bare IPv4/UDP
headers (LINKTYPE_RAW). The player also reads Wireshark/tcpdump captures
in classic pcap (Ethernet or Linux cooked), not pcapng; it plays only the
datagrams sent from port 12670 or 12671.

Log: `pio device monitor` over WiFi (TCP port 23, 192.168.100.240 by
default; in AP mode `--port socket://10.10.10.10:23`), or
`pio device monitor -e usb` over USB. A TCP client gets the last 4 KB of log
on connect (including the boot messages); one TCP client at a time, no
password. LsuWifi's own messages only go to USB; the firmware reports each
network change on the TCP log too (`[net] lost '...'`, `[net] connected to
'...', IP ...`). Every 5 s the log prints
the statistics: frames received, dropped and sent, send errors, the TWAI
counters (`bus_err`) and the OBD2 requests, flow controls and transmit
errors. If `bus_err` grows a lot, try 250 kbit/s:
`build_flags = -DCAN_BITRATE_KBPS=250`.

## Test client and base for later clients

[tools/cmp_client/](tools/cmp_client/) (C++17, Linux, CMake):

| File | Role |
|---|---|
| [cmp_subscriber.h](tools/cmp_client/cmp_subscriber.h) / `.cpp` | The reusable part: socket, periodic SUBSCRIBE, STATUS, sequence tracking, unreachable detection, `sendRequest()`, `endpoints()`. No output, no threads; events through callbacks (`onPacket` for the raw datagrams, `onFrame`, `onGap`, `onStatus`, `onUnreachable`, `onReachable`, `onRequestResult`, `onUnknownPacket`). |
| [obd_decoder.h](tools/cmp_client/obd_decoder.h) / `.cpp` | OBD2 answers from the CAN frames: ISO-TP reassembly per ECU, values for common PIDs. |
| [main.cpp](tools/cmp_client/main.cpp) | The test client: command line and printing of the callbacks. |
| [pcap_file.h](tools/cmp_client/pcap_file.h) / `.cpp` | pcap writer (recording) and reader (playback). |
| [replay.cpp](tools/cmp_client/replay.cpp) | `cmp_replay`: plays a recording back as if it were the ESP32. |
| [lib/CmpCodec](lib/CmpCodec/src/) | CMP encoder (firmware) and decoder (client), shared. |
| [lib/LsuProtocol](lib/LsuProtocol/src/lsu_protocol.h) | The subscription protocol, shared. |
| [lib/LsuObd](lib/LsuObd/src/lsu_obd.h) | OBD2 IDs, request whitelist, request and flow control frames, shared. |

CMake builds the reusable part as the static library `cmp_subscriber`
(CmpSubscriber + decoder), which `cmp_client` links against. A new client
uses the same sources and writes its own `main`.

Host tests (encoder, decoder, protocol, OBD2 frames): `pio test -e native`.
pcap reader and writer: `ctest --test-dir tools/cmp_client/build`.

## Android app

[android/](android/) (Kotlin, Jetpack Compose, Android 10+; tested on a
Galaxy S23 Ultra with Android 16): a dashboard on the phone with large
tiles for engine speed, vehicle speed, intake manifold pressure, coolant,
voltage, throttle, MAF and load, plus the VIN. It speaks the same protocol
as `cmp_client` ([Protocol.kt](android/app/src/main/java/io/github/lsuciu70/lsucmp/Protocol.kt),
[ObdDecoder.kt](android/app/src/main/java/io/github/lsuciu70/lsucmp/ObdDecoder.kt),
ports of the C++ sources, with the same golden-byte tests). It asks the
board to poll those PIDs (WATCH) and requests the VIN until it arrives.

- **Finding the board:** with the IP field blank, SUBSCRIBE goes out as a
  broadcast on every network the phone is on; the board's STATUS reply
  gives its address. Meant for the car, where the board joins the phone's
  hotspot (add the hotspot as a network on the board's web page) and gets
  whatever IP the phone hands out. A fixed IP can be typed in instead.
- **At home without the car:** `cmp_replay drive.pcap --bind 0.0.0.0
  --loop` on the laptop; the app finds it the same way.
- Values older than 3 s are dimmed; the screen stays on while connected.

Build and install (Android SDK in `~/Android/Sdk`, USB debugging on):

```
cd android
./gradlew testDebugUnitTest assembleDebug
~/Android/Sdk/platform-tools/adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## Credits

Firmware, test client and documentation were developed together with
[Claude Code](https://claude.com/claude-code) (Claude Opus 5.5 by Anthropic).
Commits made this way carry a `Co-Authored-By: Claude` trailer.
