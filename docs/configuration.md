# Configuration

Copy `data/config.template.json` — or `templates/config.espgarden_s3.json` for
the `espgarden_s3` board — to `data/config.json` and fill in the values before
uploading the filesystem:

```jsonc
{
    "version": 1,
    "id": "1a2b",            // last 4 hex digits of ESP32 MAC (printed on boot)
    "hostname": "espgarden1",
    "timezone": "<-03>3",    // POSIX TZ string — adjust for your region
    "postalCode": "",        // where this device is; the firmware never reads it,
                             // the off-device tooling geocodes it for weather
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
        "channel": 123456,
        "moisture2Field": 0     // field for probe 2, 0 = keep it off the channel
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
        "fwTitle": "esp-garden",  // only firmware with this fw_title is flashed
        "publishSec": 300,        // periodic payload period, 60..300 s
        "heartbeatSec": 900       // step values are re-sent at least this often
    },
    "cloud": {
        // Cloud cover from the luminosity channel. OFF by default: the
        // clear-sky reference compiled into the firmware is an upper envelope
        // of ONE sensor's own history at ONE mounting (scripts/cloud_fit.py),
        // and on any other board every reading under it reads as cloud.
        "enabled": false
    },
    "et0": {
        // Daily reference evapotranspiration, Hargreaves-Samani. OFF by
        // default, and on the board this was written for it MEASURED no skill:
        // see scripts/et0_fit.py and the ET0 section below before enabling it.
        "enabled": false,
        "latitude": 0.0,     // degrees, positive north; no sensible default
        "scale": 1.0         // 1.0 is the textbook formula, nothing fitted
    },
    "log": {
        "level": 4               // 0 disable .. 4 info (default) .. 6 trace
    },
    "history": {                 // on-device append-only I/O snapshots
        // Total capacity, spread over 8 segments; 0 disables. 1440 = 24 h at
        // 60 s. The ceiling is 10000 (raised from 5000 in 2.20.0), which is
        // what the RECORD costs: 480 KB of segment files once every segment
        // has filled, LittleFS blocks included. Whether that fits is a
        // separate question and a per-device one, so the firmware also
        // measures it against the partition at boot and CLAMPS with a loud log
        // line naming both numbers. /config.json is never rewritten by that
        // clamp, and Status.History in /data.json says so for as long as it
        // applies. On device 6224 today, both 5000 and 10000 resolve to 3408
        // records; on the S3 carrier's 2432 KB filesystem 10000 is granted
        // whole.
        //
        // CHANGING THIS NO LONGER DESTROYS THE STORED HISTORY. It used to:
        // the per-segment capacity is in each segment's header and begin()
        // deleted anything that disagreed. Since 2.20.0 segments written under
        // the previous value are kept and read, and are recycled to the new
        // size as they age out — so a reduction converges downwards over one
        // full cycle of the eight slots instead of emptying the buffer at the
        // next boot, and the flash it frees comes back one segment at a time.
        "records": 1440,
        "periodSec": 60          // one record per this many seconds
    },
    "moisture": [                // one entry per probe
        // dry = reading in air, wet = submerged; equal values disable the
        // two-point fallback. relay = the pump that waters this probe, an
        // index into io.relays; -1 means none, and a probe with none never
        // gets a trained model.
        //
        // invert = the reading falls as the soil wets. True for the capacitive
        // v2 modules and the default; a resistive divider usually wants false.
        // Get it wrong and the number runs backwards without the classifier
        // noticing, because it accepts either direction.
        //
        // kind = a free label for what is in the pot. Nothing reads it to
        // decide anything: it is part of the trained model's IDENTITY, so
        // changing it throws that probe's statistics away. That is the way to
        // say "different sensor now" when the pin and the pump did not change.
        { "dry": 0, "wet": 0, "relay": 0, "invert": true, "kind": "capacitive-v2" },
        { "dry": 0, "wet": 0, "relay": 1, "invert": true },
        { "dry": 0, "wet": 0, "relay": 2, "invert": true }
    ],
    "io": {                      // GPIO pin overrides (optional)
        "button": 0,
        "relays": [              // index 0 is the watering relay on every board
            { "pin": 19, "on": 0, "name": "Watering" },
            { "pin": 16, "on": 0, "name": "Relay 2" },
            { "pin": 17, "on": 0, "name": "Relay 3" },
            { "pin": 18, "on": 0, "name": "Relay 4" }
        ],
        // Air temperature and humidity. A board has AT MOST ONE of these two,
        // and a document declaring both is not refused — it would brick a
        // device — but logs a warning, reads the SHT40 and ignores the DHT.
        // Both feed the same `temperature` / `airHumidity` keys: the same
        // quantity in the same units, measured more accurately, so no stored
        // series changes meaning. Which part is fitted is published as the
        // `ambient_sensor` ThingsBoard attribute and as /data.json's
        // Status."Ambient Sensor".
        "dht": 23,
        "sht4x": {               // SHT40 on I2C; omit the key if not fitted
            "name": "",          //   a PREFIX: one part, two channels
            "address": 68        //   0x44 (A suffix); 69/70 are B and C
        },
        "i2c": {                 // the BUS, not a sensor. Optional: absent
            "sda": 21,           //   keeps the compiled per-family default
            "scl": 22,           //   (21/22 on a WROOM-32, 8/9 on the S3).
            "hz": 100000         //   10 000..1 000 000; standard mode ships.
        },                       //   It is brought up only when an I2C DEVICE
                                 //   is declared, so this alone drives nothing.
        "soilMoisturePeriodSec": 1,       // how often the probe bank is
                                          //   sampled, in seconds. 1..10000;
                                          //   anything outside that is
                                          //   absurd, warned about at boot,
                                          //   and the previous value kept.
        "soilMoisture": [                 // bare pin, or an object:
            36,
            {"pin": 35, "name": "Bed 2",  // optional power gating: the
             "powerPin": 25,              //   probe is energised only
             "powerOn": 1,                //   while it is being read,
             "settleMs": 10},             //   which is what keeps a
            32                            //   resistive probe alive
        ],
        "luminosity": 39,
        "waterLevel": 34,
        "flow": {                // pulse flow meter; omit the key if not fitted
            "pin": 27,
            "name": "Flow",
            "pulsesPerLitre": 450
        },
        "floatSwitch": {         // reservoir level switch; omit if not fitted
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


## What the config decides, and what the build still decides

**Which peripherals a board has is configuration, not a build flag.** The
length of `io.relays` is how many relays it drives; the length of
`io.soilMoisture` is how many probes it reads; a single-instance sensor is
fitted if and only if its key exists in `io`. Adding a probe or a relay is an
edit in **`/devices.html`** followed by a restart — no rebuild, no serial cable.

Two things are still decided at build time, and cannot be otherwise:

- **The set of KINDS.** A DHT needs the DHT driver linked in, so no web page
  can add a kind this firmware has no code for. `GET /capabilities.json` lists
  the ones it does have.
- **The per-kind maximum** — `RELAY_MAX` (8) and `MOISTURE_MAX` (4) in
  `BuildConfig.h`. `MOISTURE_MAX` is pinned to the number of moisture slots in
  the history record by a `static_assert`; raising one without the other would
  give a probe no place in stored history.
- **Which UPLINKS exist** — `USE_THINGSPEAK` and `USE_TALKBACK`, both 0. Unlike
  a sensor, an uplink is a whole protocol with its own credential and its own
  socket, so it is not something a web page should be able to switch on. The
  flags build in both positions, and a config selecting a backend the image
  lacks is refused loudly rather than served silently — see
  [Telemetry backends](telemetry.md#telemetry-backends).

Every driver is compiled into every image. That was measured before it was
chosen: the minimal board came to 1.166 MB and the fully populated one to
1.187 MB, so the whole `HAS_*` split was buying 21 KB out of a 1.69 MB slot.

**Upgrading from a build-flag firmware:** a config written when the flags
decided everything may still carry `io` keys for sensors that used to be
compiled out — they were harmless dead weight then and mean "fitted" now.
Check the boot log, which states exactly what it decided:

```
[I] Sensors: 3 moisture, luminosity, DHT, water level, flow, float switch
```

If it lists something the board does not have, delete that key in
`/devices.html`. A phantom flow meter counts interference as flow; a phantom
float switch reads at its pull-up, which is "empty".

`io.relays` and `io.soilMoisture` also accept the pre-2.0 spelling — a scalar
`"watering"` / `"wateringOn"` pair and a scalar `"soilMoisture"` — so a config
written for a single-relay device still loads unchanged, as exactly one relay
and one probe.

The device ID is printed to the serial monitor on every boot (`ID: 1a2b`).
