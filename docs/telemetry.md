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

## ThingSpeak and TalkBack are behind flags, and a config that asks for one is refused loudly

Both ship **OFF** (`USE_THINGSPEAK 0`, `USE_TALKBACK 0`). The live device publishes to `thingsboard.cloud` and nothing in production reached either path. **`USE_TALKBACK` already existed, at 1, and was honoured at exactly ZERO call sites.** The task was declared, registered, enabled and polling; `talkback.cpp` linked in whole. A flag like that is worse than no flag, because a reader who greps `BuildConfig.h` concludes the feature is off while the socket opens once a minute regardless. Every site is now behind it, `talkback.{h,cpp}` included. **Turning TalkBack off is a security fix, not a flash saving**: it speaks plain **HTTP/1.1 on port 80 with the API key in the request body** and carried a standing `// TODO use HTTPS`, so every poll put a ThingSpeak write key on the wire in clear text, once a minute, for the life of the device. ThingsBoard RPC does the same commands over the MQTT/TLS link already up. **The registration goes with the flag, not just the body** — a registered task whose handler returns at its first line still consumes one of the 16 slots in its bucket, and `addTask()` past the cap drops **silently**.

### What happens when `mqtt.backend` says `thingspeak` and the build has none

`mqttBackendSupported()` is the single predicate, and the answer is **refuse to publish** — stated in three places because each reaches a different reader. `mqttSetup()` logs `FATAL` and returns false before configuring the client. `mqttLoop()` returns above the WiFi check and above the backoff, so the device never sits on a broker it has no payload for: a connected session that publishes nothing is, from the broker's side, indistinguishable from a healthy device whose sensors all stopped. And **`/data.json` carries `MQTT Link: unsupported backend '<x>' — not in this build`**, which is **the one that matters**: the log is an 8 KB rolling buffer this device overwrites within hours, so a boot-time FATAL is gone by the time anyone asks why the channel is empty — and a dashboard reporting `MQTT: enabled` over a broker receiving nothing is the exact shape of the three-year outage under [Sensors, accumulators & telemetry](sensors.md), where only `Packages Sent` staying at 0 gave it away. `telemetryPublish()` also returns **above** the queue push: unlike `offline` there is nothing to wait for, since no amount of connectivity makes a backend appear in an image that lacks it.

**Falling back to ThingsBoard was rejected.** `mqtt.server`, `port`, `username`, `password` and the CA belong to the backend that was chosen; publishing ThingsBoard-shaped JSON to `v1/devices/me/telemetry` on `mqtt3.thingspeak.com` would connect, be dropped, and **report success** — a fabricated destination is worse than a refusal.

**Refusing the CONFIG was rejected, and this is the load-bearing half.** `loadFile()` returning false means compiled defaults, `ssid "undefined"`, and a board that cannot associate or be reached to fix — and the compiled default of `mqtt.backend` is still `"thingspeak"`, so a load-time refusal would brick every device whose document merely omits the key. **The parse is therefore untouched**: `thingSpeak` and `talkBack` still parse into fields nothing reads, and the 4-character credential check still counts both API keys, so *whether a field device's existing document loads is unchanged by which features were compiled in*. The keys are inert, not absent.

What stayed, and why: **the eight field constants in `include/core/telemetry.h` are NOT behind the flag**, because the numbering is a permanent contract with what is already stored in channel 1348790 — compiling the publisher out stops *writing* there, it does not erase it or release a number, so the contract outlives the flag and unused `static const`s cost no flash. **`validateThingSpeakFields()` still runs** for the same reason, warning that a set `moisture2Field` is inert rather than logging a boot line that would be untrue. And **`/data.json`'s `Channel` key is ABSENT, not `0` or `""`** — `index.js` **hides** the ThingSpeak link when it is missing, because a dead link that looks live is worse than no link, the same rule `state` and `fault` follow; `sim_state.py` mirrors it behind its own constant.

**Measured on `espgarden2`: -4 832 B flash and -236 B static RAM** (1 257 017 → 1 252 185; 66 804 → 66 568). Small against a 1.69 MB slot, which is the honest framing — the credential that stopped being broadcast in clear text is the return on this change, not the space.

## Sampling vs events — the rule, and where it was broken

**Anything whose duration is shorter than the publish period must be recorded as an EVENT or as a sticky flag. Never sampled.** A watering lasts five seconds and the payload is built once a minute, so an instantaneous read of `relayIsOn()` misses roughly eleven activations in twelve. The history record learned this when `g_relaySticky` was added; the telemetry kept sampling for months longer.

| | Use for | Example |
|---|---|---|
| **Event** | A transition an operator needs timestamped | relay started/stopped/refused, reservoir emptied, reboot, `fw_state`, a cloud transient, the daily `et0` |
| **Sticky flag** | "Did this happen at all during the period" | `relayNRan`, `relayRanMask`, `IO_HISTORY_FLAG_FLOAT_RAISED` |
| **Accumulator** | A quantity, where the mean or total is the answer | moisture, luminosity, flow rate, `flowTotalLitres`, ping | Events reach the broker through `tbPublishEvent()`, which appends to the same outbox `tbLoop()` drains every `loop()` iteration — so latency is milliseconds, not a minute.

- **The critical runner never builds one.** `relaysTick()` sets a bit in `g_relayPending[]` under `g_relayMux` and nothing else; the io task turns bits into messages at 1 Hz, where allocating a String is allowed. A 50 ms deadline and a JSON serialiser do not belong in the same function — that is the mistake that panicked this board with an interrupt watchdog.
- **The sticky mask is per CONSUMER.** It is take-and-clear, so one shared mask means whichever reader arrives first steals the event from the other: the history record would silently lose every watering the telemetry reported first. `relayStickyTake(RELAY_STICKY_HISTORY | _TELEMETRY)`.
- **`relayN` and `relayNRan` answer different questions** and both are published. The first drives a live indicator; the second is the only one that can see a watering that started and finished between two publishes.
- **The relay event carries NO bare `relay` key any more.** It held a ZERO-based index while every `relayN` key is one-based, so stored records read `{relay: "2", relayName: "Reservatorio", relay3Event: "started"}` and anything joining the two was off by one. It was **DELETED rather than renumbered**, deliberately: renumbering would give a key already in the stored history a second meaning with nothing in the data to say where the change happened, which is the more expensive mistake. The refusal event still carries a bare `relay` with the same zero-based problem — left alone because dropping it would leave a refusal with no index at all.

### Step values are published on CHANGE, and never sampled

A relay state, a reservoir contact, a reboot counter and a cumulative litre total are not quantities: they sit still and then step. Sampling one into every periodic payload spends a datapoint per key per tick restating what the cloud already knows — a value re-sent unchanged carries no information — and still reports the step LATE, by up to a whole publish period. **An asynchronous change belongs in asynchronous telemetry.** So these leave through `tbPublishEvent()` the moment they move, from the io task at 1 Hz, and are absent from the periodic payload entirely: `relay1..N`, `relay1..NRan`, `relayMask`, `relayRanMask`, `connectionLoss`, `wateringCycles`, `dhtErrorRate`, `flowRate`, `flowTotalLitres`, `reservoirRaised`, `cloudState`.

- **`cloudState` carries one exception**, the same one `moistureNFault` carries: it is published only while the model HAS an answer. Outside the fitted daylight window there is no reference and therefore no state, and a heartbeat restating "no reference" all night is precisely the waste this mechanism removes. **The transition BACK is still spent** — one publish of `no reference` at dusk — or an operator reading "latest" at midnight is told the sky is clear.
- **`mqtt.heartbeatSec` (default 900) is the floor under it.** A key that never changes would otherwise be absent from the cloud for as long as it stays put, and an operator reading "latest" cannot tell a stable value from a device that died a week ago. Every step key is re-sent at least this often whether it moved or not.
- **A float is compared against a DEADBAND, never with `!=`.** Two doubles representing the same measurement differ in their last bits for ever, so an exact comparison would publish on every tick and buy nothing. The deadband is the smallest change worth a datapoint on that channel — a question about the sensor, not about floating point: `flowRate` 0.05 L/min, `flowTotalLitres` 0.005 L, `dhtErrorRate` 0.5 points; counters and contacts pass 0.0, exact because their values are whole numbers a double holds exactly.
- **The heartbeat comparison is unsigned**, so `millis()` wrapping at 49 days costs one early heartbeat rather than stalling every key for another 49.
- **It runs on the io task, not the critical runner**, for the reason `publishRelayEvents()` does.
- **The comparison itself lives in `include/core/step_publisher.h`**, free of Arduino so `test_step_publisher` can reach it — same reason `segment_index.h` exists.
- **`relayStickyTake(RELAY_STICKY_TELEMETRY)` moved with it**, and there must stay exactly one caller: it is take-and-clear, so a second would steal activations from the first.

### `firmware` is not periodic telemetry

It was published in every payload: a string that changes at a reboot and at no other moment, restated ~1400 times a day. It is **gone from the periodic payload** and nothing replaced it, because two places already carry it — `tbOnConnect()` publishes `current_fw_version` as a CLIENT ATTRIBUTE (the key ThingsBoard itself reads to decide an update landed) and the boot event on the same connection carries `firmware` as TELEMETRY under the same key name. The timeseries still gets a point stamped at every moment the version could have changed.

### Still sampled, and whether that is fine

- **Moisture, luminosity, temperature, air humidity, water level, ping** — accumulated over the publish interval, and published three ways: the window mean under the original key, the instantaneous value under the same name plus **`Now`**, and the spread under **`Sd`**. The mean is what a trend is read from; the instantaneous value is the only one that can show a spike, and at 300 s a 1 Hz window is 300 samples deep, easily enough to flatten a watering into nothing.

  **`Sd` is the error bar, and it is a standard deviation rather than the variance the accumulator stores** — an error bar is drawn in the measurement's own units, and units-squared does not overlay a chart. It exists because the archive could not answer *"had this reading settled?"*: on 2026-09-03 a whole probe calibration had to be done by polling the live device at 30 s intervals, because every stored number was a mean with no spread beside it, and a mean of 74.0 at sd 0.05 is a different claim from a mean of 74.0 at sd 30. Cost: one key per continuous channel, +288 datapoints/day each — ~4 320/day becomes ~6 050, still a quarter of the 25 100 measured before any of this. It is NOT the cloud detector's statistic: `cloudVariability` is a normalised sample-to-sample STEP, `Sd` is the spread of the LEVEL, and over 5-minute windows a smooth ramp and a flickering sky are indistinguishable by level spread (both sd 3.2) while the step separates them 1.10 against 3.61.

  **The AVERAGE keeps the original key name, and that is the load-bearing half of the decision.** Every one of those keys has carried `getAverage()` since the ThingsBoard payload existed and there is stored history under them. Redefining one would leave a single series whose meaning changes part-way through with nothing in the data to say where — the same defect as renumbering a relay index, and harder to notice, because a number that merely shifts still looks like a reading.
- **`moistureNFault`** — published only when there IS a fault, exactly as `/data.json` omits the field. A key saying "fine" on every healthy probe every period is the waste this section is about, and it trains the eye to skip the one case that matters. The exception is the transition BACK: without one publish of the cleared verdict the cloud shows the last fault for ever.
- **`Min Free Heap`** — already a low-water mark, the sticky form of a quantity. A spike between publishes is caught; `Free Heap` alone would not.
- **`cloudVariability`** — the mean |dk| per minute over the publish period, the only cloud quantity that is a LEVEL rather than a transition; see [Cloud cover](cloud-cover.md). It is take-and-clear with exactly ONE caller, for the reason `relayStickyTake()` has exactly one.
- **The moisture CLASS** (Dry/Humid/Wet) — sampled. Defensible while the soil moves slowly, but a watering causes a fast transition, so a class-change event is the obvious next one if a badge is ever seen to skip a state.
- **Login failures and per-IP lockouts** — only in the 8 KB rolling log, which a busy device overwrites within hours. These are security events on a device exposed to a LAN and they have no durable record anywhere.
- **Config writes and restarts** — same: an audit trail that lives only in a log that rotates.

## ThingsBoard downlink — RPC and firmware over MQTT

`src/thingsboard.cpp`. Inert unless `config.mqttBackend == "thingsboard"`; the ThingSpeak path never reaches it. Two config gates: `mqtt.rpc` and `mqtt.fwUpdate`, both default true, plus `mqtt.fwTitle` (default `esp-garden`).

| Topic | Direction | Purpose |
|---|---|---|
| `v1/devices/me/attributes` | both | client attributes out (`current_fw_title`/`current_fw_version`/**`ambient_sensor`**), shared-attribute push in |
| `v1/devices/me/attributes/request/<id>` → `.../response/<id>` | out/in | ask for the `fw_*` shared keys on every connect |
| `v1/devices/me/rpc/request/+` → `.../response/<id>` | in/out | `getStatus`, `getRelays`, `startRelay`, `startWatering`, `stopRelay`, `getFirmware`, `checkFirmware`, `restart` |
| `v2/fw/request/<id>/chunk/<n>` → `v2/fw/response/...` | out/in | the firmware stream, 4 KB per packet |

**TRAP — nothing may publish from inside the PubSubClient callback.** PubSubClient uses ONE buffer for both directions, and the `payload` handed to the callback points into it; publishing from there overwrites the packet being processed. `tbHandleMessage()` therefore only records intent and every outbound message leaves from `tbLoop()`, which `mqttLoop()` calls *after* `mqttClient.loop()` returns. The outbox is a bounded `std::vector` and a full one logs an error rather than dropping silently — a lost RPC reply looks like a device that ignored the command. **The receive buffer must hold a whole chunk.** `mqttSetup()` calls `tbRequiredBufferSize()` before connecting. Undersize it and PubSubClient discards every chunk without a word: the download sits at 0 % forever.

Guards that are not in fullbot's version and should not be removed:

- **`fw_title` must match `config.mqttFwTitle`.** One tenant holds every device an operator owns; assigning the wrong package to a device profile is one wrong click, and an image built for another board needs USB to recover from.
- **A download will not start while any relay is energised.** It ends in a reboot, and on an ESP32 reset every GPIO floats until `relayPinsSafeInit()` runs — which on these active-low boards means the pump switches back on for the length of the boot. `tbLoop()` holds the announcement until relays idle.
- **Both OTA paths share one `Update` object.** `/updateEnable` answers `409` while `tbFotaInProgress()`, and the cloud path waits on `Update.isRunning()`. Letting both run corrupts whichever finishes second, and `Update` reports success for it — the damage only shows at the next boot.
- **A stalled download aborts after 5 retries**, releasing `Update`. Otherwise an abandoned cloud FOTA locks out the browser OTA that exists to recover from a bad cloud FOTA.
- **`fwVersionDiffers()` accepts a downgrade.** Rolling back is an operator decision made in ThingsBoard, and the only recovery that does not need USB. `test/test_fw_version/` covers the comparison, including the lexicographic trap (`2.10.0` vs `2.9.0`). The filesystem image cannot be delivered this way: `handleUpdateUpload` picks `U_SPIFFS` from the uploaded *filename*, and the FOTA path always writes `U_FLASH`. `littlefs.bin` still goes through `/update.html`. `mqttLoop()` backs off exponentially (1 s → 60 s) instead of retrying every `loop()` iteration, which used to bury the reason for a refused connection in hundreds of identical log lines a minute.
