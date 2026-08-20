# ESP Garden

Automatic garden irrigation and environmental monitoring system based on the ESP32, built with [PlatformIO](https://platformio.org) and integrated with the [ThingSpeak](https://thingspeak.com) IoT platform.

## Features

- **Automated irrigation** — timed watering triggered locally or remotely via ThingSpeak TalkBack
- **Multi-zone relay control** — up to 4 independently timed relays, switched off by a dedicated real-time task
- **Environmental monitoring** — soil moisture (up to 2 probes), luminosity, temperature, humidity, and water level
- **Local web UI** — dashboard, configuration editor and account management, served
  from the device with no CDN dependency (jQuery is bundled)
- **Per-probe moisture classification** — Dry / Humid / Wet from a two-point
  calibration, shown on the dashboard
- **Cloud logging** — sensor data published to ThingSpeak over MQTT every 2 minutes
- **Internet watchdog** — pings Google/Cloudflare DNS and reports connectivity losses
- **NTP time sync** — clock synchronized daily via Brazilian NTP pool
- **Authenticated web UI** — nonce + SHA-256 login with OPERATOR/ADMIN roles, per-IP lockout and persistent sessions
- **Over-The-Air firmware update** — via the web UI (ADMIN only)

## Hardware

| Environment | Board | Sensors | Relays |
|---|---|---|---|
| `espgarden1` | NodeMCU-32S | 3× soil moisture, luminosity, DHT11 (temp + humidity), water level | 4 |
| `espgarden2` | ESP32 DOIT DevKit v1 | Luminosity | 1 |
| `espgarden3` | ESP32 DOIT DevKit v1 | Soil moisture, luminosity | 1 |
| `espgarden4` | ESP32 DOIT DevKit v1 | — (base config) | 1 |
| `espgarden5` | ESP32 DOIT DevKit v1 | 2× soil moisture, luminosity, DHT11 | 4 |

### Sensors & Pinout (`espgarden5` defaults)

| Signal | Pin | Notes |
|---|---|---|
| Button | GPIO 0 | Boot button |
| Relay 1 — Watering | GPIO 19 | Active-low (`on = 0`) |
| Relay 2 | GPIO 16 | Active-low |
| Relay 3 | GPIO 17 | Active-low |
| Relay 4 | GPIO 18 | Active-low |
| DHT11 | GPIO 23 | Data line, 4.7–10 kΩ pull-up to 3V3 |
| Soil moisture 1 | GPIO 36 (`VP`, A0) | Capacitive sensor v2.0, % reported |
| Soil moisture 2 | GPIO 34 (A6) | Capacitive sensor v2.0, % reported |
| Luminosity | GPIO 39 (`VN`, A3) | 5 mm LDR in a divider with 10 kΩ to GND, % reported |
| Water level | GPIO 34 (A6) | Analog, converted via calibration curve (not on `espgarden5`) |

Two hardware rules constrain these choices:

- **Every analog input must be on ADC1 (GPIO 32–39).** ADC2 cannot be read while WiFi is associated.
- **GPIO 34–39 are input-only and have no internal pull-up** — fine for the LDR and the moisture probes, unusable for the DHT11 or a relay. Relay pins also avoid the strapping pins (0, 2, 5, 12, 15) and GPIO 6–11 (SPI flash).

Older boards keep the historical watering relay on GPIO 15; that is a strapping pin (MTDO) and new hardware should not reuse it.

`espgarden1` (NodeMCU-32S) uses: button 0 · relays **15, 16, 17, 18** ·
DHT11 23 · soil moisture **36, 35 and 32** · luminosity 39 · water level 34.
That spends five of the six ADC1 channels a WROOM-32 exposes (32–36, 39; 37 and
38 are not bonded out), leaving GPIO 33 for a fourth probe.
Its second probe is dashboard-only — field 4 is the water level there, so
publishing it needs an explicit `-D MOISTURE2_FIELD=<n>`. GPIO 16 and 17 are
free there because the ESP32-WROOM-32 has no PSRAM — on a WROVER module they are
not. `ConfigFile::validatePins()` logs every GPIO claimed by two peripherals and
every relay parked on an input-only pin (34–39) at boot.

Pin assignments can be overridden per device in `config.json` (see below).

## Getting Started

### Prerequisites

- [VS Code](https://code.visualstudio.com) + [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)
- Git

### Build & Flash

```bash
# Clone
git clone https://github.com/andrenepomuceno/esp-garden.git
cd esp-garden

# Build a specific environment
pio run -e espgarden1

# Upload firmware
pio run -e espgarden1 --target upload

# Upload filesystem (SPIFFS — config + web assets)
pio run -e espgarden1 --target uploadfs

# Open serial monitor
pio device monitor
```

### Configuration

Copy `data/config.template.json` to `data/config.json` and fill in the values before uploading the filesystem:

```jsonc
{
    "version": 1,
    "id": "1a2b",            // last 4 hex digits of ESP32 MAC (printed on boot)
    "hostname": "espgarden1",
    "timezone": "<-03>3",    // POSIX TZ string — adjust for your region
    "wifi": {
        "ssid": "your-wifi",
        "password": "your-password"
    },
    "ota": {
        "username": "admin",
        "password": "your-ota-password"
    },
    "thingSpeak": {
        "apiKey": "WRITE_API_KEY",
        "channel": 123456
    },
    "talkBack": {
        "apiKey": "TALKBACK_API_KEY",
        "channel": 123456        // TalkBack queue ID
    },
    "mqtt": {
        "clientID": "your-mqtt-client-id",
        "username": "your-mqtt-username",
        "password": "your-mqtt-password",
        "server": "mqtt3.thingspeak.com",
        "port": 8883,
        "cacert": "/thingspeak.pem"
    },
    "log": {
        "level": 4               // 0 disable .. 4 info (default) .. 6 trace
    },
    "moisture": [                // two-point calibration, one entry per probe
        { "dry": 0, "wet": 0 },  // dry = reading in air, wet = submerged
        { "dry": 0, "wet": 0 }   // equal values disable classification
    ],
    "io": {                      // GPIO pin overrides (optional)
        "button": 0,
        "relays": [              // index 0 is the watering relay on every board
            { "pin": 19, "on": 0, "name": "Watering" },
            { "pin": 16, "on": 0, "name": "Relay 2" },
            { "pin": 17, "on": 0, "name": "Relay 3" },
            { "pin": 18, "on": 0, "name": "Relay 4" }
        ],
        "dht": 23,
        "soilMoisture": [36, 34],
        "luminosity": 39,
        "waterLevel": 34
    }
}
```

`io.relays` and `io.soilMoisture` also accept the pre-2.0 spelling — a scalar
`"watering"` / `"wateringOn"` pair and a scalar `"soilMoisture"` — so a config
written for a single-relay device still loads unchanged. Entries beyond the
count compiled into the firmware (`RELAY_COUNT`, `MOISTURE_SENSOR_COUNT` in
`platformio.ini`) are ignored, and missing ones keep their compiled defaults.

The device ID is printed to the serial monitor on every boot (`ID: 1a2b`).

## Authentication

The web UI authenticates with a nonce + SHA-256 challenge — the password never
crosses the network, and the exchange cannot be replayed:

1. `GET /nonce?username=<u>` → `{nonce, salt, ttlMs}`
2. `passwordHash = sha256(salt + ":" + password)`
3. `response = sha256(nonce + ":" + passwordHash)`
4. `POST /login` with `username`, `nonce`, `response` → `{token, role}`
5. every later request sends the header `Authorization-Token: <token>`

`curl -u user:pass` does **not** work — there is no HTTP Basic path. Nonces are
one-shot and expire in 30 s, sessions idle out after 24 h, and five failed
attempts from one address return `429` for a minute.

**There is no default password in the firmware.** On first boot the device
migrates `ota.username` / `ota.password` from `config.json` into `/users.json`
as a salted SHA-256 ADMIN account, and `config.json` is never served over HTTP.
Change the password by updating `ota.*` and re-uploading the filesystem, having
first deleted `/users.json` on the device — the migration only runs for a
username that is not stored yet.

| Route | Role |
|---|---|
| `/data.json` | any signed-in user |
| `/control` | OPERATOR |
| `/config.json`, `/logs`, `/updateEnable`, `/update`, `/spiffs/*` | ADMIN |

### Changing settings without reflashing

`GET /config.json` returns the current configuration with every secret
(`wifi.password`, `ota.password`, `thingSpeak.apiKey`, `talkBack.apiKey`,
`mqtt.password`) replaced by `********`. Edit the document, send it back as the
`config` form parameter of `POST /config.json`, and any field still carrying the
mask keeps its stored value — so credentials never leave the device.

The write replaces the whole file, and the document's `id` must match the
device. Nothing re-reads the configuration at runtime: the response carries
`restartRequired: true`, and the change takes effect on the next boot
(`POST /control` with `reset=1`).

### Partition table

The firmware ships a custom 4 MB layout (`partitions/esp_garden_4mb.csv`) with
1.69 MB per OTA slot. **This cannot be delivered over OTA** — a board flashed
with the stock table needs one serial upload (`pio run -e <env> -t upload`) to
move to it.

## Soil Moisture Calibration

Each capacitive probe has its own gain and offset, so Dry / Humid / Wet cannot
come from a shared threshold — nor from channel history. A month of stored data
was fitted and rejected for this: a drying trend alone explains 86 % of the
variance, so clustering it into three groups just returns slices of that trend.

Calibrate each probe with two readings instead:

1. Hold the probe **in air** (connected — a disconnected probe floats to a rail
   and that value is useless) and note the value on the dashboard. That is `dry`.
2. Submerge it to its marked line in water. That is `wet`.
3. Enter both in **Config → moisture**, save and restart.

Until `dry` and `wet` differ the probe reports no state and the dashboard shows
no badge, rather than a made-up one.

To inspect a channel's history and see what it does and does not support:

```bash
python scripts/moisture_calibration.py --days 30
python scripts/moisture_calibration.py --days 30 --end 2023-03-08   # a past window
python scripts/moisture_calibration.py --dry 94.0 --wet 12.0        # emit bands
```

## Web Interface

| Page | Role | What it does |
|---|---|---|
| `/` | any signed-in user | Sensors, relay buttons, logs |
| `/config.html` | ADMIN | Edit the configuration in tabs; secrets stay masked |
| `/users.html` | ADMIN | Add, edit and remove accounts and roles |
| `/update.html` | ADMIN | Firmware and filesystem OTA |

## Remote Control (TalkBack)

Send commands to the device via the ThingSpeak TalkBack queue:

| Command | Description |
|---|---|
| `watering:<ms>` | Start the watering relay for `<ms>` milliseconds (max 20 000 ms) |
| `relay:<index>:<ms>` | Start relay `<index>` (0-based) for `<ms>` milliseconds (max 20 000 ms) |

Example: `watering:5000` triggers a 5-second watering cycle; `relay:2:3000`
runs the third relay for 3 seconds.

## Task Schedule

Tasks marked **critical** run on a dedicated FreeRTOS task; the rest share the
cooperative pump driven from `loop()`, where one blocking handler (ping,
TalkBack, an MQTT drain) stalls every other background task for its duration.

| Task | Period | Description |
|---|---|---|
| `relays` | 50 ms — **critical** | Switch each relay off when its timer expires |
| `ledBlink` | 1 s — **critical** | Blink built-in LED (enabled on config error) |
| `io` | 1 s | Read ADC sensors and rebuild the `/data.json` payload |
| `dht` | 10 s | Read temperature and humidity from DHT11 |
| `checkInternet` | 15 s | Ping DNS servers, update connectivity state |
| `mqtt` | 2 min | Publish averaged sensor data to ThingSpeak |
| `talkBack` | 5 min | Poll ThingSpeak TalkBack for remote commands |
| `clockUpdate` | 24 h | Re-sync NTP clock |
| `logBackup` | 1 h | Flush serial log to SPIFFS |
| `checkMoisture` | 4 h | Check soil moisture delta after watering |

## Dependencies

| Library | Purpose |
|---|---|
| [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) | Async HTTP server |
| [CriticalTaskScheduler](https://github.com/andrenepomuceno/CriticalTaskScheduler) | Cooperative task scheduler |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | MQTT client |
| [DHT sensor library](https://github.com/adafruit/DHT-sensor-library) | DHT11/22 driver |
| [ESP32Ping](https://github.com/marian-craciunescu/ESP32Ping) | ICMP ping |
| [Arduino_JSON](https://github.com/arduino-libraries/Arduino_JSON) | JSON parsing |

## Live Data

[ThingSpeak Live Data](https://thingspeak.com/channels/1348790)

## Photos

### Third Generation Prototype

![Third Generation Prototype](docs/prototype3.jpeg)

### Local Web Interface

![Local web UI](docs/ui.png)

## Useful Links

- [Arduino core for the ESP32](https://github.com/espressif/arduino-esp32)
- [Espressif IoT Development Framework](https://github.com/espressif/esp-idf)
- [PlatformIO Documentation](https://docs.platformio.org)
- [ThingSpeak Documentation](https://www.mathworks.com/help/thingspeak/)
