#!/usr/bin/env python3
"""Subscribe to the lsu-cmp ESP32 and print the received CAN frames.

Sends SUBSCRIBE ("LSUS" + laptop time in ns since the Unix epoch) every
second, which also keeps the ESP32 clock in sync with this machine, and
UNSUBSCRIBE ("LSUU") on exit. Decodes the ASAM CMP data packets and reports
gaps in sequenceCounter. Wireshark can capture the same traffic in parallel
(Decode As... UDP port 12670 -> ASAM CMP).

Usage: tools/cmp_client.py [host] [--port 12670] [--quiet]
"""

import argparse
import socket
import struct
import time

SUBSCRIBE_INTERVAL_S = 1.0
FLAG_NAMES = {0x02: "insync", 0x20: "overflow"}


def decode(pkt):
    if len(pkt) < 40:
        return None
    version, _, device_id, msg_type, stream_id, seq = struct.unpack_from(">BBHBBH", pkt, 0)
    ts, iface, cflags, ptype, plen = struct.unpack_from(">QIBBH", pkt, 8)
    _, _, id_word, _, _, dlc, dlen = struct.unpack_from(">HHIIHBB", pkt, 24)
    return {
        "seq": seq,
        "ts": ts,
        "iface": iface,
        "flags": cflags,
        "ok": version == 1 and msg_type == 0x01 and ptype == 0x01,
        "id": id_word & 0x1FFFFFFF,
        "rtr": bool(id_word & (1 << 30)),
        "ext": bool(id_word & (1 << 31)),
        "dlc": dlc,
        "data": pkt[40:40 + dlen],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", nargs="?", default="10.10.10.10", help="ESP32 IP (default: AP mode 10.10.10.10)")
    ap.add_argument("--port", type=int, default=12670)
    ap.add_argument("--quiet", action="store_true", help="only print a summary every second")
    args = ap.parse_args()

    esp = (args.host, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.1)

    next_sub = 0.0
    expected_seq = None
    count = lost = 0
    last_report = time.monotonic()
    try:
        while True:
            now = time.monotonic()
            if now >= next_sub:
                sock.sendto(b"LSUS" + struct.pack(">Q", time.time_ns()), esp)
                next_sub = now + SUBSCRIBE_INTERVAL_S
            try:
                pkt, _ = sock.recvfrom(2048)
            except socket.timeout:
                pkt = None

            if pkt:
                f = decode(pkt)
                if f is None or not f["ok"]:
                    print(f"? unexpected packet ({len(pkt)} bytes)")
                else:
                    count += 1
                    if expected_seq is not None and f["seq"] != expected_seq:
                        gap = (f["seq"] - expected_seq) & 0xFFFF
                        lost += gap
                        print(f"! sequence gap: {gap} packet(s) lost")
                    expected_seq = (f["seq"] + 1) & 0xFFFF
                    if not args.quiet:
                        t = time.strftime("%H:%M:%S", time.localtime(f["ts"] / 1e9))
                        ms = (f["ts"] // 1_000_000) % 1000
                        can_id = f"{f['id']:08X}" if f["ext"] else f"     {f['id']:03X}"
                        body = "RTR" if f["rtr"] else f["data"].hex(" ").upper()
                        flags = ",".join(n for b, n in FLAG_NAMES.items() if f["flags"] & b)
                        print(f"{t}.{ms:03d}  {can_id}  [{f['dlc']}]  {body:<23}  {flags}")

            if args.quiet and now - last_report >= 1.0:
                print(f"{count} frames/s, {lost} lost total")
                count = 0
                last_report = now
    except KeyboardInterrupt:
        pass
    finally:
        sock.sendto(b"LSUU", esp)
        sock.close()


if __name__ == "__main__":
    main()
