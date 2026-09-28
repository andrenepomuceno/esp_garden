# Telemetry and remote control

## Remote Control (TalkBack) — compiled out by default

**`USE_TALKBACK` ships at 0 and the whole path is behind it**: the task is not
registered, `talkback.cpp` compiles to nothing, and the API key is never sent.
Not only for the ~1.5 KB — `talkback.cpp` speaks **plain HTTP/1.1 on port 80
with the API key in the request body**, so every poll put a ThingSpeak write key
on the wire in clear text once a minute. [ThingsBoard RPC](#remote-commands-over-thingsboard-rpc)
covers the same commands over the MQTT/TLS link that is already up.

Set the flag to 1 and rebuild to get the behaviour below back.

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

**The ThingSpeak column is compiled out by default** (`USE_THINGSPEAK 0`). The
field numbering below stays documented because it is a permanent contract with
what is already stored in channel 1348790 — compiling the publisher out stops
*writing* to that channel, it does not erase it or release a single number.

**A build that cannot serve the configured backend REFUSES, loudly.** If
`mqtt.backend` says `"thingspeak"` on an image built with `USE_THINGSPEAK 0`,
the device logs a FATAL at boot, never opens a broker connection, queues no
payloads, and `/data.json` reports
`MQTT Link: unsupported backend 'thingspeak' — not in this build` for as long
as the mismatch lasts. It does **not** silently fall back to ThingsBoard: the
server, port, credentials and CA in the document belong to the backend that was
chosen, so a fallback would invent a destination nobody configured. It does
**not** refuse the config either — `loadFile()` returning false means compiled
defaults and an unreachable board, and the compiled default of `mqtt.backend`
is `"thingspeak"`, so that would brick every device. The config still loads;
only publishing stops.

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
`U_FLASH`. Use `/update.html` for `littlefs.bin`.
