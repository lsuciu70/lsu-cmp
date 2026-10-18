# lsu-esp32-wifi (LsuWifi)

WiFi manager library for ESP32 (PlatformIO, Arduino framework). Add it to any
project and the board can be moved to a new place and reconnected from a phone,
without reflashing.

- At boot it scans, then tries the saved external networks that are in range,
  in **priority order** (their order in the list; an "Up" button on the web
  page moves a network up), each up to 3 times (10 s per attempt). Until
  v0.2.0 the strongest signal won instead.
- After boot it follows the board when it moves (e.g. in a car, from the
  home network to a phone hotspot):
  - connection lost for 20 s (`LSUWIFI_RESCAN_AFTER_MS`): it scans again and
    tries the saved networks in range, in priority order, else starts the AP;
  - in AP mode, every 60 s (`LSUWIFI_AP_RESCAN_MS`): if a saved network is
    in range it leaves the AP and connects. Skipped while a client is on
    the AP, so a phone configuring the board is not disturbed.
  - while connected it stays on its network, even if one with a higher
    priority comes into range.
  - Set either interval to 0 to disable that check.
- If none connects (or none is saved) it starts its own access point:
  - IP `10.10.10.10`
  - SSID `lsu-esp32`, password `12345678`
- Web UI at `http://<ip>/wifi` (`/` redirects there), in both modes:
  - add / edit / delete external networks (up to 10)
  - change the AP SSID and password
  - Restart button (changes apply after restart)
- Settings are stored in NVS (namespace `lsuwifi`) and survive reflashing
  the application.
- Hostname on the external network: `lsuwifi-XXXX` (last MAC bytes), to find
  the board in the router's client list.

## Moving a board to a new place

1. Power it on. It finds no known network and starts the AP.
2. Connect a phone to `lsu-esp32` (password `12345678`).
3. Open `http://10.10.10.10`, add the local network, press **Restart**.
4. The board connects. Find its IP in the router (hostname `lsuwifi-XXXX`) or
   on the serial log (`[LsuWifi] connected to '...', IP ...`). A DHCP
   reservation in the router keeps the IP fixed. The MAC to reserve for is
   shown on the `/wifi` page (and in the serial log at boot).

## Adding it to a project

### 1. `platformio.ini`

```ini
lib_deps =
    git+ssh://git@github.com/lsuciu70/lsu-esp32-wifi.git#v0.2.0
```

The repo is private, so the SSH URL is used (it relies on the machine's
GitHub SSH key). The plain `https://` URL only works if the repo is made
public. `#v0.2.0` pins a release tag; drop it to follow `main`. While
changing the library and a project together, use the local checkout instead:
edits are picked up on the next build, with no push needed.

```ini
lib_deps =
    LsuWifi=symlink://../lsu-esp32-wifi
```

For ESP32-C3 / S3 boards flashed over their native USB, Serial output needs:

```ini
build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
```

### 2. Code: pick the case that fits the app

**A. The app has no web UI.** The WiFi page is the board's UI (`/` redirects
to `/wifi`). See `examples/basic`.

```cpp
#include <LsuWifi.h>

void setup() {
  Serial.begin(115200);
  LsuWifi.begin();  // blocks until connected or the AP is up
}

void loop() {
  LsuWifi.loop();  // serves the web UI; call it often
  // ... application ...
}
```

**B. The app has its own web UI.** The app owns `/` and puts a "WiFi setup"
entry in its menu. The WiFi page shows a link back to the app. Register the
app's routes on `LsuWifi.server()`. See `examples/with-app-ui`.

```cpp
void setup() {
  LsuWifi.setAppLink("/", "My app");  // before begin(); also frees "/"
  LsuWifi.begin();
  LsuWifi.server().on("/", handleAppHome);
  // ... other app routes ...
}
```

In the app's menu: `<a href="/wifi">WiFi setup</a>`. To show the WiFi state
in the app's own page, read `GET /wifi/status`:

```json
{"mode":"sta","connected":true,"ssid":"cls-router","ip":"192.168.100.240","mac":"AC:27:6E:5E:8E:E8","rssi":-48}
```

(`mode` is `"ap"` or `"sta"`; `rssi` is present only in `sta` mode.)

**C. The app has its own web UI on another server that needs port 80**
(`AsyncWebServer`, an HTTPS library's HTTP server, ...). Move LsuWifi to
another port with `build_flags = -DLSUWIFI_HTTP_PORT=8080`. The app's menu
links to `http://<host>:8080/wifi`, built in JS from `location.hostname`. Call
`setAppLink("/", ...)` as in case B. On a non-80 port, the back link points to
port 80 on the same host.

### 3. Rules for the host project

- **Do not touch WiFi directly.** No `WiFi.begin()`, `WiFi.mode()`,
  `WiFi.softAP()`, `WiFi.disconnect()` and no hard-coded credentials. LsuWifi
  owns the radio. When integrating into an existing project, remove its WiFi
  connect code.
- **One web server on port 80.** Prefer moving the app's routes onto
  `LsuWifi.server()` (cases A/B). If that's not practical, use case C. Never
  have two servers on the same port.
- **Reserved paths:** `/wifi` and everything under `/wifi/`, plus `/`
  unless `setAppLink("/", ...)` or `setRootRedirect(false)` is called before
  `begin()`. Routes match in registration order, so the redirect would win.
- **Keep `loop()` non-blocking.** Long `delay()`s or blocking reads make the
  web UI unresponsive.
- **`begin()` can take a while.** A scan takes about 2-3 s. Each saved network
  that is visible but fails costs up to 3 × 10 s. Start time-critical hardware
  (relays, outputs to a safe state) *before* `LsuWifi.begin()`.
- **Network features must handle AP mode.** In AP mode there is no internet or
  LAN. Guard MQTT, HTTP clients, NTP etc. with `LsuWifi.isConnected()`.
- **OTA** (`ArduinoOTA`): call `ArduinoOTA.begin()` after `LsuWifi.begin()`
  and `ArduinoOTA.handle()` in `loop()`. It works in both modes; in AP mode
  connect to the board's AP first and upload to `10.10.10.10`.
- **NVS:** do not use the `lsuwifi` namespace in the app. `pio run -t erase`
  wipes the saved networks (the board returns to AP defaults).

### API

| Call | Purpose |
| --- | --- |
| `LsuWifi.begin()` | Connect to a saved network or start the AP; start the web server |
| `LsuWifi.loop()` | Handle HTTP requests and a pending restart |
| `LsuWifi.isConnected()` | `true` when connected to an external network |
| `LsuWifi.isApMode()` | `true` when running as its own AP |
| `LsuWifi.ip()` | Current IP (`10.10.10.10` in AP mode) |
| `LsuWifi.server()` | The shared `WebServer` on port 80 |
| `LsuWifi.setAppLink(href, label)` | Before `begin()`: link from the WiFi page back to the app's UI; `"/"` also disables the root redirect |
| `LsuWifi.setRootRedirect(bool)` | Before `begin()`: whether `/` redirects to `/wifi` (default `true`) |
| `GET /wifi/status` | JSON with the WiFi state, for the app's own UI |

### Compile-time options

Set with `build_flags = -D...`:

| Macro | Default |
| --- | --- |
| `LSUWIFI_DEFAULT_AP_SSID` | `"lsu-esp32"` |
| `LSUWIFI_DEFAULT_AP_PASSWORD` | `"12345678"` |
| `LSUWIFI_MAX_NETWORKS` | `10` |
| `LSUWIFI_ATTEMPTS` | `3` |
| `LSUWIFI_CONNECT_TIMEOUT_MS` | `10000` |
| `LSUWIFI_HTTP_PORT` | `80` |

String values need escaped quotes: `-DLSUWIFI_DEFAULT_AP_SSID=\"garage\"`.
The defaults only apply until something is saved from the web UI.

## Limitations

- No password on the web UI. Anyone on the same network can change the settings.
- No captive portal: open `http://10.10.10.10` by hand.
- Hidden networks are not tried (they do not show up in the scan).
- Changes apply only after a restart.
- In AP mode it stays AP until restarted; it does not retry the saved networks.

## Developing this library

`pio run -t upload && pio device monitor` builds `examples/basic` against
`src/` and flashes it (ESP32-C3 env in `platformio.ini`). For the other
example: `PLATFORMIO_SRC_DIR=examples/with-app-ui pio run -t upload`.
