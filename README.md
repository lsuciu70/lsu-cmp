# lsu-cmp

Firmware ESP32-C3 care ascultă pasiv (listen-only) magistrala CAN de pe
OBD2 și trimite fiecare cadru, încadrat ASAM CMP, prin UDP peste WiFi către
un laptop. Specificația completă: [SPEC.md](SPEC.md).

## Decizii (SPEC.md §6)

| Decizie | Alegere |
|---|---|
| Toolchain | PlatformIO, framework Arduino; TWAI prin `driver/twai.h` din ESP-IDF |
| deviceId / streamId | 1 / 1 — alese local, **nu** dintr-un registru oficial ASAM |
| Port UDP | 12670 — arbitrar, **nu** un port IANA standard pentru CMP |
| Destinație | model cu abonare: laptopul se abonează la ESP, ESP trimite unicast la abonat |
| WiFi | biblioteca [LsuWifi](README-WIFI.md); AP implicit `lsu-cmp` / `12345678` pe `10.10.10.10` |
| interfaceId | 0 — arbitrar, un singur canal CAN fizic |
| Timestamp | ora laptopului, transmisă în fiecare SUBSCRIBE |

Configurația e în [src/config.h](src/config.h).

## Protocolul de abonare (propriu, nu face parte din ASAM CMP)

Laptop → ESP, UDP port 12670, big-endian:

| Mesaj | Conținut |
|---|---|
| SUBSCRIBE | `"LSUS"` + uint64 ora laptopului, ns de la epoch Unix (12 octeți) |
| UNSUBSCRIBE | `"LSUU"` (4 octeți) |

- Ultimul expeditor al unui SUBSCRIBE devine singurul abonat.
- Laptopul retrimite SUBSCRIBE la 1 s; după 5 s fără SUBSCRIBE abonatul expiră
  și ESP-ul nu mai trimite nimic.
- La fiecare SUBSCRIBE ESP-ul își recalculează offsetul de ceas. Timestamp-ul
  CMP e momentul recepției cadrului CAN în ora laptopului, cu precizie de
  ordinul latenței WiFi (câteva ms, fără compensare). Fără abonat nu se
  trimite nimic, deci toate pachetele au flag-ul `insync`.
- Pachetele CMP pleacă de pe portul 12670 către IP:portul abonatului.

## Formatul CMP

Un cadru CAN = un datagram UDP de 40–48 octeți (CmpHeader + MessageHeader +
payload CAN), conform SPEC.md §4, cu o precizare față de SPEC: masca
`0xFFFFFF1F` din biblioteca openDAQ se aplică pe cuvântul brut citit pe un
host little-endian. Pe fir (big-endian) câmpul `id` are ID-ul în biții
0–28, `rsvd` = bit 29, `RTR` = bit 30, `IDE` = bit 31. Encoderul
([lib/CmpEncoder](lib/CmpEncoder/src/cmp_encoder.h)) a fost verificat octet
cu octet față de pachete construite cu openDAQ/ASAM-CMP-Library.

Câmpul `crc` e 0, cu `crcSupport`/`sbcSupport` = 0 (nesuportat): TWAI nu
expune CRC-ul sau numărul de biți de umplere ai cadrului recepționat. Flag-ul
`overflow` e setat pe primul pachet de după cadre pierdute din coada internă.

## Utilizare

1. Build și upload prin WiFi (OTA, implicit către 192.168.100.240):
   `pio run -t upload`. În modul AP: `--upload-port 10.10.10.10`. Prin USB
   (primul flash sau recuperare): `pio run -e usb -t upload`. Cât timp o
   actualizare OTA e în curs, trimiterea CMP se oprește.
2. Acasă se conectează ca station la routerul salvat (rezervare DHCP
   192.168.100.240). Unde nu e nicio rețea salvată în raza, pornește AP-ul
   `lsu-cmp`. Laptopul se conectează la el; din `http://10.10.10.10` se pot
   adăuga rețele externe (vezi [README-WIFI.md](README-WIFI.md)).
3. Pe laptop: `tools/cmp_client.py` (implicit `10.10.10.10`), sau
   `tools/cmp_client.py <IP>` în modul station. `--quiet` afișează doar
   cadre/s și pierderile.
4. Validare (SPEC.md §7): Wireshark pe aceeași interfață, *Decode As…* UDP
   port 12670 → ASAM CMP.

Logul: `pio device monitor` prin WiFi (TCP port 23, implicit către
192.168.100.240; în modul AP `--port socket://10.10.10.10:23`), sau
`pio device monitor -e usb` prin USB. La conectare prin TCP se retrimit
ultimii 4 KB de log (inclusiv mesajele de la boot); un singur client TCP
odată, fără parolă. Mesajele interne LsuWifi apar doar pe USB. Logul afișează la 5 s statisticile: cadre primite, pierdute, trimise,
erori de trimitere și contoarele TWAI (`bus_err`). Dacă `bus_err` crește
mult, încearcă 250 kbit/s: `build_flags = -DCAN_BITRATE_KBPS=250`.

Teste encoder pe host: `pio test -e native`.
