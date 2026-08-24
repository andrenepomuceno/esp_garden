# ESP Garden

Automatic garden irrigation and environmental monitoring system based on the ESP32, built with [PlatformIO](https://platformio.org) and integrated with the [ThingSpeak](https://thingspeak.com) IoT platform.

## Features

- **Automated irrigation** — timed watering triggered locally or remotely via ThingSpeak TalkBack
- **Multi-zone relay control** — up to 4 independently timed relays, switched off by a dedicated real-time task
- **Environmental monitoring** — soil moisture (up to 3 probes), luminosity, temperature, humidity, water level, pulse flow meter and a reservoir float switch
- **Local web UI** — dashboard, configuration editor and account management, served
  from the device with no CDN dependency (jQuery is bundled)
- **Per-probe moisture classification** — Dry / Humid / Wet from a two-point
  calibration, shown on the dashboard
- **Scheduled watering** — up to 8 timed relay activations, edited in the web UI
- **Cloud logging** — sensor data published to ThingSpeak or ThingsBoard over MQTT (TLS)
- **On-device history** — a fixed-size ring buffer keeps the last N I/O snapshots
  across reboots, served as JSON
- **Internet watchdog** — pings Google/Cloudflare DNS and reports connectivity losses
- **NTP time sync** — clock synchronized daily via Brazilian NTP pool
- **Authenticated web UI** — nonce + SHA-256 login with OPERATOR/ADMIN roles, per-IP lockout and persistent sessions
- **Over-The-Air firmware update** — from the web UI (ADMIN only) or pushed from ThingsBoard over MQTT
- **Remote commands** — ThingsBoard RPC for relay control and status, with the same guards as the web UI

## Hardware

| Environment | Board | Sensors | Relays |
|---|---|---|---|
| `espgarden1` | NodeMCU-32S | 3× soil moisture, luminosity, DHT11 (temp + humidity), water level, flow, float switch | 4 |
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
| Flow meter | GPIO 27 | Pulse input, interrupt-counted; `pulsesPerLitre` sets the scale (`espgarden1`) |
| Float switch | GPIO 26 | Digital, internal pull-up; `activeLevel` sets which level means "raised" (`espgarden1`) |

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
        "cacert": "/thingspeak.pem",
        "backend": "thingspeak",  // or "thingsboard"
        "useTLS": true,           // false for a self-hosted broker on 1883
        "rpc": true,              // accept remote commands (ThingsBoard only)
        "fwUpdate": true,         // accept firmware pushed from the broker
        "fwTitle": "esp-garden"   // only firmware with this fw_title is flashed
    },
    "log": {
        "level": 4               // 0 disable .. 4 info (default) .. 6 trace
    },
    "history": {                 // on-device ring buffer of I/O snapshots
        "records": 1440,         // file capacity; 0 disables. 1440 = 24 h at 60 s
        "periodSec": 60          // one record per this many seconds
    },
    "moisture": [                // two-point calibration, one entry per probe
        { "dry": 0, "wet": 0 },  // dry = reading in air, wet = submerged
        { "dry": 0, "wet": 0 },  // equal values disable classification
        { "dry": 0, "wet": 0 }
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
        "soilMoisture": [36, 35, 32],
        "luminosity": 39,
        "waterLevel": 34,
        "flow": {                // pulse flow meter (HAS_FLOW_SENSOR)
            "pin": 27,
            "name": "Flow",
            "pulsesPerLitre": 450
        },
        "floatSwitch": {         // reservoir level switch (HAS_FLOAT_SWITCH)
            "pin": 26,
            "name": "Float Switch",
            "activeLevel": 0,    // logic level that means "raised"
            "interlock": false,  // refuse to run a pump on an empty reservoir
            "fillRelay": 3       // the relay that refills it, exempt; -1 = none
        }
    },
    "schedules": [               // up to SCHEDULE_COUNT (8) timed activations
        {
            "name": "Morning zone 1",
            "relay": 0,          // index into io.relays
            "hour": 6,
            "minute": 30,
            "days": 127,         // bitmask, bit 0 = Sunday; 127 = every day
            "durationMs": 10000, // 1..30 000 — the same ceiling startRelay applies
            "enabled": false     // absent means false
        }
    ]
}
```

Schedules are edited at **`/schedules.html`**, not through the generic config
editor — that editor renders a top-level array of objects as a read-only text
field. A schedule fires at most once per minute per entry, is skipped while the
clock is unsynced (the device will not water on a 1970 clock), and goes through
the same `startRelay()` as every other path, so the duration ceiling and the
already-running guard apply. Changes take effect after a restart.


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
| `/data.json`, `/history.json` | any signed-in user |
| `/schedules.html` | ADMIN (the page reads and writes `/config.json`) |
| `/control` | OPERATOR |
| `/config.json`, `/logs`, `/updateEnable`, `/update`, `/users.json`, `/users`, `/spiffs/*` | ADMIN |

`/logout` needs only a valid token. `/spiffs/users*`, `/spiffs/sessions*` and
`/spiffs/config*` answer 403 even to an ADMIN — the browse handler would
otherwise serve the salted password hashes, the live bearer tokens and the
plaintext WiFi and MQTT credentials.

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

### Reservoir interlock

`io.floatSwitch.interlock` makes `startRelay()` refuse a pump while the float
reads empty — the difference between a pump that runs dry for 30 s and one that
does not run at all. The check lives in `startRelay()`, not at the call sites,
so the web UI, TalkBack, a schedule and a ThingsBoard RPC all inherit it and
all get the same refusal back. `fillRelay` names the relay that refills the
reservoir; it is exempt, because blocking the one thing that fixes an empty
tank would deadlock the system the interlock exists to protect.

**It defaults to off, and that is not laziness.** A float that is not wired yet
sits at the internal pull-up, which reads as *empty* — switching this on by
default would stop every watering on every board that has the sensor compiled
in and not fitted. Check the reading on the dashboard first, then turn it on.
While it is blocking, `/data.json` says so in `Status.Interlock`, because a
refusal that leaves no trace is indistinguishable from a relay button that does
not work.

There is deliberately **no auto-refill**. A single float that fails in the
"empty" direction would hold the fill relay on, and the failure mode of that is
a flood rather than a dry pot.

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
| `/history.html` | any signed-in user | Charts over 1 h / 6 h / 12 h / 1 d / 7 d / 30 d |
| `/devices.html` | ADMIN | Name the sensors and relays; review the pin map |
| `/schedules.html` | ADMIN | Timed relay activations |

Two endpoint behaviours worth knowing, because neither has a button:

- **`GET /config.json?secrets=1`** (ADMIN) exports the configuration
  **unmasked**. Take this before any filesystem upload: `uploadfs` overwrites
  `/config.json`, and a backup taken through the normal masked `GET` cannot be
  restored — every credential comes back as eight asterisks and the loss only
  surfaces at the next boot, as a device that cannot join the network. The
  export is logged with the caller's IP.
- **`GET /history.json?window=<seconds>`** selects by time and decimates to fit
  `limit`, returning the `stride` it used. Without it a 24 h window is 1440
  records against a 200-record cap, so the page would silently show only the
  newest three hours. The relay mask is OR-ed across each decimation bucket
  rather than sampled — it is sticky precisely because a watering is seconds
  long, and sampling would drop seven activations in eight.

## Remote Control (TalkBack)

Send commands to the device via the ThingSpeak TalkBack queue:

| Command | Description |
|---|---|
| `watering:<ms>` | Start the watering relay for `<ms>` milliseconds (max 20 000 ms) |
| `relay:<index>:<ms>` | Start relay `<index>` (0-based) for `<ms>` milliseconds (max 20 000 ms) |

Example: `watering:5000` triggers a 5-second watering cycle; `relay:2:3000`
runs the third relay for 3 seconds.

## Telemetry backends

The broker connection is one `mqtt` block; only the topic and the payload
format change with `mqtt.backend`.

| | ThingSpeak | ThingsBoard |
|---|---|---|
| Topic | `channels/<id>/publish` | `v1/devices/me/telemetry` |
| Payload | form-encoded `field1=..&field2=..` | JSON, arbitrary keys |
| Channels | **8 fields, all taken** | unlimited |
| Auth | `mqtt.username` / `mqtt.password` | access token in `mqtt.username`, empty password |

**ThingsBoard is what unblocks the extra probes.** The ThingSpeak channel's
eight fields are spoken for, which is why soil probes 2 and 3 are dashboard-only
there; the JSON payload carries every probe, its Dry/Humid/Wet state, each relay,
the DHT error rate and the firmware version with no numbering to negotiate.

```jsonc
"mqtt": {
    "backend": "thingsboard",
    "server": "thingsboard.example.com",
    "port": 1883,
    "useTLS": false,          // TLS costs 30-45 KB of heap for the handshake
    "username": "<device access token>",
    "password": "",
    "clientID": "espgarden1"
}
```

Switching backend does not migrate history: the ThingSpeak channel keeps what it
has, and ThingsBoard starts empty.

### Remote commands over ThingsBoard (RPC)

With `mqtt.backend = "thingsboard"` and `mqtt.rpc = true`, the device answers
two-way RPC on `v1/devices/me/rpc/request/+`. Send them from the device's
**Rpc debug terminal** widget or the REST API.

| Method | Params | Answers |
|---|---|---|
| `getStatus` | — | firmware, hostname, uptime, connectivity, counters, relay array |
| `getRelays` | — | `[{index, name, on, remaining}]` |
| `startRelay` | `{"relay": 0, "seconds": 5}` or `{"relay": 0, "durationMs": 5000}` | `{ok, relay, durationMs}` |
| `startWatering` | `{"seconds": 5}` — relay 0 on every board | as above |
| `stopRelay` | `{"relay": 0}` | `{ok, relay, wasRunning}` |
| `getFirmware` | — | running version, accepted `fw_title`, update state |
| `checkFirmware` | — | re-asks the broker for the firmware shared attributes |
| `restart` | — | reboots from `loop()` shortly after answering |

Commands go through the same `startRelay()` as the web UI, so **the cloud gets
no privileged path to the pumps**: the 30 s ceiling, the range check and the
already-running guard all still apply, and a refusal comes back as
`{"ok": false, "error": "..."}` rather than silence.

### Firmware updates over ThingsBoard (FOTA)

With `mqtt.fwUpdate = true` the device subscribes to the firmware shared
attributes and downloads an assigned package over MQTT in 4 KB chunks
(`v2/fw/request/<id>/chunk/<n>`), verifies the MD5, and reboots. It reports
`fw_state` telemetry (`DOWNLOADING` → `DOWNLOADED` → `VERIFIED` → `UPDATING`,
or `FAILED` with `fw_error`), and publishes `current_fw_title` /
`current_fw_version` as client attributes on every connection — which is how
ThingsBoard learns the update landed.

Assign a package in ThingsBoard with **`Title` = `esp-garden`** (matching
`mqtt.fwTitle`), checksum algorithm **MD5**, and upload
`.pio/build/<env>/firmware.bin`. Any version different from the running one is
accepted, in either direction, so a rollback is just assigning the older package.

Four things it refuses to do:

- **Flash a package titled anything else.** One tenant holds every device an
  operator owns and assigning the wrong package is one wrong click; an image
  built for another board is a brick that needs USB to recover.
- **Start while a relay is energised.** The update ends in a reboot, and on an
  ESP32 reset every GPIO floats until `relayPinsSafeInit()` runs — which on an
  active-low relay board means the pump switches back on for the length of the
  boot. The download waits for an idle relay.
- **Share the flash with a browser upload.** Both paths drive one `Update`
  object, so `/updateEnable` answers `409` while a cloud download is running and
  the cloud download waits while a browser upload is in flight.
- **Hold the flash forever.** A stalled download re-requests its chunk five
  times and then aborts, releasing `Update` — otherwise an abandoned cloud FOTA
  would lock out the browser OTA that exists precisely to recover from one.

Progress appears in `/data.json`'s `Status` as `Cloud Update` while it runs.
Set `mqtt.fwUpdate = false` to switch the whole path off.

The filesystem image is **not** distributable this way — `handleUpdateUpload`
picks `U_SPIFFS` from the uploaded filename, and the FOTA path always writes
`U_FLASH`. Use `/update.html` for `spiffs.bin`.

## Task Schedule

Tasks marked **critical** run on a dedicated FreeRTOS task; the rest share the
cooperative pump driven from `loop()`, where one blocking handler (ping,
TalkBack, an MQTT drain) stalls every other background task for its duration.

| Task | Period | Description |
|---|---|---|
| `relays` | 50 ms — **critical** | Switch each relay off when its timer expires |
| `ledBlink` | 1 s — **critical** | Blink built-in LED (enabled on config error) |
| `io` | 1 s | Read ADC sensors and rebuild the `/data.json` payload |
| `dht` | 1 s | Read temperature and humidity from DHT11 |
| `checkInternet` | 15 s | Ping DNS servers, update connectivity state |
| `history` | `history.periodSec` | Append one I/O snapshot to the ring buffer |
| `schedules` | 20 s | Fire any schedule that is due |
| `mqtt` | 1 min | Publish averaged sensor data to the configured backend |
| `talkBack` | 1 min | Poll ThingSpeak TalkBack for remote commands |
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
