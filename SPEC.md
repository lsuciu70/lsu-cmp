# Specification: ESP32-C3 CAN → ASAM CMP UDP Sender

## 1. Purpose

Firmware for the ESP32-C3 that passively reads the CAN bus on a vehicle's OBD2 connector (through the internal TWAI controller), wraps every received CAN frame in the ASAM CMP (Capture Module Protocol) format, and sends the packets over UDP and WiFi to a data sink (PC) on the local network.

We transmit nothing on the CAN bus, we only listen (listen-only).

## 2. Hardware (confirmed, wired)

Three boards: ESP32-C3-MINI (dev board), WCMCU-230 (SN65HVD230 CAN transceiver), a step-down module (MP1584EN) for power.

| From | Pin | To | Pin |
|---|---|---|---|
| OBD2 | Pin 16 (+12V) | Step-down | IN+ |
| OBD2 | Pin 4/5 (GND) | Step-down | IN- |
| OBD2 | Pin 6 (CAN High) | WCMCU-230 | CANH |
| OBD2 | Pin 14 (CAN Low) | WCMCU-230 | CANL |
| Step-down | OUT+ (5V) | ESP32-C3 | 5V |
| Step-down | OUT- | ESP32-C3 | G |
| ESP32-C3 | 3V3 | WCMCU-230 | 3V3 |
| ESP32-C3 | G | WCMCU-230 | GND |
| ESP32-C3 | GPIO1 | WCMCU-230 | CTX (TWAI_TX) |
| ESP32-C3 | GPIO3 | WCMCU-230 | CRX (TWAI_RX) |

No 120Ω termination resistor on this node (the vehicle bus is already terminated at both physical ends).

Target vehicle: VW Golf 7, 2017, 1.6 TDI (MQB platform, Gateway J533). OBD2 pins 6/14 = the diagnostic bus, ISO 15765-4 standard.

## 3. TWAI configuration (ESP32-C3 internal CAN driver)

- **Mode: listen-only** (`TWAI_MODE_LISTEN_ONLY` in ESP-IDF). Mandatory: we never send ACKs or error frames on the live bus of a real vehicle.
- **Baud rate: 500 kbit/s** as the first test (ISO 15765-4 standard for most post-2008 diagnostic buses). If massive bit/frame errors show up, try 250 kbit/s.
- Classic CAN only (2.0A/B), no CAN-FD: neither the transceiver (SN65HVD230) nor the ESP32-C3 TWAI supports CAN-FD.
- We accept both 11-bit and 29-bit IDs (open filter, no masking on reception, at least during the initial test/traffic exploration phase).

## 4. ASAM CMP framing format, verified from source

**Source of truth used:** the open-source library `openDAQ/ASAM-CMP-Library` (Apache-2.0), read directly from its source code (not from memory/assumption). We have no access to the official ASAM PDF (paid/members only), so wherever the library does not explicitly confirm something, it is marked below as unknown.

All multi-byte fields are **big-endian (network byte order)**.

### 4.1 CmpHeader: 8 bytes, once per UDP packet

```
offset  size  field
0       1     version            (=1)
1       1     reserved           (=0)
2       2     deviceId           (to be configured, see §6)
4       1     messageType        (enum: undefined=0x00, data=0x01, control=0x02, status=0x03, vendor=0xFF)
5       1     streamId           (to be configured, see §6)
6       2     sequenceCounter    (incremented with every packet sent, per stream)
```
For our messages: `messageType = data (0x01)`.

### 4.2 MessageHeader: 16 bytes, immediately after CmpHeader

```
offset  size  field
0       8     timestamp          (nanoseconds since the Unix epoch, confirmed from the library example)
8       4     interfaceId        (logical identifier of the source CAN interface, e.g. 0 for the only channel)
12      1     commonFlags        (bit 0x01=recalc, 0x02=insync, 0x0C=seg mask, 0x10=diOnIf, 0x20=overflow, 0x40=errorInPayload)
13      1     payloadType        (see §4.4: the RAW 1-byte value, NOT the library's 16-bit enum)
14      2     payloadLength      (length of the payload that follows, in bytes)
```

**Note on payloadType:** the library internally defines a 16-bit enum (e.g. `can = 0x0101`), but the high byte (0x01) is actually the `messageType` already present in the CmpHeader, and the low byte (0x01) is the real value written in this 1-byte field. So for CAN: `payloadType (1 byte) = 0x01`.

### 4.3 CAN payload: 16-byte header + up to 8 data bytes (classic CAN)

```
offset  size  field
0       2     flags              (bitmask, see below)
2       2     reserved
4       4     id + RTR + IDE     (see bit packing below)
8       4     crc + SBC          (see bit packing below)
12      2     errorPosition
14      1     dlc                (raw DLC, 0-8 for classic CAN)
15      1     dataLength         (actual length of the included data, 0-8)
16      N     CAN data (N = dataLength)
```

**Bit packing of the `id` field (4 bytes, offset 4):**
- The actual CAN ID is masked with `0xFFFFFF1F` (exact bit packing, not just the first N bits; to be implemented exactly like the library, with its getId/setId functions)
- bit masked with `0x20` = reserved (rsvd)
- bit masked with `0x40` = RTR/RRS (remote request)
- bit masked with `0x80` = IDE (extended ID, 29-bit vs 11-bit)

> **Note (added after implementation):** these masks apply to the raw 32-bit word as read by the library on a little-endian host, not to the big-endian value on the wire. On the wire the `id` field holds the CAN ID in bits 0–28, `rsvd` in bit 29, `RTR` in bit 30 and `IDE` in bit 31. See [README.md, "CMP format"](README.md#cmp-format).

**Bit packing of the `crc` field (4 bytes, offset 8):** holds the CAN CRC + CRC support + SBC (stuff bit count) + parity. Fine details, less critical for a simple sender (we can put 0 if we do not compute the real CRC of the received frame; the ESP32 TWAI does not expose the raw frame CRC through the standard API; to be checked during implementation whether any workaround exists or whether it is left 0/unsupported).

**Flags (offset 0, 2 bytes):** per-frame error bits: crcErr=0x0001, ackErr=0x0002, passiveAckErr=0x0004, activeAckErr=0x0008, ackDelErr=0x0010, formErr=0x0020, stuffErr=0x0040, crcDelErr=0x0080, eofErr=0x0100, bitErr=0x0200, rsvd=0x0400, srrDom=0x0800, brs=0x1000 (CAN-FD, not applicable), esi=0x2000 (CAN-FD, not applicable).

### 4.4 Total size per CAN frame

`8 (CmpHeader) + 16 (MessageHeader) + 16 (CAN header) + dataLength` = minimum 40, maximum 48 bytes for a full classic CAN frame (8 data bytes). Fits in a single UDP datagram, no segmentation needed.

## 5. Proposed firmware architecture

1. **TWAI capture task**: TWAI driver initialized in listen-only mode, reads frames from the receive queue (blocking or with a short timeout), puts them into an internal FreeRTOS queue.
2. **Encode + send task**: takes frames from the queue, builds the CMP packet (CmpHeader + MessageHeader + CAN payload) as in §4, increments `sequenceCounter`, sends it through a UDP socket.
3. No need for large buffering: one CAN frame = one UDP packet, sent as soon as it is available (we do not aggregate several frames into one UDP packet, following the reference library's model where one CMP "frame" = one message).

## 6. Decisions/configuration REQUIRED before coding (do not assume; ask or choose explicitly and document the choice)

- **Toolchain:** native ESP-IDF (direct access to `driver/twai.h`) or the Arduino framework (through PlatformIO, which also has `twai.h` available on the recent ESP32 Arduino core). To be chosen explicitly: native ESP-IDF gives finer control, Arduino is faster to prototype.
- **deviceId / streamId:** arbitrary values chosen by us (there is no central ASAM registry for hobby projects), e.g. deviceId=1, streamId=1. To be clearly documented in the code as "chosen locally, not from an official registry".
- **Destination UDP port:** there is no confirmed standard IANA port for CMP; choose an arbitrary port (e.g. 50000) and document it as such, not as "standard".
- **Destination IP (data sink):** static IP configured in code, or broadcast/multicast? To be decided.
- **WiFi credentials:** hardcoded in code (simple, for a prototype) or through a configuration mechanism (e.g. WiFiManager)? To be decided based on workflow preference.
- **interfaceId (in MessageHeader):** fixed arbitrary value (e.g. 0) as long as there is a single physical CAN channel.

## 7. Validation

Wireshark has an official `asam-cmp` dissector: use it to confirm that the packets sent by the ESP32 are parsed correctly by a third-party tool, before assuming the encoding is correct based only on our own code.

## 8. Not covered by this document (uncertainties stated explicitly)

- The exact format of the CMP **Status/Control** messages (device discovery on the network): not verified, not needed for a simple data sender.
- The exact computation of the CAN CRC for the payload's `crc` field: the ESP32 TWAI might not expose this value raw; to be checked during implementation and, if not available, explicitly documented in the code that 0/unsupported is sent, not an invented value.
- 100% confirmation that the bus on pins 6/14 of the Golf 7 runs at 500 kbit/s (very likely, but not yet verified with a real capture).
