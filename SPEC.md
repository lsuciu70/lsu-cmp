# Specificație: ESP32-C3 CAN → ASAM CMP UDP Sender

## 1. Scop

Firmware pentru ESP32-C3 care citește pasiv magistrala CAN de pe conectorul OBD2 al unui vehicul (prin controllerul TWAI intern), încadrează fiecare cadru CAN primit în formatul ASAM CMP (Capture Module Protocol), și trimite pachetele prin UDP peste WiFi către un data sink (PC) din rețeaua locală.

Nu transmitem nimic pe magistrala CAN — doar ascultăm (listen-only).

## 2. Hardware (confirmat, cablat)

Trei plăci: ESP32-C3-MINI (dev board), WCMCU-230 (transceiver CAN SN65HVD230), un modul step-down (MP1584EN) pentru alimentare.

| De la | Pin | La | Pin |
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

Fără rezistor de terminare 120Ω pe acest nod (magistrala vehiculului are deja terminare la ambele capete fizice).

Vehicul țintă: VW Golf 7, 2017, 1.6 TDI (platformă MQB, Gateway J533). Pinii 6/14 OBD2 = magistrala de diagnoză, standard ISO 15765-4.

## 3. Configurație TWAI (driver CAN intern ESP32-C3)

- **Mod: listen-only** (`TWAI_MODE_LISTEN_ONLY` în ESP-IDF). Obligatoriu — nu trimitem niciodată ACK/cadre de eroare pe o magistrală vie a unui vehicul real.
- **Baud rate: 500 kbit/s** ca prim test (standard ISO 15765-4 pentru majoritatea magistralelor de diagnoză post-2008). Dacă apar erori masive de bit/frame, de încercat 250 kbit/s.
- Doar CAN clasic (2.0A/B), fără CAN-FD — nici transceiverul (SN65HVD230), nici TWAI-ul ESP32-C3 nu suportă CAN-FD.
- Acceptăm atât ID-uri pe 11 biți cât și pe 29 biți (filtru deschis, fără mascare la recepție, cel puțin în faza inițială de test/explorare a traficului).

## 4. Formatul de încadrare ASAM CMP — verificat din sursă

**Sursă de adevăr folosită:** biblioteca open-source `openDAQ/ASAM-CMP-Library` (Apache-2.0), citită direct din codul sursă (nu din memorie/presupunere). Nu avem acces la PDF-ul oficial ASAM (plătit/membri), deci acolo unde biblioteca nu confirmă explicit ceva, e marcat mai jos ca necunoscut.

Toate câmpurile multi-octet sunt **big-endian (network byte order)**.

### 4.1 CmpHeader — 8 octeți, o dată per pachet UDP

```
offset  size  field
0       1     version            (=1)
1       1     reserved           (=0)
2       2     deviceId           (de configurat, vezi §6)
4       1     messageType        (enum: undefined=0x00, data=0x01, control=0x02, status=0x03, vendor=0xFF)
5       1     streamId           (de configurat, vezi §6)
6       2     sequenceCounter    (incrementat la fiecare pachet trimis, per stream)
```
Pentru mesajele noastre: `messageType = data (0x01)`.

### 4.2 MessageHeader — 16 octeți, urmează imediat după CmpHeader

```
offset  size  field
0       8     timestamp          (nanosecunde de la epoch Unix — confirmat din exemplul bibliotecii)
8       4     interfaceId        (identificator logic al interfeței CAN sursă, ex. 0 pt. singurul canal)
12      1     commonFlags        (bit 0x01=recalc, 0x02=insync, 0x0C=seg mask, 0x10=diOnIf, 0x20=overflow, 0x40=errorInPayload)
13      1     payloadType        (vezi §4.4 — valoarea RAW de 1 octet, NU enumul pe 16 biți din bibliotecă)
14      2     payloadLength      (lungimea payload-ului care urmează, în octeți)
```

**Atenție la payloadType:** biblioteca definește intern un enum pe 16 biți (ex. `can = 0x0101`), dar octetul superior (0x01) e de fapt `messageType`-ul deja prezent în CmpHeader, iar octetul inferior (0x01) e valoarea reală scrisă în acest câmp de 1 octet. Deci pentru CAN: `payloadType (1 octet) = 0x01`.

### 4.3 Payload CAN — 16 octeți header + până la 8 octeți date (CAN clasic)

```
offset  size  field
0       2     flags              (bitmask, vezi mai jos)
2       2     reserved
4       4     id + RTR + IDE     (vezi bit-packing mai jos)
8       4     crc + SBC          (vezi bit-packing mai jos)
12      2     errorPosition
14      1     dlc                (DLC-ul brut, 0-8 pt. CAN clasic)
15      1     dataLength         (lungimea reală a datelor incluse, 0-8)
16      N     date CAN (N = dataLength)
```

**Bit-packing câmpul `id` (4 octeți, offset 4):**
- ID-ul CAN propriu-zis e mascat cu `0xFFFFFF1F` (bit-packing exact, nu doar primii N biți — de implementat identic cu biblioteca, cu funcțiile getId/setId)
- bit mascat cu `0x20` = rezervat (rsvd)
- bit mascat cu `0x40` = RTR/RRS (remote request)
- bit mascat cu `0x80` = IDE (extended ID, 29-bit vs 11-bit)

**Bit-packing câmpul `crc` (4 octeți, offset 8):** conține CRC-ul CAN + suport CRC + SBC (stuff bit count) + paritate — detalii fine, mai puțin critice pentru un sender simplu (putem pune 0 dacă nu calculăm CRC-ul real al cadrului recepționat; TWAI-ul ESP32 nu expune CRC-ul brut al cadrului prin API-ul standard, de verificat la implementare dacă există vreun workaround sau dacă se lasă 0/unsupported).

**Flags (offset 0, 2 octeți)** — biți de eroare pe cadru: crcErr=0x0001, ackErr=0x0002, passiveAckErr=0x0004, activeAckErr=0x0008, ackDelErr=0x0010, formErr=0x0020, stuffErr=0x0040, crcDelErr=0x0080, eofErr=0x0100, bitErr=0x0200, rsvd=0x0400, srrDom=0x0800, brs=0x1000 (CAN-FD, nu se aplică), esi=0x2000 (CAN-FD, nu se aplică).

### 4.4 Dimensiune totală per cadru CAN

`8 (CmpHeader) + 16 (MessageHeader) + 16 (CAN header) + dataLength` = minim 40, maxim 48 octeți pentru un cadru CAN clasic complet (8 octeți date). Încape într-un singur datagram UDP, fără nevoie de segmentare.

## 5. Arhitectură firmware propusă

1. **Task captură TWAI** — driver TWAI inițializat în listen-only, citește cadre din coada de recepție (blocking sau cu timeout scurt), le pune într-o coadă FreeRTOS internă.
2. **Task encodare + trimitere** — ia cadre din coadă, construiește pachetul CMP (CmpHeader + MessageHeader + payload CAN) conform §4, incrementează `sequenceCounter`, trimite prin socket UDP.
3. Fără nevoie de buffering mare — un cadru CAN = un pachet UDP, trimis imediat ce e disponibil (nu agregăm mai multe cadre într-un singur UDP, conform modelului bibliotecii de referință unde un "frame" CMP = un mesaj).

## 6. Decizii/configurări NECESARE înainte de cod (nu presupune, întreabă sau alege explicit și documentează alegerea)

- **Toolchain:** ESP-IDF nativ (acces direct la `driver/twai.h`) sau Arduino framework (prin PlatformIO, care are și el `twai.h` disponibil pe ESP32 Arduino core recent). De ales explicit — ESP-IDF nativ dă control mai fin, Arduino e mai rapid de prototipat.
- **deviceId / streamId:** valori arbitrare alese de noi (nu exthere un registru central ASAM pt. proiecte hobby) — ex. deviceId=1, streamId=1. De documentat clar în cod ca "alese local, nu dintr-un registru oficial".
- **Port UDP destinație:** nu există un port IANA standard confirmat pentru CMP — de ales un port arbitrar (ex. 50000) și documentat ca atare, nu ca "standard".
- **IP destinație (data sink):** IP static configurat în cod, sau broadcast/multicast? De decis.
- **Credențiale WiFi:** hardcodate în cod (simplu, pt. prototip) sau printr-un mecanism de configurare (ex. WiFiManager)? De decis pe baza preferinței de workflow.
- **interfaceId (în MessageHeader):** valoare arbitrară fixă (ex. 0) cât timp există un singur canal CAN fizic.

## 7. Validare

Wireshark are un dissector oficial `asam-cmp` — de folosit pentru a confirma că pachetele trimise de ESP32 sunt parsate corect de un tool terț, înainte de a presupune că encoding-ul e corect doar pe baza codului nostru.

## 8. Ce NU acoperă acest document (incertitudini asumate explicit)

- Formatul exact al mesajelor **Status/Control** CMP (descoperire device pe rețea) — neverificat, nu e nevoie pentru un sender simplu de date.
- Calculul exact al CRC-ului CAN pentru câmpul `crc` din payload — TWAI-ul ESP32 s-ar putea să nu expună această valoare brut; de verificat în timpul implementării și, dacă nu e disponibilă, de documentat explicit în cod că se trimite 0/unsupported, nu o valoare inventată.
- Confirmarea 100% că magistrala de pe pinii 6/14 ai Golf 7 e la 500 kbit/s (foarte probabil, dar nu verificat cu un capture real încă).
