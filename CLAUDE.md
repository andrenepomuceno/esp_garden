# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Operating guide for **esp-garden**. Companion doc: [README.md](README.md).

**Reference repo:** `fullbot-firmware` (ESP32-S3 firmware for the FullBot solar-panel cleaning robot), at `~/solarbot/fullbot-firmware` **inside WSL** (`wsl.exe -e bash -lc '...'` from Windows — it is not on a `/mnt` path). It has its own `CLAUDE.md` plus a `docs/` directory. The user's explicit instruction is to **reuse it freely** — scheduler, MQTT, web UI, web server, OTA, config, logging. Read it before designing anything new here; most problems this repo is about to hit are already solved there. See [Porting from fullbot-firmware](#porting-from-fullbot-firmware) for what transplants cleanly and what does not.

---

## Project at a glance

ESP32 firmware for an automatic garden: soil moisture + luminosity + DHT11 + optional water level, a watering relay, a local async web dashboard, ThingSpeak MQTT/TLS telemetry, ThingSpeak TalkBack for remote commands, and browser OTA.

| Layer | Path | Role |
|---|---|---|
| Entry point | `src/main.cpp` (108 lines) | Relay safe-init, device id, filesystem, config, logger backup, `webSetup()`, `tasksSetup()` |
| Orchestration | `src/tasks.cpp` (769) | **The single task-registration site.** Every `DECLARE_TASK`, every handler body, `tasksSetup()`, `tasksLoop()`, schedules, connectivity |
| Relays | `src/relays.cpp` (259) | Relay state under `g_relayMux`, `startRelay`/`stopRelay`/`startWatering`, the 50 ms critical tick |
| Sensors | `src/sensors.cpp` (472) | Every ADC read, the flow ISR, the float switch, the DHT and its plausibility gate, `moistureState()` |
| Telemetry | `src/telemetry.cpp` (504), `include/core/step_publisher.h` | ThingSpeak field constants, `mqttAddField`, the periodic ThingsBoard payload, the change-based step publisher, the publish queue |
| Config | `src/config.cpp` (775), `include/core/config.h` | `ConfigFile` singleton + `g_*` reference aliases for the non-pin fields |
| Config — pins | `src/config_pins.cpp` | What a WROOM-32 GPIO can do, and the boot-time audit that applies it. One place, three consumers |
| Config — `io` | `src/config_io.cpp` | Parsers for the `io` block, where every entry accepts several shapes so a field device keeps loading after a firmware update |
| Config — save | `src/config_document.cpp` | Whole-document validation before a write: the save-time counterpart of `loadFile()` |
| Logging | `src/logger.cpp` | Level-filtered singleton, 8 KB rolling RAM buffer, LittleFS backup rotating over 4 files |
| Web | `src/web.cpp` (455) | WiFi events, mDNS, `AsyncWebServer`, **the route table**, `/control`, `/logs`, `/history.json` |
| Web handlers | `src/web_data.cpp`, `web_config.cpp`, `web_ota.cpp`, `web_users.cpp` | `/data.json` cache · masked `GET`/`POST /config.json` · browser OTA · `/users.json` |
| Auth | `src/custom_login.cpp`, `src/user_store.cpp` | Nonce + SHA-256 login, role middleware, per-IP lockout, `/users.json`, `/sessions.json` — ported from fullbot |
| MQTT | `src/mqtt.cpp` | Transport only: `PubSubClient` over TLS or plain, reconnect backoff, buffer sizing. `mqtt.backend` picks ThingSpeak `channels/<id>/publish` or ThingsBoard `v1/devices/me/telemetry` |
| ThingsBoard | `src/thingsboard.cpp` (729) | The downlink half: client/shared attributes, two-way RPC, the chunked `v2/fw` firmware stream |
| Versions | `src/fw_version.cpp` | Semantic-version compare — the check deciding whether a cloud image is flashed. Host-tested |
| TalkBack | `src/talkback.cpp` | Hand-rolled HTTP/1.1 POST to `api.thingspeak.com` (**plain HTTP, port 80**) |
| History | `src/io_history.cpp`, `include/core/segment_index.h` | Append-only segments of I/O snapshots on LittleFS, served by `/history.json` |
| Moisture model | `src/moisture_classifier.cpp` (pure maths, host-tested), `src/moisture_model.cpp` (training, persistence) | Gaussian naive Bayes per probe, labelled by watering events. See [Soil moisture](#soil-moisture-a-classifier-trained-on-watering-events) |
| Cloud cover | `src/cloud_cover.cpp` (pure maths, host-tested), `src/cloud_model.cpp` (clock, minute bucket, snapshot), `include/core/clear_sky_table.h` (**generated**) | Clearness index against an empirical clear-sky envelope. See [Cloud cover](#cloud-cover-an-empirical-clear-sky-reference-and-what-the-data-could-not-settle) |
| Evapotranspiration | `src/evapotranspiration.cpp` (pure maths, host-tested), `src/et0_model.cpp` (clock, day rollover, snapshot) | Daily Hargreaves-Samani ET0 from the device's own temperature extremes. See [Evapotranspiration](#evapotranspiration-the-fit-said-no-and-that-is-the-result) |
| Stats | `src/accumulator_v2.cpp` | Rolling window mean + variance over a `std::list<float>` |
| Web sources | `data/` | Plain, diffable, served as-is by the simulator. `index.*`, `login.*`, `config.*`, `users.*`, `update.*`, `auth.js`; vendored `jquery.js`, `sha256.js`, `spark-md5.js`, `bootstrap.css` (all MIT, shipped `.gz`); `favicon.ico`, `*.pem`, `config.template.json` |
| Filesystem image | `scripts/build_assets.py` → `.pio/assets/` | Bundles each page's scripts into one file and gzips everything the web server serves. `data_dir` points the image here, so `-t buildfs` cannot pack the unbundled sources |
| Partitions | `partitions/esp_garden_4mb.csv` | 1.69 MB per OTA slot, 512 KB LittleFS. **Cannot be changed over OTA** |
| Filesystem | `include/core/filesystem.h` | The one line naming the driver. Everything else says `FILESYSTEM`, never `LittleFS` |
| Tooling | `scripts/` | `dev_server.py` + `sim_state.py` · `sim_moisture.py` · `sim_config.py` · `sim_auth.py` (host simulator of the device HTTP API), `check_lines.py` (the file-size gate), `moisture_calibration.py`, `moisture_fit.py` + `moisture_stats.py` (the PC-side moisture fit: reads the archive and the device's config, detects the configuration seams and the handling transients itself, and **refuses to emit a parameter the data cannot support**, naming the check that refused it), `cloud_fit.py` (fits the clear-sky reference from the archive and writes the generated header), `et0_fit.py` (geocodes `postalCode`, pulls the public daily record and scores the device's ET0 against a station's — it is allowed to answer "do not enable this", and did), `feeds_plot.py`, `tb_export.py` (incremental ThingsBoard → SQLite archive in `backups/`, wide CSV on `--csv`), `telemetry_ui.py` + `telemetry_page.py` (local read-only browser for that archive on **:8090** — charts, the dead-key inventory, boots and gaps; it imports `tb_export`'s seam constant and its sync rather than restating either) |

**No source file exceeds 1000 lines, and `python scripts/check_lines.py` is what
says so.** The rule sat in this file unenforced for long enough that two files
crossed it unnoticed — the gate exists because the honour system had already
failed. It prints the largest files on success too: a failure arrives when the
split is expensive, and the useful signal is the file three commits away from
crossing. `tasks.cpp` (1123), `web.cpp` (1004), `config.cpp` (1125) and
`devices.js` (1155) were all split at that threshold. `tasks.cpp` kept every `DECLARE_TASK` and every handler and `web.cpp` kept `webSetup()`, in both cases because the ordering *inside* those functions is load-bearing — see the boot sequence and the route-order note below.

Host tests live in `test/` and run under **`[env:native]`** (`pio test -e native`). Coverage is `AccumulatorV2`, the history segment arithmetic, firmware-version comparison, the moisture classifier, the probe-health verdict, the cloud-cover classifier, the evapotranspiration maths and the step-publisher change detection — everything else reaches WiFi, LittleFS, `Arduino_JSON` or FreeRTOS. **The pattern for making something testable is to put the arithmetic in an Arduino-free header** (`segment_index.h`, `step_publisher.h`) rather than to stub the platform: both are small enough to look obviously right and wrong in a way that produces plausible answers instead of failures. See [test/README.md](test/README.md), including why the JSON logic must not be trusted to a hand-written stub.

---

## What has actually run

Every claim below this line is either **verified** — it has been executed on the
hardware or in a test, with the date — or **unverified**, which means it was
written and compiles and nothing more. Move a line up only after running it.
Adding a section to this file does not promote anything.

This exists because this document has been wrong. It stated *"no page loads a
script from a CDN"* while all nine pages pulled Bootstrap from jsdelivr; it
stated *"no source file exceeds 1000 lines"* while two did; it recorded
`FW_VERSION` as `1.1.0` long after it was `2.1.0`. All three were found by a
reader, not by the project. A number without an execution behind it is a guess
that has been formatted to look like a fact.

**A hardware fault worth knowing before blaming firmware:** every boot prints

```
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
flash read err, 1000
ets_main.c 371
rst:0x10 (RTCWDT_RTC_RESET)   <- second attempt, boots fine
```

The ROM bootloader fails its first flash read and the RTC watchdog restarts it.
That is marginal supply — a long USB cable, a hub, or the regulator sagging —
not code, and it costs a few seconds of every boot. It will become a real boot
loop if the supply gets worse.

**Getting the board back after serial work:** `esptool.py --port COM7 run`.
Hand-rolled DTR/RTS toggling left it in `DOWNLOAD_BOOT` — off the network,
silent on serial, and looking exactly like a hang. An hour went into diagnosing
a board that was sitting in the ROM bootloader waiting for a flash.

**Reading serial:** `pio device monitor` under a `timeout` truncates its buffer
unpredictably, which made a healthy boot look like it stopped at three different
places. Read the port directly and flush per line when the answer matters.

**Keep the ELF of every image you flash.** `backups/elf/` holds one per
version. A core dump only decodes against the exact binary that produced it —
a rebuild from the same sources does not match, because the app image embeds
its own hash — and without it `espcoredump` refuses and the backtrace is
garbage. An hour went into learning that.

**The board was replaced on 2026-08-31.** The NodeMCU-32S died; the garden now
runs an **ESP32 DevKit v1 built by `[env:espgarden2]`, device id `6224`**, at
192.168.1.55. Everything below that says `9e7c` was measured on the dead board
and still describes this firmware — the two carry the same ESP32-WROOM-32, so
nothing about the readings, the pins or the timings changed with it.

### Swapping the board

**No pin changes were needed, and that is worth knowing before anyone invents
some.** `espgarden1` and `espgarden2` differ by exactly one line — `board =
nodemcu-32s` against `board = esp32doit-devkit-v1` — and the `-D ESPGARDENn`
flags they also carry are read by nothing in the tree. Same module, same 4 MB,
same `LED_BUILTIN` on GPIO 2. Every pin the config uses (relays 15/16/17/18,
probes 36/35/32, DHT 23, luminosity 39, water 34, flow 27, float 26) exists on
both. On a 30-pin DevKit v1 GPIO 0 is not on the header, which does not matter:
`io.button` is a `pinMode(INPUT)` nothing reads.

**What DOES have to change is `id`, and getting it wrong bricks the board.**
`ConfigFile::loadFile()` rejects the whole document when `id` does not match
`ESP.getEfuseMac() % 0x10000`, so the old config on a new chip means compiled
defaults, `ssid "undefined"`, and a device that cannot associate or be reached
to fix. Read the id from the firmware's own `ID:` boot line rather than deriving
it from esptool — that line is by definition the number the check compares
against. Every other field carries over, and carrying the ThingsBoard
credentials over is what keeps the cloud device, and its telemetry history, the
same one.

Do it with the relay board DISCONNECTED. Every reset floats the GPIOs and these
relays are active-low.

**`open(): /littlefs/<asset> does not exist` on every page load is NOISE.**
AsyncFileResponse probes the plain name before falling back to `.gz`, and the
ESP-IDF VFS logs that first miss at error level whatever happens next. Since the
assets ship gzipped, every page produces a burst of them and every one is
followed by a 200. Verified on 6224: `/index.html` 200 gzip 1694 B while the log
called it missing three times.

**Verified on hardware (devices `9e7c` and, from 2026-08-31, `6224`):**

- Nonce + SHA-256 login, roles, per-IP lockout, persistent sessions.
- ThingSpeak *and* ThingsBoard MQTT over TLS. Channel 1348790 entry 304860 was
  the first publish since 2023-03-07, after the CA pin was corrected
  (2026-08-23). ThingsBoard connected on `thingsboard.cloud:8883` the same day.
- ThingsBoard downlink **subscriptions** — all four topics confirmed in the
  device log at debug level (2026-08-24).
- Browser OTA of both firmware and filesystem, including recovery from a
  half-written filesystem via serial.
- `GET /config.json?secrets=1` → edit → `POST` round trip, verified by byte
  count (1267 sent, 1267 written). The masked-secret restore path.
- The I/O history buffer, its wrap-around, and `/history.json` — as a ring, under SPIFFS. The append-only segments that replaced it are host-tested and built but have not yet run on the board.
- Relay switching, the 30 s ceiling, the sticky mask in the history record.
- Probe readings: three probes at 42.9 / 57.0 / 52.6 with variances ≤ 0.10.
- **Heap under load** (2026-08-24, firmware 2.2.1). Six endpoints hammered in
  parallel — including `/history.json?limit=200` — eight rounds, zero failures.
  269 KB free at boot, ~115 KB in steady state, **76 KB at the low-water mark**.
  Memory is not the constraint it was assumed to be. The number to watch is the
  largest free block: it drifts 53 → 49 KB while free stays flat, which is
  fragmentation from `/history.json` building its response.

  **The String was NOT enough, and the peak did bite — chunked generation is
  what fixed it.** With 548 records stored, `GET /history.json?limit=200` — the
  request the history page makes on every load — answered **200 OK with a
  Content-Length of ZERO**. ~35 KB of String plus the ~35 KB copy
  `beginResponse` makes do not fit beside a 53 KB largest free block, and the
  failure is silent: the page just shows nothing. The handler now emits one
  record per filler callback, so peak is independent of `limit`. Verified on
  the device at 5 / 100 / 200 records and with `window=86400`.

  **`AsyncResponseStream` was tried as the fix and made it worse — withdrawn.**
  The stream looked obviously right, since `beginResponse` copies the String it
  is handed and the path therefore costs the payload twice. Measured on the
  same eight-round parallel load: largest free block **53 KB → 33 KB** (once to
  16), low-water heap **76 KB → 60 KB**. The stream's buffer grows by
  reallocation and leaves a hole at every step, while one `reserve()` of the
  right size is a single allocation. The double copy costs PEAK; the stream
  costs FRAGMENTATION, and fragmentation is what bites a device that has to
  stay up for months. If the peak ever does matter, the answer is chunked
  generation — one record per callback — not a differently-shaped buffer.
- **Runtime hardware.** Firmware 2.2.0 boots off `config.json` alone and logs
  what it decided: `Sensors: 3 moisture, luminosity, DHT, water level, flow,
  float switch`, `DHT11 on GPIO 23`, `Flow sensor on GPIO 27`. No `HAS_*` flag
  exists any more (2026-08-24).
- **`GET /capabilities.json`** — returns `relayMax 8`, `moistureMax 4`,
  `analogPins [32,33,34,35,36,39]` and `reservedPins [1,3]`, i.e. it excludes
  the unbonded pins and separates the serial console, from the same predicates
  `validatePins()` uses.
- **`GET /moisture.json`** — answers on the device, reports the gates and
  `blockedBy: "not trained yet"` for all three probes. The endpoint and the
  refusal path are exercised; the FIT is not (see below).
- **`POST /spiffs/delete`** (2026-08-24). All four refusals fired on the
  device: `/users.json` (credential store), `/config.json` (use the validating
  endpoint), `..` in the path, and a 404 for a path that is not there — the
  last one deliberately, so a wrong path is reported rather than silently
  succeeding.
- **`POST /spiffs/upload`** (2026-08-24). A 48-byte file written and read back
  byte-identical through `/spiffs/`, with 139 KB still free. All four refusals
  fired on the device: wrong MD5, `/config.json`, `/users.json`, and `..` in
  the path. *(The probe file `/probe.js` is still on the filesystem — there is
  no delete endpoint. Harmless, and it is why one exists as a to-do.)*
- **The relay guard on the browser OTA** (2026-08-24). With Zona 2 energised,
  `/updateEnable` answered **409 "Zona 2 is running. Updating reboots the
  device, and a relay is energised across a reset."**; it answered 200 once the
  relay expired.
- **The vendored `bootstrap.css.gz`** — served from the device at 29 899 bytes.
  No page reaches a CDN any more.
- **The moisture classifier's TRAINING pass, on real data** (2026-08-24
  08:22). One watering of Zona 2 — a relay run by hand to test the OTA guard —
  was detected as a single rising edge, and the 33 readings inside the 60-minute
  window before it were labelled DRY:

  ```
  [moisture] probe 1: +1 events (1 total), J=0.0, dry/humid/wet = 57.1/0.0/0.0
  [moisture] trained on 56 records, 33 samples, 0 outliers dropped
  ```

  Humid and wet stayed empty, which is the consumption window working: the
  cycle that event opened is not complete until the NEXT watering bounds it, so
  everything after it waits. The gates then refused the probe at 1 of 6 events.
  What is verified is the labelling, the window and the refusal. What is still
  not is a **complete** cycle, and with it the first non-empty WET class.
- **The `IoRecord` layout change** — the 40 → 48 byte growth was detected by
  the header check and the buffer reformatted rather than being misread:
  `io_history: incompatible or corrupt file, recreating` →
  `formatted /io_history.bin for 1440 records (69136 bytes)`.
- **The SPIFFS → LittleFS migration** (2026-08-24, firmware 2.5.0, over USB).
  Both images flashed back to back on COM7, and after it: `/config.json`
  intact at 1219 masked bytes with `id 9e7c` and `mqtt.backend thingsboard`,
  all six credentials still present, the admin login working against the
  migrated `/users.json`, every asset serving from the device (`index.html`
  5776 B, `bootstrap.css` 29 899 B, `jquery.js` 30 451 B, both `.pem` files),
  `/history.json` answering, and the three `/spiffs/` 403 shadows —
  `config.json`, `users.json`, `sessions.json` — still firing under an ADMIN
  token. The history ring and the moisture model were reformatted, as expected.

  **A one-off panic on the FIRST boot after the migration, not reproduced
  since.** `Core 1 panic'ed (LoadProhibited)`, `EXCVADDR 0x00000028`, return
  address `0x400933ac` = `multi_heap_internal_lock` — a heap allocation with a
  null heap handle, i.e. heap corruption, raised inside `webSetup()`. The board
  rebooted itself and came up clean, and **five deliberate resets afterwards
  produced zero panics**. It is recorded here rather than dismissed because a
  boot panic on an active-low relay board pulses every pump for the length of a
  reset. What is NOT established is a cause: one sample, corrupted backtrace,
  and a board that already prints `flash read err, 1000` on every boot from a
  marginal supply. If it recurs, the next step is a core dump
  (`esptool read_flash 0x3F0000 0x10000`) against
  `backups/elf/firmware-2.5.0-littlefs.elf`, which is kept for exactly that.

- **The append-only history, on the device** (2026-08-24, firmware 2.6.0,
  over the air). The rewrite that replaced the ring survived the append that
  killed its predecessor: uptime passed ten minutes and kept going where the
  old design panicked at ~60 s, every boot. The log reads

  ```
  io_history: removed the legacy ring /io_history.bin
  io_history: ready, 0/1440 records across 0 of 8 segments (180 each)
  io_history: logging every 60 s
  ```

  and after ten minutes `/spiffs/hist0.bin` is **492 bytes — a 12-byte header
  plus exactly 10 records of 48** — while `hist1..7` are still 404, which is
  the lazy creation working. `/history.json?limit=5` answers with records 60 s
  apart carrying all three probes and the DHT. Deleting the legacy ring freed
  the expected 69 KB: the filesystem went 392 → 332 KB.

  **Flash usage is flat, which is the number that condemned the ring.** Measured
  over eight consecutive appends at 60 s: `hist0.bin` grew 972 → 1356 bytes,
  exactly 48 per record (28 x 48 + 12 = 1356), while the filesystem stayed at
  336 KB throughout. The earlier 324 → 336 KB climb was LittleFS allocating the
  new segment's blocks and settling, not a leak — copy-on-write reuses the last
  block until it fills, so a 48-byte append costs one 4 KB block rewrite rather
  than the ~69 KB the ring paid. That is roughly 1440 block erases a day spread
  across free space, against blocks rated for ~100 000: years, where the ring
  measured in weeks. (The erase count is arithmetic from the measurement, not a
  measurement — nothing here reads the wear counters.)

  What is NOT yet exercised is a **rotation**: segment 0 fills at 180 records,
  so the first recycle is three hours in, and the retention swing it introduces
  has only been tested on the host.

- **The firmware OTA path, end to end** (2026-08-24). 1.2 MB uploaded to a
  board with no USB attached, `/updateEnable` armed against idle relays, and
  the version confirmed by polling until it changed — the upload's own
  connection timed out after 420 s, which as the trap below says is not a
  verdict. Two web assets then went up individually through `/spiffs/upload`
  and were read back byte-identical, so `data/` changes no longer cost a
  partition rewrite.

- **The page-load fix, measured on the device** (2026-08-25). Before:
  `devices.html` pulled 7 requests and **1 of 3 loads** came back with a
  truncated asset, the largest free block collapsing 45 → 1 KB under the load.
  After bundling and gzipping: **4 requests, 10/10 clean loads, largest free
  block steady at 53 KB.** Every asset was verified byte-identical after upload
  and again after gzip round-trip. Nine pages were deployed one file at a time
  through `/spiffs/upload`, so `/config.json` was never at risk, and the
  filesystem went 392 → 320 KB.

  **Two reboots were self-inflicted and are recorded as such.** Hammering the
  device with eight parallel requests in back-to-back rounds tripped an
  **interrupt watchdog** once; a second reboot logged `software (ESP.restart)`,
  whose only paths are `/control reset` and a finished OTA. Between them the
  board had run **15 hours** unattended with no reboot at all. Load-testing an
  active-low relay board is not free: every reset pulses every pump.

- **Firmware 2.6.1, over the air, and what it proved** (2026-08-25).

  - **The five-root CA bundle validates on the device, not just in openssl.**
    The pem was uploaded BEFORE the OTA on purpose, so the reboot the update
    needs anyway became the test: the board came back with
    `MQTT Link: connected` and `Last Publish: 31s ago`. That is the check that
    matters, because `mqttSetup()` reads the CA once at boot — a bad bundle
    would have shown up only at the next reset, silently.
  - **`MQTT Link` and `Last Publish` answer on the device.** The dashboard now
    distinguishes the operator's switch from the broker link, which is the gap
    that hid a three-year outage.
  - **The superseded-twin deletion works**, logged four times by the device:
    `[upload] removed the superseded /config.js` and three more. Uploading the
    `.gz` removed the plain file that AsyncFileResponse would otherwise have
    kept serving, so the explicit deletes that followed correctly found nothing.
  - **The exact upload guard unblocked the last four assets.** `/config.*` and
    `/users.*` had been refused by a prefix match; they deployed cleanly.
  - **All nine pages, swept clean.** Every page serves gzipped at 4 requests
    (5 where a vendored sha256/SparkMD5 is also needed), with no truncation.
  - **The history survived the OTA reboot**: `1139/1440 records across 7 of 8
    segments`. Segment CREATION is now well exercised; the first recycle is
    still ahead, at 1440.

- **The board swap, end to end** (2026-08-31, device `6224`). A virgin DevKit
  v1 took the firmware, printed `ID: 6224`, and the config rewritten with that
  one field loaded: `Loading done`, WiFi up, `MQTT Link: connected` and a
  publish accepted 9 s later — so the ThingsBoard device and its history
  survived the hardware. Login worked against a `/users.json` seeded from
  `ota.*` on first boot, and every asset served gzipped.

- **The polarity default, against ground truth** (2026-08-31). With the soil
  reported wet by the operator, both connected capacitive probes read 128 ADC
  counts (0.10 V) and the dashboard showed 96.87 — the wet end. `invert` true
  is therefore right for the capacitive v2, confirmed against the pot rather
  than against a datasheet. It is also the first end-to-end check that the
  conversion, the polarity and the displayed number agree.

- **The probe health check, on a second chip** (2026-08-31). The verdict formed
  exactly as designed: `fault` absent while `samples < 120` — no evidence is
  not health — and all three unplugged probes flagged `noisy` from 3 minutes
  in, at sd 885/1745/885 counts against temperature at 0.00-0.03. One probe
  read `var 0.00` in the instant it was still flagged, which is the decayed
  evidence deciding rather than the current window.

- **The probe health check, on the device** (2026-08-25, firmware 2.7.0, over
  the air). All three unplugged probes were flagged within minutes:
  `/data.json` carries `fault: "noisy"` on each, and `/moisture.json` names the
  spread — 1113, 1773 and 1753 ADC counts inside one window. Every connected
  sensor on the same board passed: temperature 4 counts, air humidity 20,
  luminosity 164 while the light was changing. The dashboard badge and the
  refusal text both render from the device.

- **The OTA fixes and the honest upload report** (2026-08-31, firmware 2.7.3,
  device 6224). The three faults were found because `/update.html` reported
  `ERR_CONNECTION_RESET` on every attempt while the flash was in fact landing:
  attempt 2 "failed at 100 %" and the board came up on the new firmware. After
  2.7.3 the firmware and then `update.js.gz` both went up through the page and
  the log reads `[upload] wrote /update.js.gz (5085 B)`. The board is on 2.7.3
  with all three probes reading and no false faults.

- **Firmware 2.8.0's telemetry change, over the air** (2026-09-03, device
  6224). One clean boot, `software (ESP.restart)`, no panic: the last 2.7.3
  point is 18:42:52 and the first 2.8.0 point 18:43:13, so the board was
  unreachable for **21 s**. Heap 110 KB free, 93 KB low-water at 16 minutes,
  with the larger accumulator windows already allocated.

  - **`mqtt.publishSec` = 300 holds.** Publishes at 18:48:12, 18:53:12 and
    18:58:12 - 300 s apart, the first 301 s after boot - so `setPeriod()` and
    `enableDelayed()` both take the configured value and not the compiled 60 s.
  - **The instantaneous twins are stored, and the averages kept their meaning.**
    All six of `temperatureNow`, `airHumidityNow`, `luminosityNow`,
    `moisture1Now`, `moisture2Now`, `pingNow` arrive beside the original names,
    which still carry `getAverage()`. The ThingsBoard key count went 37 -> 43.
    No stored series changed meaning, which was the expensive thing to get
    wrong.
  - **Step values are off the periodic tick.** The 18:58:12 payload carried
    exactly 12 keys, every one continuous. The nine step keys - `relay1..3`,
    `relay1..3Ran`, `relayMask`, `relayRanMask`, `connectionLoss`,
    `dhtErrorRate`, `wateringCycles` - appear once at 18:58:13, which is the
    **900 s heartbeat** firing 902 s after boot. `firmware` published once at
    boot and not since.
  - **Composition, with both sides now measured:** 12 continuous keys x 288
    ticks + 9 step keys x 96 heartbeats = **~4 320 datapoints/day**, against the
    **25 100/day** measured on 2.7.3. About **-83 %**, on a board carrying two
    probes and no water level.

  **The OTA client called this a failure, and the reason was found later the
  same night.** The upload sent 1.2 MB in 12 s at 100 KB/s and the device
  answered `200 OK`; the script then printed "the web server never came back"
  while the flash had in fact landed. It happened three times.

  **The reboot takes SIX SECONDS**, measured once the client was made to print
  absolute timestamps: upload complete 22:33:13, board down 22:33:17, serving
  again 22:33:19, uptime 6 s. The bug was the wait itself:

  ```python
  while time.time() < deadline and not dev.alive():   # true at once: the OLD
      time.sleep(3)                                   # firmware is still up
  if not dev.alive():                                 # now it IS rebooting
      raise SystemExit("the web server never came back")
  ```

  `handleUpdateRequest` answers 200 and only then calls `requestRestart()`,
  which reboots ~500 ms later from `loop()`. So the first probe caught the old
  firmware still serving, the loop never ran a single iteration, and the second
  probe caught the reboot. The two calls straddled it. **The wait never
  happened**, which is why an intermediate diagnosis here — that the board was
  taking five minutes to return — was wrong: it assumed a loop that had run to
  its deadline. A client must wait for the board to go DOWN before waiting for
  it to come back, or "alive" is satisfied by the firmware being replaced.

- **The resistive probes' transfer curve, and the first working two-point
  calibration** (2026-09-03, device 6224). Five conditions measured by hand and
  converted to raw ADC — the units the physics is in, since the displayed number
  is already `100 - ADC%` under `invert`:

  | condition | shown | ADC |
  |---|---|---|
  | wet soil, Zona 3 (archive mean, n=1099, undisturbed) | 75.52 | 1002 |
  | wet soil, Zona 1 (archive mean, n=1099, undisturbed) | 73.48 | 1086 |
  | air — a still-damp probe, mid-transit | 59.32 | 1666 |
  | **dry soil**, Zona 1, a different pot | 53.11 | 1920 |
  | water, submerged | 0.00 | 4095 |

  **Four of the five are monotonic in the direction physics predicts** — wetter
  reads lower — which is what confirms `invert: true` is right for these
  resistive probes, measured at both ends of soil rather than inferred. The
  "air" point is not an anchor and not an anomaly: the probe was still damp, so
  it lands BETWEEN wet and dry soil, exactly where a drying probe should, and
  that is a consistency check rather than a reading.

  **Submersion is the one outlier and it is out of domain.** A resistive divider
  submerged in tap water has its lowest resistance and should read the extreme
  WET end; it read the opposite rail. Comparator saturation or the electrodes
  shorting the divider are both plausible and neither was established. It does
  not matter: soil is never submerged in tap water, so the estimator is anchored
  on the two soil points and this one is excluded by irrelevance, not by
  mystery.

  Applied as `moisture[0] = {dry 53.1, wet 73.5, kind "resistive"}`. Sent 1020
  bytes, device wrote **1060** — the +40 is the five 8-character masks being
  replaced by real credentials, and since the handler REFUSES a document still
  carrying a mask, the 200 is itself proof the restore worked. After the reboot
  `/data.json` carries `state: "Wet"` on Zona 1.

  **Zona 3 was deliberately left uncalibrated** (`dry == wet == 0`), so
  `moistureState()` returns empty and the field is absent rather than invented.
  Its wet end is measured; its dry end has never been taken. The two probes'
  wet ends differ by only 2.0 points, so borrowing was tempting — and is exactly
  what this file forbids, because a threshold is per-probe.

  **`dry` is an UPPER BOUND, not the dry value.** The probe was still falling at
  ~0.15 per 30 s when it came out of the dry pot. That narrows the span and
  makes the badge say "humid" slightly earlier, which for a garden is the right
  side to err on. It needs redoing when a zone dries on its own.

**Unverified — written, compiles, never run on hardware:**

- **The whole evapotranspiration model** (firmware 2.10.0). Host-tested in
  `test_evapotranspiration`, built in all five envs, and **shipped OFF**:
  `et0.enabled` defaults to false, so on the board today it computes nothing and
  publishes nothing. Not one day has closed on the device, no `et0` key has
  reached ThingsBoard, and `Status.ET0` has never rendered.

  **It ships off for a stronger reason than the cloud model does, and the
  reason is a measurement rather than a caution.** The cloud table is off
  because it describes one mounting. This is off because, validated against a
  public station over the archive's own window, the estimate **had no
  demonstrated skill on this device** — see
  [Evapotranspiration](#evapotranspiration-the-fit-said-no-and-that-is-the-result).
  Specifically untested on hardware:

  - **The day rollover against the real clock.** `localtime_r` and
    `tm_year * 366 + tm_yday` decide when a day ends; the arithmetic is
    host-tested against synthetic keys, and no midnight has passed on the board.
  - **The coverage refusal in the field.** A day short of 22 of 24 hours is
    refused and logged. That path has never been reached by a real reboot.
  - **`et0.scale` other than 1.0.** Never set on a device, so the warning it
    logs has never printed.
  - **The event itself.** `publishEt0Event()` has never run, so the four keys
    have never been seen in a payload.

- **The whole cloud-cover model** (firmware 2.9.0). Fitted on the archive,
  host-tested in `test_cloud_cover`, built in all five envs, and **shipped
  OFF**: `cloud.enabled` defaults to false, so on the board today it computes
  nothing and publishes nothing. What has never happened is a single minute of
  it running on the device — not one clear-sky lookup against the real clock,
  not one state, not one episode. Specifically untested on hardware:

  - **That the device's minute mean matches the archive's.** The argument that
    it does is structural — while `mqtt.publishSec` was 60 the stored
    `luminosity` series WAS the mean of each minute's 1 Hz samples — but it is
    an argument, not a measurement, and nothing has compared the two.
  - **The local clock path.** `localtime_r` against `config.timezone` decides
    which bin every reading lands in, and a whole-bin error at the morning ramp
    is eight points of reference. The fit assumed UTC-3 and reproduced the
    hourly means this file was given, which is evidence about the ARCHIVE.
  - **The thresholds against a sky somebody looked at.** Every class boundary
    was fitted to six days of one sensor's numbers. No human has confirmed that
    the badge said "overcast" while it was overcast.
  - **The episode rate in the field.** Replay says 0-1 a day settled and 3-7 on
    a mixed day; the device has produced none.
  - **`cloudVariability` and `cloudState` in a real payload.** Neither key has
    reached ThingsBoard.

  And the standing warning, which is the reason for the default: the compiled
  clear-sky table describes ONE sensor at ONE mounting in ONE season, the
  archive already contains a mounting change six days before the fit, and
  **nothing on the device can detect the next one**.

- **The `Sd` error bars** (firmware 2.9.1). Six extra keys per publish, one per
  continuous channel, computed from a variance the accumulator already
  maintained. Never published from the device, so neither the values nor the
  datapoint cost above has been observed — the cost is arithmetic from the
  measured 288 ticks/day.

- **`relayStop` on the device** (firmware 2.8.1). The whole contract is
  verified against the simulator — relay started for 20 s, stopped on demand,
  and a repeated stop on an idle relay accepted as harmless — and `index.js`
  parses, but the firmware's own `stopRelay()` path has never been reached from
  `/control`. Exercising it means energising a real pump: the board carries
  three relays, all wired, and the relay 4 that used to be the test slot was
  removed from the config on 2026-09-02.

- **The parts of firmware 2.8.0 that have NOT run.** The periodic path, the
  heartbeat, the twins and the accumulator re-sizing are verified above. These
  are not:
  - **A relay transition through `tbPublishEvent()`.** No relay has been
    switched since the flash, so the change-driven half of step publishing -
    the whole reason it is asynchronous - has never fired. The heartbeat has;
    they are different code paths.
  - **The deadbands.** `flowRate` 0.05, `flowTotalLitres` 0.005 and
    `dhtErrorRate` 0.5 are host-tested as arithmetic only. `dhtErrorRate` has
    published on the heartbeat alone, at a constant 0.
  - **`moistureNFault`.** Both probes read healthy, so neither the key nor the
    single publish when a fault CLEARS has been seen on the device.
  - **The DHT plausibility gate.** No out-of-range reading has been rejected on
    hardware, and the gate has since been **corrected twice over**. Its two
    original justifications were both wrong: the 45.04 C reading is INSIDE the
    rated 0-50 C band and was never going to be rejected, and the 15.1 % RH
    reading it *would* have rejected is **real**. This garden is in Brasilia
    (`postalCode`), where the dry season takes the afternoon below 20 % as a
    matter of course -- measured in the archive: 261 points, 2.0 %, minimum
    15.00, on the two driest days. The floor is now 5 %, low enough to catch
    only garbage. **A datasheet's rated range describes ACCURACY, not what the
    sensor can physically report**, and clamping to it discards the true
    extremes -- which on a garden controller are the readings that matter most.

    **Two of those numbers have since been refined, and the decision does not
    move.** Of the 261 sub-20 % points, **243 are ambient and 18 are not**: the
    18 fall inside a 38-minute patch of direct sun on 2026-09-03 whose dewpoint
    collapses and recovers, so they measure a cooked sensor rather than dry air.
    The minimum of 15.00 is on 09-01 and is real, so the 5 % floor still rests
    on genuine readings. **And the temperature spike is no longer unexplained**
    -- the luminosity channel saturates for exactly that window while both soil
    probes stay flat. See
    [Evapotranspiration](#evapotranspiration-the-fit-said-no-and-that-is-the-result).
  - **The dropped bare `relay` key.** Nothing here is known to read it, but no
    consumer was checked.

- **The PC-side moisture fit's ACCEPT path** (`scripts/moisture_fit.py`,
  2026-09-03). The tool has run end to end against the real archive and the
  live device's `GET /config.json`, and every branch it took was a REFUSAL —
  which is the outcome the data supports and is recorded as a result, not as a
  failure. What has never happened is the other half: no real probe has ever
  produced a proposed `dry`/`wet` pair, so the acceptance path is exercised only
  by `--self-test`'s synthetic ten-cycle garden. Specifically unexercised:

  - **`--push`.** Written, off by default, and never once run. It POSTs the
    masked document back with `moisture` replaced; the change would land at the
    next boot. No byte has been written to 192.168.1.55 by this tool.
  - **The `WET_ANCHOR_MAX_LAG_SEC` = 2 h window.** Chosen as the firmware's
    30-minute wet window plus the drift detector's own 30-minute smear, doubled
    for headroom. It is arithmetic about the two thresholds, not a measurement
    of how long a real pot takes to settle.
  - **`DRIFT_MIN_CHANGE` = 1.0 point / 30 min against a whole season.** It sits
    two orders of magnitude above the measured -0.323 points/day and an order
    below a handled probe, so it has margin on both sides — but it has only
    been tested against a spring week. A wetter month that dries faster is the
    case that would move it.

- **Per-probe polarity and probe power gating** (firmware 2.7.0). The config
  keys parse, the five envs build, and the save/load round trip is verified
  against the simulator — but no probe has yet been read with `invert` false,
  and no `powerPin` has ever been driven, because the resistive sensors have
  not arrived. What is specifically untested on hardware is the settle time: 10
  ms is a default chosen from how a divider and an LM393 behave, not from a
  measurement on the WD-38 that is coming.

- **The history boot-loop interlock.** The RTC strike counter compiles and
  ships, and the path that clears it after ten minutes has run — but no panic
  has been counted against it, so the refusal itself has never fired. It exists
  because the board is now reachable only over the air, where a writer that
  panics before the web server answers is unrecoverable.

- **A segment RECYCLE.** Seven of eight segments are in use and creation is
  proven; what has not happened is the oldest being truncated and reused, which
  starts at 1440 records — and with it the retention swing between 7/8 and 8/8
  of capacity, so far only tested on the host.

- **ThingsBoard RPC.** No command has been sent. The device's credential is
  MQTT Basic rather than an access token, so the tenant REST API is not
  reachable from here; it needs a person in the ThingsBoard UI.
- **ThingsBoard FOTA.** No package has ever been assigned. The chunk stream,
  the MD5 verification, the relay-idle deferral and the 409 interlock against
  the browser path are all unexercised.

- **The absorption time constant, on real soil.** `moistureTimeConstant()` is
  covered by six host tests against synthetic first-order rises, and the
  estimator, the fold and the confidence feedback all compile and ship. What
  has never happened on this board is a watering whose 30 minutes afterwards
  are in the history buffer at the moment training runs — the one watering on
  record was a manual relay test, and the history was reformatted by the
  LittleFS migration anyway. Until a real cycle completes, every probe reports
  `tau` 0 and the 5-minute linear ramp is what is actually running.

- **The reservoir interlock.** `floatInterlock` is off, and the float switch is
  not wired.
- **The flow meter.** Not wired. `flowPulsesPerLitre` is a datasheet number, not
  a measured one.

- **Scheduled watering.** Four schedules exist, all disabled. None has fired.

**Measured, and contradicting something this file used to say:**

- A **disconnected** probe on GPIO 32 does not sit at a rail, and does not sit
  anywhere in particular. Two readings hours apart in the same session:
  **52.6 (variance 0.01)** and **86.7**. Quiet at any instant, and free to be
  somewhere else entirely later — which is worse than a rail, because each
  sample looks like plausible soil.

  *(An earlier version of this line reported only the 52.6 and treated it as
  characteristic. **Withdrawn** — one sample of a floating input describes that
  minute. The "~94 here" it replaced was equally unreproducible.)*

  The consequence stands and gets stronger: the 3σ outlier rejection will not
  fire on any single one of these, so what rejects a disconnected probe is the
  SEPARATION gate — a floating pin does not respond to its pump, so dry, humid
  and wet collapse onto one number and J goes to 0.

## Hardware v2 — `espgarden5`

The second-generation node carries **1 luminosity sensor (LDR, analog) · 1 DHT11 · 4 relays · 2 soil-moisture sensors (analog)**, built by `[env:espgarden5]`. Relay and probe counts are **runtime**, from `config.json` — see [Runtime hardware](#runtime-hardware--the-build-no-longer-knows-what-is-fitted). `BuildConfig.h` only caps them at `RELAY_MAX` 8 and `MOISTURE_MAX` 4.

Default pin map (overridable per device through `config.json`):

| Signal | Pin | Why that pin |
|---|---|---|
| Soil moisture 1 | GPIO 36 (`VP`, A0) | ADC1 |
| Soil moisture 2 | GPIO 34 (A6) | ADC1 |
| Luminosity (LDR) | GPIO 39 (`VN`, A3) | ADC1 |
| DHT11 data | GPIO 23 | Output-capable with an internal pull-up; 34–39 cannot do either |
| Relays 1–4 | GPIO 19, 16, 17, 18 | Output-capable, not strapping, not flash |

Hard constraints that shaped it, and that still apply to any change:

- **ADC2 is unusable while WiFi is on** (ESP32 silicon limitation). Every analog channel **must** land on ADC1: GPIO 32–39. GPIO 34–39 are input-only and have no internal pull-up — fine for analog, useless for a relay or the DHT.
- **ADC1 is the scarce resource, and on a WROOM-32 it has six usable channels**: 32, 33, 34, 35, 36, 39. GPIO 37 and 38 are ADC1 on the die but are not bonded out on the module. `espgarden1` now spends five of the six (3 probes + luminosity + water level), leaving GPIO 33. **GPIO 32/33 double as XTAL_32K_P/N** — free on a WROOM-32, which ships without that crystal, but unavailable on a module that has one fitted.
- **Relay pins must avoid the strapping pins** (0, 2, 5, 12, 15) and GPIO 6–11 (SPI flash). The legacy watering relay sits on **GPIO 15**, a strapping pin (MTDO), and stays there so boards in the field are unaffected; `espgarden5` moves relay 0 to GPIO 19. Other free output-capable pins: 13, 14, 21, 22, 25, 26, 27.
- **Relays are active-low** (`relayPinOn = 0`) and a floating pin reads as "energise". `relayPinsSafeInit()` is therefore the **first statement of `setup()`**, and `loadConfigFile()` calls it again once the real pin assignment is known. Do not move either call later — everything between them (filesystem mount, config load, WiFi association) takes seconds, which is long enough to run a pump dry.
- **A ThingSpeak channel has only 8 fields**, and the numbering is a permanent contract with the history already in channel 1348790. Map: `1` moisture0, `2` watering duration, `3` ping, `4` water level, `5` luminosity, `6` temperature, `7` air humidity, `8` boot time. **Relays 1–3 have no field and are local-only.**
- **The second soil probe's field is explicit configuration, not an implicit reuse.** `thingSpeak.moisture2Field` defaults to 0 (off), and `validateThingSpeakFields()` **refuses a number a fitted sensor already publishes** rather than letting the payload carry the field twice — one silently wins and the loser's history is overwritten with the winner's units. Renumbering rewrites the meaning of everything already stored, so the choice belongs to whoever owns the channel. Field 8 is the least destructive candidate: `bootTime` is published once at startup and is empty in nearly every record.
- **Flash sits at ~63 % of the 1.69 MB app slot** since `partitions/esp_garden_4mb.csv` replaced the stock table (it was 83.8 % of 1.31 MB before). Roughly 640 KB of headroom remain for the rest of the fullbot port — see [Porting](#porting-from-fullbot-firmware).

---

## Essential commands

PlatformIO is **not on `PATH`** on Windows. Use the penv binary; in WSL `pio` is on `PATH` directly.

```powershell
# ---- Build / flash (PowerShell) ------------------------------------------
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
& $pio run -e espgarden1                      # compile only  (~9 s, verified)
& $pio run -e espgarden1 -t upload            # flash APP ONLY
& $pio run -e espgarden1 -t buildfs           # builds .pio/assets first, then littlefs.bin
& $pio run -e espgarden1 -t uploadfs          # TRAP: overwrites the device's /config.json
& $pio device monitor -b 115200               # serial @115200
& $pio run -t clean -e espgarden1
```

- Envs: `espgarden1` (NodeMCU-32S — moisture + luminosity + DHT), `espgarden2`, `espgarden3`, `espgarden4`, `espgarden5` (hardware v2 — 2 probes + LDR + DHT + 4 relays). CI builds all five in a matrix.
- **The filesystem image is BUILT from `data/`, never packed from it.** `[platformio] data_dir = .pio/assets` and a `pre:` hook run `scripts/build_assets.py` on every invocation, so there is no step to forget. Run it by hand with `python scripts/build_assets.py --list` to see what it would produce.
- **`pio run` does not need `data/config.json`**; `-t buildfs` / `-t uploadfs` do. `data/config.json` is gitignored. CI does `cp data/config.template.json data/config.json` — **never replicate that locally**, it destroys the real Wi-Fi/ThingSpeak/OTA credentials of a physical device.
- The ESP32 currently attached is on **COM7** (Silicon Labs CP210x). It is an
ESP32 DevKit v1 built by `[env:espgarden2]` since the NodeMCU-32S died on
2026-08-31 — build with `-e espgarden2`, not `espgarden1`. `upload_port`/`monitor_port` are unset, so PlatformIO auto-detects; pass `--upload-port COM7` when several boards are attached.
- **Host tests:** `pio test -e native` (CI gates the firmware build on them). `native` is the env; `test_*` is a filter, so `pio test -e test_accumulator` fails.
- **`[platformio] default_envs` excludes `native`**, so a bare `pio run` builds only the five firmware envs. Without it `pio run` tries to build the test environment as firmware and dies on Unity's config header.
- **There is no lint and no static-analysis configured.** `.clang-format` exists (Mozilla base, 4-space indent) but nothing enforces it — format only the lines you touch.

```bash
# ---- Frontend without hardware -------------------------------------------
python scripts/dev_server.py                  # http://127.0.0.1:8080, stdlib only
python scripts/dev_server.py --port 9000 --host 0.0.0.0
```

The simulator serves `data/` and mocks the device API — including the full nonce + SHA-256 login (`/nonce`, `/login`, `/logout`) and the same public/guarded split. Simulator credentials are fixed at **`admin` / `admin`** and printed at startup. **It is a second implementation of the device's HTTP contract** — a payload key, a route or an auth rule changed in `web.cpp` must be mirrored here, or the simulator silently drifts from the firmware.

```bash
# ---- Read the reference firmware (WSL) -----------------------------------
wsl.exe -e bash -lc 'cat ~/solarbot/fullbot-firmware/CLAUDE.md'
wsl.exe -e bash -lc 'ls ~/solarbot/fullbot-firmware/docs'
```

---

## Boot sequence — and why a device can hang in it

`setup()` runs in this order, and the order matters:

1. **`relayPinsSafeInit()`** — parks every relay at its idle level using the compiled defaults. Must stay first; see the relay constraint above.
2. `LED_BUILTIN` output; `logger` is constructed on first use and its constructor calls `Serial.begin(115200)`.
3. `id = ESP.getEfuseMac() % 0x10000`, printed as hex — this is the value that must appear in `config.json`'s `"id"`.
4. `FILESYSTEM.begin(true)` (LittleFS; formats on failure).
5. `loadConfigFile(id)` → applies `log.level` and re-parks the relays on their configured pins.
6. `g_ledBlinkEnabled = error` — set here, not after `tasksSetup()`.
7. `logger.backupSetup()` → rotates `/log0..3.txt` via `/current.txt`.
8. `webSetup()` → `WiFi.begin()`, mDNS, **all routes registered, server listening**.
9. `tasksSetup()` → pins, TalkBack, **critical runner started**, then **two blocking loops**.

**TRAP — `tasksSetup()` blocks `setup()`, but no longer forever.** It spins
`while (!g_hasInternet)` pinging 8.8.8.8 / 8.8.4.4 / 1.1.1.1 every second, then
`while (g_bootTime < g_safeTimestamp)` re-running NTP every 2 s. **Both are now
capped at `g_bootWaitMaxMs` = 60 s**, so a device with Wi-Fi but no internet, or
with a wrong SSID, reaches `loop()` after two minutes instead of never — which
is what makes watering and MQTT start at all on a box behind a captive portal.

Two minutes of a dead `loop()` is still two minutes: relay timing and the error
blink survive it only because they are CRITICAL tasks on their own runner. The
web server is up throughout — `webSetup()` is step 8 and the waits are step 9 —
so `/data.json` answers, with accumulators that have never been fed.

**TRAP — a bad config is still fatal, it is just visible now.** `ConfigFile::loadFile` returns false when `/config.json` is missing, unparseable, has an `"id"` that does not match this chip's efuse MAC, or has any credential string shorter than 4 chars. `main.cpp` then continues with **compiled defaults** (`ssid = "undefined"`), which cannot associate — so the device wedges in the loop above. The error blink does now run (`ledBlink` is a critical task enabled before the blocking waits, and `g_ledBlinkEnabled` is set before `tasksSetup()`), but the device is still stuck. **A slow-blinking LED means "config did not load".**

Three traps that used to live here are fixed; do not re-introduce them:

- `AccumulatorV2::getLast()` now returns 0 on an empty window instead of dereferencing `std::list::back()`.
- **Never read an accumulator from a request handler.** ESPAsyncWebServer runs handlers on the `async_tcp` task while the io task does `push_back`/`pop_front` on the same `std::list` at 1 Hz; walking a list across a `pop_front` dereferences a freed node. `/data.json` avoids it by being rendered from `ioTaskHandler` into a cached string the handler copies under a mutex.

  This trap was written down here **and violated anyway**, by `/moisture.json`, which needs live values rather than a cached payload. The fix generalises: `moistureReading(index)` returns a snapshot published by the io task under a spinlock, and `moistureState()` reads it too, so it is safe from any thread. **A new endpoint that needs a live sensor value uses that, or publishes its own snapshot the same way.** Grepping for `g_soilMoisture` outside `sensors.cpp` and the background tasks should find nothing.
- `g_otaEnabled` is cleared in `handleUpdateRequest`, so a single `/updateEnable` no longer arms the device until reboot.

---

## Task scheduler

`CriticalTaskScheduler` (`andrenepomuceno/CriticalTaskScheduler@^1.0.3`, aliases `TSTask` / `TSScheduler`). Tasks are declared with the `DECLARE_TASK(name, period)` macro at the top of `src/tasks.cpp`, which mints `name##TaskHandler`, `g_##name##TaskPeriod` and `g_##name##Task` together. Registration happens only in `tasksSetup()`.

| Task | Period | Bucket | Notes |
|---|---|---|---|
| `relays` | 50 ms | **critical** | Switches each relay off when its timer expires |
| `ledBlink` | 1 s | **critical** | Only blinks while `g_ledBlinkEnabled` |
| `io` | 1 s | background | All ADC reads, `webUpdateDataCache()`, then the relay / float / **cloud** / **step-value** events. The cloud model rides this tick rather than taking a slot of its own — 59 ticks in 60 only add to a running sum |
| `dht` | 1 s | background | Enabled only when `config.dhtFitted`; tracks `g_dhtReadErrors` / `g_dhtTotalReads`, and a reading outside the DHT11's rated range is discarded and counted as an error |
| `checkInternet` | 15 s | background | |
| `history` | `history.periodSec` (60 s) | background | One `IoRecord` appended to the newest segment; the 60 s here is only the fallback until `tasksSetup()` calls `setPeriod()` |
| `schedules` | 20 s | background | Fires a due schedule; three ticks a minute, with a 10 min catch-up window |
| `mqtt` | `mqtt.publishSec` (5 min) | background | Builds and publishes the **periodic** payload for whichever backend is configured. The compiled 1 min is only the fallback until `tasksSetup()` calls `setPeriod()`, exactly as for `history`. Step values do not ride this tick at all |
| `talkBack` | 1 min | background | Polls the TalkBack queue |
| `clockUpdate` | 24 h | background | NTP re-sync |
| `logBackup` | 1 h | background | `logger.backup()` |
| `moistureModel` | 24 h | background | Trains the classifier; three streaming passes over the history, so it stalls other background tasks for seconds |
| `checkMoisture` | 4 h | background | Armed by `startRelay(0, …)`, disables itself |

Both buckets are registered with the same `g_taskScheduler.addTask()`; `DECLARE_CRITICAL_TASK` passes `critical = true`, which routes the task to `executeCritical()` on the `TSFreeRTOSCriticalRunner` thread. **Register and enable every critical task before `g_criticalRunner.start()`** — the scheduler holds no internal locks and the library documents mutation-after-start as unsafe.

Consequences that bite:

- **`execute()` runs at most ONE background task per call** — the latest-due one. A handler that blocks stalls every other *background* task for its whole duration.
- **`checkInternetTaskHandler` blocks on up to three `Ping.ping(addr, 2)` calls**, `talkBackTaskHandler` on a socket with a 5 s timeout, and `mqttTaskHandler` on draining up to 30 queued messages over TLS. Relay switch-off and the error blink survive that only because they are critical — putting relay timing back on the cooperative pump means a pump that runs long.
- Background tasks reschedule from **end of callback**, so a slow handler pushes its own next run out; critical tasks reschedule from the tick time and keep a fixed cadence.
- **A request handler must never call `ESP.restart()` or block.** `request->send()` only *queues* the response and the `async_tcp` task that flushes it is the one the handler runs on, so both kill the connection before the client sees anything. `/control` sets a flag and `requestRestart()` reboots from `loop()` 500 ms later; moving the restart after the `send()` was tried first and was not enough.
- **`g_relay[]` is touched from three threads** — the critical runner, `loop()` (TalkBack) and `async_tcp` (`/control`). Every access goes through the `g_relayMux` spinlock, and `relayWrite()` is deliberately called *outside* it: a `digitalWrite` inside a `portENTER_CRITICAL` section is exactly the kind of work that must not hold a spinlock.
- **`Scheduler::addTask()` returns `bool` and every call site ignores it.** The cap is `CRITICALTASKSCHEDULER_MAX_TASKS` = **16** per bucket. This repo registers **11 background and 2 critical** today; one task per relay plus per sensor crosses the cap and tasks are then **silently dropped**. Either check the return value or raise it with `-D CRITICALTASKSCHEDULER_MAX_TASKS=32`.

---

## Configuration

`ConfigFile` is a singleton (`config`) whose members are also exposed as `g_*` **reference aliases** bound in `config.cpp`. Old code uses `g_*`; new code should prefer `config.<field>`. Do not add new aliases.

**Adding a config key = 5 edits, all required:**
1. field in `include/core/config.h`
2. default in the `ConfigFile()` constructor (`src/config.cpp`)
3. parse in `ConfigFile::loadFile()` — a sensor's pin is parsed inside `if (xFitted)`, and the fitted flag is `io.hasOwnProperty("key")`
4. key in `data/config.template.json`
5. row in the README config table

Facts worth knowing before touching it:

- **`config.id` must equal `ESP.getEfuseMac() % 0x10000` in hex.** A config from another device is rejected wholesale. The id is printed on every boot as `ID: 1a2b`.
- **A filesystem deploy also wipes the history ring and the moisture model.** `uploadfs` and the filesystem OTA rewrite the whole 512 KB partition, so `/io_history.bin` and `/moisture_model.bin` go with the web assets — the watering-event counter restarts from zero and the classifier waits another six waterings. That happened three times in one day here. **Changed only files under `data/`? Use `POST /spiffs/upload`.** The whole-image path is for when the firmware changed too, or when a struct grew and the stored file would be discarded anyway.
- **A filesystem deploy overwrites `/config.json` with `data/config.json`, and a masked GET cannot be restored from.** Back up with `GET /config.json?secrets=1` (ADMIN, logged with the caller's IP) BEFORE any `uploadfs` or filesystem OTA, and write the result into `data/config.json`. Taking a "backup" through the normal masked GET loses every credential — eight asterisks each — and the loss only surfaces at the next boot.
- **`GET`/`POST /config.json` exist (both ADMIN) and secrets are masked in transit.** `GET` replaces `wifi.password`, `ota.password`, `thingSpeak.apiKey`, `talkBack.apiKey` and `mqtt.password` with `********` (`g_configSecretMask`); `POST` restores any field still carrying the mask from the document on disk. This deliberately diverges from fullbot, which serves the raw config — including plaintext credentials — to any authenticated session.
- **`POST /config.json` replaces the whole file**, exactly like fullbot's: `ConfigFile::saveFile()` opens with `FILE_WRITE` (truncate). The handler merges into the stored document first, so a caller only has to send a complete document, not a diff. **Nothing re-reads the file at runtime** — the response carries `restartRequired: true` and the change lands on the next boot.
- The handler rejects a document whose `id` does not match `config.deviceId`. Without that check a config pasted from another garden would be written, `loadFile()` would reject it at boot, and the device would come up on compiled defaults it cannot connect with — unreachable to fix without USB.
- **Changing `ota.password` is pushed into `UserStore` too.** The login password lives in `/users.json`, so a config-only change would otherwise not take effect until a filesystem deploy wiped the user store.
- **Every `io` entry accepts more than one shape, and all of them must keep working** — a device in the field has to survive a firmware update. `io.relays`: array of `{pin,on,name}`, or the pre-2.0 `io.watering` + `io.wateringOn` scalars. `io.soilMoisture`: array of `{pin,name,powerPin,powerOn,settleMs}`, array of `{pin,name}`, array of bare pins, or a single bare pin — the three power keys are optional and absent means permanently powered. `io.dht` / `io.luminosity` / `io.waterLevel`: `{pin,name}` or a bare pin. `loadSensor()` handles the sensor cases in one place. Entries past the compiled count are ignored and missing ones keep their defaults — a short array logs a warning rather than zeroing a pin.
- **`mqtt.publishSec` (60..300, default 300) sets the PERIODIC payload's period only.** It is applied by `tasksSetup()` through `setPeriod()`, the same way `history.periodSec` is — so `g_mqttTaskPeriod` is now nothing but the compiled fallback the task is constructed on, and **reading it where the real period is meant is a live trap**. Two things must follow it and did not follow it for free:
  - **Every accumulator window is sized from it in `sensorsSetup()`, scalars included.** The scalars used to take their window from `g_mqttTaskPeriod` at *file scope*, which was right only while that period was a constant. Static initialisers run long before `config.json` is read — the exact trap that made a file-scope `DHT_Unified` run for ever on the compiled default pin. `g_pingTime` lives in `tasks.cpp` and is re-sized in `tasksSetup()` for the same reason.
  - **The publish queue depth is `3600000 / mqttPublishPeriodMs()`**, i.e. an hour of buffer at whatever period is configured: 60 messages at the floor, 12 at the ceiling, and it cannot reach zero for any value the clamp allows.
- **`postalCode` is metadata the firmware never acts on.** It is parsed, stored
  and otherwise inert: no geocoding, no validation, no behaviour depends on it.
  It exists so the off-device tooling knows where the device is without being
  told separately, and so the archive is self-describing. Optional — a device
  that has never been told where it is boots exactly as before. Named in
  English per the conventions even though the value here is a Brazilian CEP,
  because the device can be anywhere.
- **`mqtt.heartbeatSec` (60..3600, default 900) is the floor under change-based publishing**, not a publish period. See [Sampling vs events](#sampling-vs-events--the-rule-and-where-it-was-broken).
- **`et0.enabled` (default FALSE), `et0.latitude` and `et0.scale` gate the
  evapotranspiration estimate.** Off for a stronger reason than `cloud.enabled`:
  that one is unproven, this one was **measured to have no skill on this
  device** — the thermometer follows 35 % of the outdoor swing, so the diurnal
  range Hargreaves-Samani runs on describes the enclosure. `latitude` has no
  sensible default (0.0 is the equator, a real place), so an unset one cannot be
  detected and is instead logged at boot; a value outside ±90 is refused rather
  than clamped, because a clamped typo hides which number was wrong. `scale`
  ships at 1.0 — the textbook formula, nothing fitted — is refused outside
  0.5..2.0, and logs a warning whenever it is not 1.0, because a value far from
  it is a siting fault wearing a calibration coefficient. See
  [Evapotranspiration](#evapotranspiration-the-fit-said-no-and-that-is-the-result).
- **`cloud.enabled` (default FALSE) gates the cloud-cover model entirely** — the
  computation, the `/data.json` badge, `cloudState` and `cloudVariability`. The
  default is off for the same reason `floatInterlock` is: the clear-sky table
  compiled into the firmware is an upper envelope of one sensor's own history at
  one mounting, and on any other board every reading under it reads as cloud.
  Enabling it is a claim its owner makes after running `scripts/cloud_fit.py`
  against that device's own archive.
- **Sensor and relay names are LABELS, not identifiers.** `/data.json` keys `Inputs` and `Outputs` by them, so renaming changes the dashboard immediately — but the `Relays` array stays index-addressed, the history record is positional, and the ThingsBoard telemetry keys stay `moisture1..N`. Renaming therefore never rewrites stored history. A name on `io.dht` is a *prefix*, because one pin produces two channels.
- **`"version"` in the JSON is still never read** — the template says `2` but nothing enforces or migrates on it. `log.level` *is* read now (clamped to `LOG_DISABLE..LOG_TRACE`) and applied in `loadConfigFile()`.
- **`loadFile()` mutates fields as it parses and only returns `false` at the end**, so a rejected config leaves a half-populated object behind.
- **`GET /config.json` is reachable** (matched by the trailing `serveStatic("/", FILESYSTEM, "/")`) and returns Wi-Fi, MQTT and OTA passwords in plaintext. It is behind HTTP Basic auth; nothing else protects it. `fullbot-firmware` fixed the equivalent by moving credentials into a salted-hash `UserStore` and shadowing the path with an explicit 403 handler registered *before* the static handler.

---

## Web server, endpoints & OTA

`src/web.cpp`, one `AsyncWebServer` on port 80, mDNS as `<hostname>.local`.

| Route | Method | Auth | Handler |
|---|---|---|---|
| `/`, `/index.*`, `/login.*`, `/update.*`, `/auth.js`, `/sha256.js`, `/favicon.ico` | GET | **public** | `servePublicFile()` — an explicit allow-list, no blanket `serveStatic` |
| `/nonce` | GET | **public** | issues the login challenge |
| `/login`, `/logout` | POST | **public** | `CustomLogin` |
| `/data.json` | GET | session | serves the cache built by the `io` task |
| `/control` | POST | **OPERATOR** | `relay`+`relayTime`, `relay`+`relayStop` (stop a running relay; explicit rather than a zero `relayTime`, because `String::toInt()` answers 0 for unparseable input and a malformed field must not stop a pump), `watering`, `wateringTime`, `mqtt`, `reset` |
| `/history.json` | GET | session | last N I/O snapshots, `?limit=` (cap 200) |
| `/logs` | GET | **ADMIN** | the whole 8 KB log buffer as `text/plain` |
| `/config.json` | GET/POST | **ADMIN** | read with secrets masked / write the whole document. `?secrets=1` exports verbatim for a restorable backup |
| `/config.html`, `/config.js` | GET | **public** | configuration editor in tabs (the data behind it is ADMIN) |
| `/history.html`, `/history.js` | GET | **public** | charts over 1 h / 6 h / 12 h / 1 d / 7 d / 30 d |
| `/users.html`, `/users.js` | GET | **public** | account management page (the data behind it is ADMIN) |
| `/schedules.html`, `/schedules.js` | GET | **public** | scheduled watering editor |
| `/moisture.html`, `/moisture.js` | GET | **public** | the classifier's inference and its fitted parameters (data behind it needs a session) |
| `/moisture.json` | GET | session | per-probe class means, spreads, priors, separation, watering response, absorption &tau;, and which gate refused an unclassified probe |
| `/capabilities.json` | GET | session | per-kind maxima, the kinds this build has drivers for, and the usable pins — so the UI never restates a rule the firmware owns |
| `/spiffs/upload` | POST | **ADMIN** | replace ONE file instead of rewriting the partition |
| `/updateEnable`, `/update` | POST | **ADMIN** | OTA arm + upload |
| `/users.json` | GET | **ADMIN** | usernames and roles only — never the salt or hash |
| `/users` | POST | **ADMIN** | `action=upsert\|delete`, `username`, `password`, `role` |
| `/devices.html`, `/devices.js` | GET | **public** | sensor + actuator management page (data behind it is ADMIN) |
| `/spiffs/*` | GET | **ADMIN** | file browse, with `users*` / `sessions*` / `config*` shadowed 403 |
| `/spiffs/delete` | POST | **ADMIN** | remove ONE file, refusing exactly what the upload refuses |

**Auth is nonce + SHA-256, not Basic Auth** (`USE_CUSTOM_LOGIN=1`, `src/custom_login.cpp`, ported from fullbot):

1. `GET /nonce?username=<u>` → `{nonce, salt, ttlMs}` — an unknown user still gets a deterministic decoy salt derived from `config.deviceId`, so the endpoint cannot enumerate accounts.
2. `passwordHash = sha256hex(salt + ":" + password)`
3. `response = sha256hex(nonce + ":" + passwordHash)`
4. `POST /login` with `username`, `nonce`, `response` (+ optional `remember=true`) → `{token, role, ttlMs}`
5. every later request carries header **`Authorization-Token: <token>`**

**TRAP:** `curl -u user:pass` returns 401 on every guarded route — there is no Basic-Auth path. Nonces are one-shot with a 30 s TTL; sessions idle out after 24 h; 5 failures from one IP → `429 Retry-After: 60`. `remember=true` persists the token to `/sessions.json`, so **a filesystem deploy signs everyone out**.

**A `Session` stores the user's INDEX, not the username.** `UserStore::remove()` erases from a `std::vector`, so every later entry shifts and a live session silently starts resolving to a different account — and to its role. `POST /users` with `action=delete` therefore calls `customLogin.invalidateAllSessions()` and answers `{"reauth":true}`; the page signs itself out. Any future code path that reorders the store owes the same call.

**There is no default password compiled into the firmware.** `UserStore::load()` seeds the first account by migrating `config.json`'s `ota.username` / `ota.password` into `/users.json` as ADMIN (salted SHA-256). A device whose config never loaded has no users, logs a FATAL, and the web UI is unreachable by design.

**Registration order is load-bearing.** ESPAsyncWebServer matches handlers in the order added, and `AsyncURIMatcher::prefix` is a prefix match — the 403 shadows for `/spiffs/users`, `/spiffs/sessions` and `/spiffs/config` must stay *above* the `serveStatic("/spiffs", …)` line, or the credential store, the live bearer tokens and the plaintext WiFi/MQTT passwords are served to any admin session.

**A slow OTA is almost certainly SELF-INFLICTED.** The web UI pushes a 1.2 MB
image in under a minute; a script here took ~7 minutes for the same image, and
the difference was that script's own wait loop from a previous run, still
logging in every 5 s while the upload was in flight. The board serves HTTP from
a single `async_tcp` task, so anything polled during an upload competes with it
— and `kMaxSessions` is 4, so a loop that authenticates also evicts whoever is
using the browser. This file already recorded the same mistake once, under the
filesystem upload; it was made again. **Wait on liveness with an unauthenticated
GET of a public path, authenticate once at the end, and send nothing else while
an upload is running.**

**TRAP — an OTA client that times out has NOT necessarily failed.** A 1.2 MB
upload takes minutes; the device writes, verifies the MD5 and reboots, and by
then the HTTP connection the client was waiting on is long gone. Reading the
version 30 seconds after arming reports the OLD firmware because the old
firmware is still the one running. Both happened here and the second was
misdiagnosed as a failed flash. **Confirm by polling until the version changes
or the uptime resets, not by the upload's return value** — and run the upload
in the background, because a 10-minute tool timeout landing mid-write is how
this session already corrupted a filesystem once.

**TRAP — a failed browser OTA used to reboot the board, and then poison the
retry.** Three faults in one path, all found while an operator was trying to
flash from `/update.html` and getting `ERR_CONNECTION_RESET`:

- `handleUpdateRequest` restarted on `!Update.hasError()`, which is FALSE when
  `Update.begin()` was never reached. An upload that died partway therefore
  rebooted the device on a half-written partition, and that reset killed the
  next attempt's connection — five `software (ESP.restart)` boots in the log
  with the firmware unchanged. It now requires `Update.isFinished()`.
- A partial write leaves `Update` RUNNING, so the next `Update.begin()` refuses
  because one is already in progress: a single interrupted upload poisoned
  every retry until a power cycle. Every failure path now calls
  `Update.abort()`.
- The reboot was `delay(500); ESP.restart()` inside the handler. `send()` only
  QUEUES the response and the `async_tcp` task that flushes it is the one the
  handler runs on, so the browser was guaranteed to see a reset rather than
  "OK" — the same trap this file already records for `/control`. It now calls
  `requestRestart()`.

**The page no longer believes the upload's return value.** A finished flash
ends in a reboot and the reboot kills the connection the browser is waiting on,
so a successful update arrives at the client as an error — an operator watched
`/update.html` report "Upload failed" at 100 % three times while the firmware
on the board had in fact changed. `update.js` now reads the version before
sending, and afterwards polls `/data.json` until it changes: a different version
is success, a 401 means the reboot signed the browser out (also success, said as
such), and only a deadline with nothing changed is reported as a failure. A
4xx from the device is still shown verbatim, because that is a real refusal with
a real reason and the device is still up to be asked. `scripts/dev_server.py`
bumps its reported version on a simulated firmware upload so the page can be
exercised against the mock at all.

OTA details: `/updateEnable` arms a module-level `g_otaEnabled` which `handleUpdateRequest` clears again, so one arm buys one upload. `handleUpdateUpload` picks `U_SPIFFS` when the uploaded *filename* is exactly `filesystem`, else `U_FLASH`; `data/update.js` renames the file to the selected radio value (`firmware` | `filesystem`) and sends an `MD5` form field computed client-side with SparkMD5. On success the device `delay(500)` then `ESP.restart()`.

`/data.json` shape — the contract shared by `data/index.js`, `scripts/dev_server.py` and any future frontend:

```jsonc
{ "Status":  { "Hostname": "...", "Firmware": "2.0.0", "Uptime": "...", "Internet": "online|offline", ... },
  "Inputs":  { "Soil Moisture 1": { "val": "...", "avg": "...", "var": "..." }, ... },
  "Outputs": { "Watering": "0|1", "Relay 2": "0|1", ... },      // keyed by relay NAME
  "Relays":  [ { "index": 0, "name": "Watering", "on": 0, "remaining": 0 }, ... ],
  "Channel": "1348790" }
```

The luminosity entry carries **`state`** — `clear`, `partly cloudy` or
`overcast` — and only while `cloud.enabled` is on AND the local time is inside
the model's fitted daylight window. Absent, not empty, exactly as a probe's
`state` is.

A moisture entry also carries **`fault`** — `noisy`, `floating`, `railed` or
`stuck` — and ONLY when there is one, exactly as `state` is absent rather than
empty. A dashboard is read at a glance, and a column saying "connected" on
every healthy probe trains the eye to skip the one case that matters.
`/moisture.json` carries the evidence behind it.

`Inputs` and `Outputs` keys are **human-readable labels, not identifiers** — `Outputs` is keyed by the configurable `relayName`, so it is display-only. Anything that needs to *address* a relay uses the `Relays` array and its `index`. With one probe the moisture label stays `"Soil Moisture"` (no suffix) so existing dashboards keep working; with two it becomes `"Soil Moisture 1"` / `"Soil Moisture 2"`.

The current UI (`data/index.html`) loads **Bootstrap and jQuery from CDNs**, so the dashboard degrades without internet — on a device whose whole point is surviving connectivity loss. `fullbot-frontend` bundles everything with Webpack and ships gzipped assets into `data/frontend/`; that is the fix.

---

## Sensors, accumulators & telemetry

Every sensor compiles into every image; `config.*Fitted` and `config.moistureCount` decide which are read. See [Runtime hardware](#runtime-hardware--the-build-no-longer-knows-what-is-fitted).

**Adding a sensor KIND = 8 edits** (adding an *instance* of an existing kind is now a web-UI edit): `config.h` pin field + fitted flag → `config.cpp` default, parse and `validatePins()` role → `config.template.json` → `sensors.cpp` accumulator + read → `sensors.h` extern → `telemetry.cpp` payload → `web_data.cpp` `Inputs` block (gated on the fitted flag) → `web_capabilities.cpp` kinds list → `dev_server.py` mock. Miss the last one and the simulator lies.

- Accumulator windows are sized as `mqttPublishPeriodMs() / g_<source>TaskPeriod`, so each average covers exactly one publish interval: at the 5 min default that is **300 samples** for luminosity, moisture, water level, flow and the DHT, and 20 for the ping. **`sensorsSetup()` sizes EVERY accumulator, scalars included**, and `tasksSetup()` sizes `g_pingTime`. The scalars used to take their window from `g_mqttTaskPeriod` at *file scope*, which was correct only while that period was a constant — it is `mqtt.publishSec` now, and static initialisers run long before `config.json` is read. Same trap as a file-scope `DHT_Unified`, same fix: the constructor argument is only a safe starting length. Arithmetic, not a measurement: the windows cost 9 × 300 × 4 B + 20 × 4 B ≈ **10.6 KB of heap at 300 s against 2.1 KB at 60 s**, allocated once at setup.
- Conversions live in macros at the top of **`sensors.cpp`**: `ADC_TO_PERCENT(x) = x*100/4095`, and the water level uses a fitted curve `9 - 12*sin(4.04 - 1.61*V)`. Moisture is **inverted per probe, not per board** — `100 - pct` only when `moisture[i].invert` is set, which it is by default. It used to be unconditional, and that was one sign for a board that can carry a capacitive probe and a resistive one at once.
- **`AccumulatorV2` is a fixed-size ring buffer** and allocates exactly once, when its window is sized. It used to be a `std::list<float>` doing a push/pop per sample at 1 Hz forever, which contradicted the "no dynamic allocation in steady state" rule and never stopped fragmenting the heap. `test_steady_state_does_not_allocate` counts array `operator new` and asserts zero across 5000 samples, so the rule is now a red test rather than a note. **`getAverage()` is O(1)** — the running `sum`/`sumSq` are maintained by `add()`. It used to walk the list TWICE per read, and putting that inside a spinlock is what panicked the board with an interrupt watchdog. Incremental sums drift, so `resync()` recomputes once per full window: amortised O(1), bounded error. A non-finite sample is dropped rather than poisoning the sums permanently.
- **A DHT reading outside the part's RATED RANGE is a bad read, not weather, and is counted as one.** The DHT11 is specified 0–50 °C and 20–90 % RH; this device published `airHumidity` down to **15.1 %** — ten points below anything the sensor can resolve — while `dhtErrorRate` read 0.00 throughout. Two failures compounding: the out-of-range sample was averaged into the window every dashboard and every stored point is drawn from, *and* it was counted as a successful read, so the one counter that exists to say the sensor is misbehaving said the opposite. `sensorsReadDht()` now discards it **and increments `g_dhtReadErrors`** — dropping it silently would fix the first and keep the second. The bounds are the datasheet's, in named constants beside the gate: widen them and the gate stops meaning "the sensor cannot have measured this". *(The other reading in that audit, `temperature` at 45.04 °C, is INSIDE the rated band and this gate does not touch it — and it turned out to be a real 38-minute patch of direct sun on the sensor, not a glitch. See [Evapotranspiration](#evapotranspiration-the-fit-said-no-and-that-is-the-result).)*
- **The DHT is placement-`new`ed into `g_dhtStorage` inside `tasksSetup()`**, not constructed at file scope. `DHT_Unified` copies the pin in its constructor, and at static-init time `config.json` has not been read — a file-scope instance permanently ran on the compiled default and silently ignored `io.dht`. Keep the construction late; the storage buffer exists so this costs no heap.
- The MQTT payload is accumulated into a global `String g_mqttMessage` by `mqttAddField()` / `mqttAddStatus()` **called from several tasks** (watering start, connectivity change) and flushed by `mqttTaskHandler`. Safe today only because one background task runs at a time; a critical task must not call it without a lock.
- The publish queue holds at most `60*60*1000 / mqttPublishPeriodMs()` messages — an hour of buffer at whatever period is configured, so **60 at the 60 s floor and 12 at the 300 s ceiling** — **in RAM**, dropped oldest-first, lost entirely on reboot.

  **Backing it to flash was looked at and deliberately NOT done. See the reasoning before implementing it**, because the obvious version is unsafe here. `fullbot-firmware`'s `/telemetry_backup.json` writes to an **SD card**, appends one `<topic>\t<payload>\n` line per spilled message, and on drain writes a temp file and renames — and it carries its own `// TODO check file size or line count limits`, so it is unbounded. None of that transplants: this board has no SD, so the file would land on the same 512 KB LittleFS partition as the web assets, the history segments and the moisture model, with ~124 KB free. A flash writer on that partition is exactly what [reboot-looped this board](#the-ring-buffer-does-not-survive-the-move-and-the-reason-generalises), the device is now reachable only over the air, and none of this can be exercised on hardware from here.

  What it would have to be, if someone does it: **append-only** (never patched in place), **written only while the link is DOWN** so the happy path costs zero flash writes, **bounded in bytes** with the oldest dropped, and drained by publishing then deleting the file wholesale. What it buys is one hour of averaged sensor means across a reboot — and B3 already moved every step value off the queue, so what is at risk is 12 payloads of slow-moving levels. That is a poor trade against a second writer on the partition that has already bricked this garden once.
- **TalkBack is plain HTTP on port 80** with the API key in the request body (there is a `// TODO use HTTPS` in `talkback.cpp`). The only command parsed is `watering:<ms>`, capped at 20 000 ms by `startWatering()`.
- **`data/thingspeak.pem` pins the CA for the MQTT TLS connection, and a stale pin fails silently for years.** It used to hold the *intermediate* `DigiCert TLS RSA SHA256 2020 CA1`; ThingSpeak now serves a chain under `DigiCert Global G2 TLS RSA SHA256 2020 CA1` / `DigiCert Global Root G2`, so every connect returned `-9984 X509 - Certificate verification failed` and channel 1348790 received nothing between **2023-03-07 and 2026-08-19**. Nothing surfaces this: `/data.json` keeps reporting `MQTT: enabled`, and only `Packages Sent` staying at 0 gives it away. It now pins the **root**, not the intermediate — roots last until 2038, intermediates rotate. Verify with `openssl s_client -connect mqtt3.thingspeak.com:8883 -showcerts` before assuming the file is current.
- **`mqttLoop()` retries the connection on every `loop()` iteration with no backoff**, so a TLS failure produces hundreds of log lines per minute and drowns everything else in the serial log. Worth a backoff if you touch `mqtt.cpp`.
- **`/data.json` reports the LINK, not just the switch.** `MQTT` is the
  operator's toggle (`enabled`/`disabled`, and `index.js` binds a checkbox to
  it); `MQTT Link` is `connected` or `down (rc=N)` from PubSubClient, and
  `Last Publish` is the age of the last publish the broker accepted. Conflating
  the two is what made the 2023-2026 outage invisible: the dashboard said
  `MQTT: enabled` throughout, and only `Packages Sent` staying at 0 gave it
  away — a counter that resets every boot, so on a device up for an hour it
  reads the same whether the broker took everything or nothing.
- **The pinned bundle is five roots**: ISRG Root X1 (to 2035), X2 (2040),
  USERTrust ECC (2038), and — added 2026-08-25 — **ISRG Root YR and YE, both to
  2045**. `mbedtls_x509_crt_parse` accepts a concatenated PEM bundle, which is
  what makes pinning several roots one file.
  
  YR and YE were added because the live `thingsboard.cloud:8883` chain
  terminates in **`ISRG Root YR` cross-signed by X1**, and that cross-signature
  is transitional by design: it exists only until the new root is widely
  trusted. Pinning X1 alone worked *because of it*, and would have failed
  silently the day it was withdrawn — the exact signature of the 2023-2026
  outage. YR and YE mirror X1 and X2: Let's Encrypt's `gen-y` hierarchy has an
  RSA root and an ECDSA one, and which issues a given certificate is the CA's
  choice, not ours.
  
  **Verified, not assumed** (`openssl s_client -CAfile`): the live chain
  validates against the new bundle, and — the check that matters — against
  **ISRG Root YR ALONE**, which is what proves the anchor still works once the
  cross-signature is gone. Both new roots are self-signed
  (`openssl verify -CAfile self self`), so they are trust anchors and not
  cross-signed copies wearing the same name.

---

## Soil moisture: a classifier trained on watering events

Three sources decide the Dry/Humid/Wet badge, in this order, and the ladder
matters — each rung is only reached when the one above refuses:

1. **The trained model** (`src/moisture_model.cpp`) — Gaussian naive Bayes,
   one Gaussian per class per probe, fitted from the probe's own history.
2. **The two-point calibration** (`moisture[i].dry` / `.wet`) — thirds of the
   probe's measured span.
3. **Nothing.** An empty string, and `/data.json` omits `state` entirely.

### Why the labels come from the relays and not from the data

Clustering the history was tried first and it produces confident nonsense.
Measured on 29.7 days / 20 417 samples: a linear drying trend alone explains
**86.4 %** of the variance (-0.323 points/day). With the trend removed, BIC on
the residuals prefers k=2, and that second component sits at **-24.5** — the
near-zero readings of a disconnected probe, not a soil state. Clustering the
*raw* series does return three groups (8.3, 35.9, 41.3), which are the outliers
plus two arbitrary slices of the trend: thresholds from them label "dry"
whatever comes late in any drying period.

The relay record is what changes the problem. A watering is an **event**, and
an event is a label:

| Window | Label | Why |
|---|---|---|
| `(T, T + 30 min]` after a watering | **Wet** | Not from T itself: absorption takes time, and the reading at the pump's own edge is still the old soil |
| `[T_next - 60 min, T_next)` | **Dry** | Arithmetic, not assumption — moisture decreases monotonically between waterings, so the minimum of a cycle is just before the next one |
| everything between | **Humid** | |
| before the first event in the buffer | *none* | There is no cycle to place the reading in |

**Every sample carries a CONFIDENCE, not just a label**, passed as the weight
`gaussianAdd()` has always accepted. Until it was used, a reading taken at the
instant the pump started counted as firmly "wet" as one taken twenty minutes
later. Boundary samples are precisely what blurs the class means together, and
blurred means are what fails the separation gate.

- **Wet** follows the probe's own absorption curve — see [Absorption](#absorption-the-soil-is-a-diffusion-process-not-a-step)
  below. Soil does not become wet when a pump starts; for the first minutes the
  probe is still reading the old soil. Where no time constant has been measured
  yet, a linear ramp over `g_absorptionLagSec` (5 min) stands in.
- **Dry** ramps up as the next watering approaches. Moisture falls monotonically
  between waterings, so the closest sample is the driest and the most confident.
- **Humid** tapers toward both neighbours over `g_taperSec` (10 min): a reading
  one minute outside the wet window is not meaningfully more humid than one a
  minute inside it.

**The watering RESPONSE is tracked and reported** — the mean rise from the dry
window to the wet one, decayed across runs like everything else. It is free,
because the labels the fit already needs are exactly what it is made of, and it
is the cheapest evidence that a probe is in the pot its pump waters. A response
near zero after two waterings is reported as `blockedBy` **before** the
statistical gates, because it names a physical cause — disconnected probe,
wrong pot, pump not running — where "bands overlap" would arrive days later and
name only the symptom.

**What is deliberately NOT a feature: time since watering.** Adding it would
raise accuracy and destroy the point. The model exists to say what the SOIL is
doing, read from a probe; a classifier that leans on the pump schedule is a
timer wearing a posterior, and it would report "wet" confidently through a
disconnected probe or a failed pump — the exact failures the separation gate
and the response check are there to catch. The watering record is used to
LABEL the training data and never as an input at classification time.

`moisture[i].relay` says whose pump matters. **-1 means no pump feeds this
probe**, and such a probe never gets a model at all — nothing labels its
readings — so it falls through to the two-point calibration.

### Why training accumulates instead of refitting

The history buffer holds 24 h and a zone is watered once or twice a day. A
from-scratch daily fit therefore has **one or two events**, which is a
description of yesterday rather than a model. So `moistureModelTrain()`
multiplies the stored sufficient statistics by `g_moistureDecayPerRun` (0.93,
a half-life of about ten days) and folds the new day into them. All three
moments decay together, so the *estimate* is unchanged and only the confidence
in it ages — which is the intent: yesterday's soil is still evidence about
today's, just less of it.

Gaussian naive Bayes was chosen partly for this: it needs only `weight`, `sum`
and `sumSq` per class, so training **streams** over the history file
(`IoHistory::forEach`) instead of holding 69 KB of records in RAM, and the
whole model is 12 doubles per probe.

### Absorption: the soil is a diffusion process, not a step

Water reaching a probe takes time, and how long depends on the soil, the pot,
and how well the probe touches either. That is a property of ONE probe and
cannot be a constant shared by four of them — which is what
`g_absorptionLagSec` was, a five-minute guess applied to every probe on the
board.

Treated as first-order diffusion the reading approaches its new level as
`m(t) = baseline + rise * (1 - e^-(t-T)/tau)`, and `tau` is estimated per probe
from its own rises: the 63.2 % crossing of the step, interpolated between the
two samples that straddle it. `moistureTimeConstant()` in
`src/moisture_classifier.cpp` is the whole of it, host-tested, free of Arduino.

- **The baseline is the last reading BEFORE the pump, not the first one after.**
  At the pump's own edge the soil still reads its old value; starting from the
  first post-watering sample measures the rise from a point already part of the
  way up it, and returns a `tau` that is too small.
- **Interpolation is not decoration.** At a 60 s history period the raw crossing
  is quantised to a whole minute, so a probe that answers in three minutes is
  measured with a 30 % error.
- **It refuses more often than it answers, on purpose.** Fewer than
  `g_riseMinSamples` (5) samples, a rise under `g_riseMinPoints` (1 point), or
  no crossing inside the 30-minute wet window all return 0, which means
  *unmeasured* and never *instant*. A probe that does not answer its pump must
  not be handed a confident time constant — it is precisely the probe the
  response check and the separation gate exist to catch.
- **Polarity is not assumed**, exactly as nothing else here assumes it: the
  crossing is tested in the direction the rise actually went.
- **Only pass 2 measures it.** Pass 3 walks the same records and would count
  every curve twice, and the 3-sigma outlier rejection does not apply to a rise
  at all — a transient is a shape, and rejecting its samples against a class
  mean would flatten the very thing being measured.
- **It is decayed like every other statistic** (`g_moistureDecayPerRun`), so one
  watering measured through noise cannot rewrite the estimate, and a probe
  slowly losing contact with its soil shows up as a drifting `tau` well before
  the separation gate refuses it.

Feeding `tau` back is the point: `moistureAbsorptionConfidence(dt, tau)` is the
weight a wet-window sample carries, so a fast probe reaches full confidence in
three minutes and a slow one is still discounted at fifteen. The 5-minute ramp
remains as the stand-in for a probe with no measurement yet.

**`tau` is a WEIGHT on training samples, never a feature at classification
time** — the same line the watering schedule is held to, and for the same
reason. A model that reads the clock is a timer wearing a posterior.

### The gates — refusing is the feature

`moistureModelIsUsable()` reports nothing unless **all** of these hold:

- **`g_moistureMinEvents` = 6 watering events** (decayed). A model fitted to
  one cycle describes that cycle.
- **`g_moistureMinWeightPerClass` = 20** accumulated weight in every class.
- **`g_moistureMinSeparation` = 4** — Fisher's `J = (μ_wet - μ_dry)² / (σ²_wet +
  σ²_dry)`, so the means are at least two pooled standard deviations apart. A
  probe watered so often it never dries lands near 0 here.
- **Ordering**: humid must lie *between* dry and wet. Not decoration — if it
  does not, the labelling disagrees with the physics that produced it, and
  every classification is a coin toss wearing a posterior. Polarity is not
  assumed, exactly as the two-point calibration does not assume it.

`/moisture.json` reports **which gate refused**, per probe. A classifier that
silently declines is indistinguishable from one that is broken.

### Traps

- **The fit is two passes, and it has to be.** Pass one fits everything; pass
  two refits rejecting samples beyond 3σ of that first fit. The rejection
  threshold is a function of the fit it protects, so one pass cannot do it.
  This is what keeps an out-of-family reading from dragging a class mean to a
  value the soil never had — the same component BIC found when the history was
  clustered blind.

  **It does not catch every disconnected probe, and it does not have to.**
  Measured on this board: with probe 3 unplugged, floating GPIO 32 reads 52.6
  with a variance of 0.01 — mid-scale, quiet, and indistinguishable from soil
  by value alone. The 3σ test never fires on it. What catches it is the
  SEPARATION gate: a disconnected pin does not respond to its pump, so dry,
  humid and wet all land on the same number, J collapses toward 0 and the
  model is refused. That is the gate earning its place — the probe is rejected
  for the reason it is actually broken.
- **All probes are trained in the same three passes**, not three passes each.
  This is a background task, and background tasks are cooperative: twelve
  passes over 1440 records on a four-probe board would stall MQTT and TalkBack
  for ten seconds once a day.
- **The sticky relay mask means one watering spans several records.** Only the
  rising edge counts, or a long watering becomes several events.
- **Variance is floored** at `g_gaussianVarianceFloor`. A class whose samples
  are identical has zero variance, an infinite log-likelihood, and wins every
  comparison regardless of the reading.
- **Likelihoods are computed in the log domain.** They differ by many orders of
  magnitude here; computed directly they underflow to zero for every class at
  once, which reads as a tie.
- **The model file is discarded on a layout change** (`MOI1` magic +
  `sizeof(MoistureModelState)`), not reinterpreted. Weeks of evidence are
  cheaper to rebuild than a wrong band is to notice.
- Training also runs **5 minutes after boot**, not only every 24 h: a device
  power-cycled each evening would otherwise never reach its daily tick.

`test/test_moisture_classifier/` covers the maths on the host — the ordering
gate, the separation gate, inverted polarity, the variance floor, the outlier
z-score, that confidence tracks how much the classes actually overlap, and the
absorption estimator: that `tau` is recovered from a synthetic first-order rise,
that a slow probe is distinguished from a fast one, that a probe which did not
respond is refused, that inverted polarity gives the same `tau`, that too few
samples is refused, and that the confidence follows the exponential rather than
the old straight line.

### The probe is hardware, and the firmware stopped assuming which

Two things about a soil probe were baked into the code and are now per-probe
configuration, because a board can carry one of each.

**Polarity — `moisture[i].invert`.** `sensorsReadIo()` did
`100 - ADC_TO_PERCENT(...)`, one sign for the whole board. It is right for the
capacitive v2 modules these were built around: their 555 output FALLS as the
soil wets. A resistive divider does the opposite. The classifier never cared —
both the two-point calibration and the ordering gate accept either direction —
but the number on the dashboard and in the stored history would have run
backwards, and every chart with it. Default `true`, which is what every existing
board already does.

**Power gating — `io.soilMoisture[i].powerPin` / `powerOn` / `settleMs`.** This
is the difference between a resistive probe lasting a season and lasting weeks.
Its two electrodes sit in wet soil with a DC potential across them, which is an
electrolysis cell: the anode dissolves, the readings drift, and the probe is
scrap. Driving the module's VCC from a GPIO and energising it only around the
reading takes the duty cycle from 100 % to under 1 % at a 1 s period.

- **Coalesced.** Two probes on one MOSFET is the normal wiring, so every power
  pin is switched on, ONE settle delay is paid — the longest any probe asked
  for — and all probes are read. Powering them one at a time would pay it twice.
- **The first conversion after power-up is discarded.** The input was floating a
  moment ago and the SAR capacitor carries charge from the previous channel.
- **`settleMs` is capped at 250.** The delay runs inside the 1 Hz io task, which
  is the same cooperative pump MQTT and TalkBack share. A probe needing more
  than that is one to read less often, not one to stall every background task
  for.
- **The pin is parked OFF in `sensorsSetup()`**, before it is ever driven on,
  for the same reason `relayPinsSafeInit()` is the first statement of `setup()`.
- **`validatePins()` checks it as an OUTPUT**, and a pin shared between two
  probes is not reported as a duplicate — that is the intended wiring.
- **Current is the operator's problem, and the UI says so.** An LM393 module
  with its LED draws around 20 mA, which is most of what one ESP32 GPIO should
  source; two of them want a small MOSFET rather than a shared pin.

**Swapping a sensor now discards its model.** `MoistureProbeModel` identified a
probe by pin and relay, and replacing a capacitive module with a resistive one
changes neither — same hole in the pot, same pump — while inverting the transfer
curve. Weeks of Gaussians would have carried over and kept reporting a confident
badge, and the separation gate would not have caught it: the bands stay well
separated, they are simply the wrong bands. The identity now also carries
`sourceInvert` and `sourceTag`, a hash of `moisture[i].kind`. That free-text
label is the manual lever — relabel a probe and its statistics are thrown away —
and it is why `kind` is only written when non-empty, so an empty one does not
discard a model on every save.

### Is there a sensor on the pin? One test, and only one

**Every obvious statistic fails.** 200 history records over 4.3 h with all three
probes unplugged:

| probe | mean | sd | median step | distinct values |
|---|---|---|---|---|
| 0 | 87.2 | 4.37 | 1.00 | 176 |
| 1 | 61.1 | 2.65 | 0.02 | 30 |
| 2 | 50.2 | 1.29 | 0.03 | 96 |

- **Not the value.** All three sat mid-scale, as an earlier disconnected probe
  did at 52.6 and later at 86.7.
- **Not the correlation between probes.** 0 and 1 track at **+0.87**, but real
  probes in one garden, same schedule and weather, correlate just as hard.
- **Not the spread of those per-minute means.** That is what averaging removes.

What works is the spread **between CONSECUTIVE conversions**. Measured live
against the luminosity channel on the same ADC at the same moment:

| channel | state | var | sd |
|---|---|---|---|
| Luminosidade | **connected** | 0.02 | 0.14 |
| Umidade 1–3 | floating | ~800–1990 | 27.9–44.6 |

Four orders of magnitude, with a control on the same chip. Soil cannot jump
between one conversion and the next however fast it is watered; a floating pin
does nothing else. The threshold is **400 ADC counts**, and the same number
separates every case measured or modelled:

```
watering, 1200 counts over 5 min at 1 Hz        4
connected probe, level sd 80 (white noise)    113
probe moved from soil to air by hand          204
---- threshold ----                           400
floating pin alternating rail to rail        3910
```

A single large step contributes `S/sqrt(N)`, which is why moving a probe by hand
stays well under the line while continuous swinging does not.

**Three other tests were built and removed, because they accused working
hardware.** With two probes connected and a third pin tied to 3V3:

```
Umidade Zona 1   railed     <- healthy, simply lifted out of the soil
Umidade Zona 2   railed     <- a real fault
Umidade Zona 3   floating   <- healthy, sitting in wet soil
```

Two false accusations out of three. A capacitive module in air reads full scale
and is genuinely indistinguishable from a pin shorted to 3V3 — there is nothing
there for a statistic to separate — and the source-impedance regression crossed
its 5 % threshold on a probe that was working. **A detector that cries wolf on a
good sensor is worse than one that occasionally stays quiet**, so the coupling
regression, the rail check and the flatline check are gone.

The statistic was also changed once for the same reason: it measured the spread
of the LEVEL, and a healthy probe lifted from wet soil into the air went
287 → 4095 counts inside the window and was reported `noisy` at sd 1889. A
watering does the same thing more gently, which would have made the most
important event in the system look like a fault.

**What it deliberately does not catch:** a disconnected pin that happens to sit
quiet. This board has held one at variance 0.01, quieter than any of its
connected probes. That is the price of not accusing working sensors; such a
probe is still caught days later by the watering-response check and the
separation gate, through its failure to answer its own pump.

`/data.json` carries `fault: "noisy"` on the probe entry only when there is one,
and `/moisture.json` carries the jump in ADC counts behind it.

### The two-point fallback

The rung below the model, and the only one an uncalibrated device has.

Absolute thresholds come from **two reference readings per probe** — in air and
submerged. Each capacitive probe has its own gain and offset, which is exactly
why they cannot share a threshold.

- `config.json` carries a `moisture` array parallel to `io.soilMoisture`:
  `[{"dry": <air>, "wet": <submerged>, "relay": <index>}, ...]`.
- `moistureState(i)` (`sensors.cpp`) asks the model first, then falls back to
  thirds of that probe's own span, and returns **an empty string while
  `dry == wet`**. An uncalibrated probe with no model shows no badge rather
  than a fabricated one. `/data.json` only carries `state` when it is non-empty.
- **Ordering is not assumed anywhere, and the docs should not assert it
  either.** `include/core/config.h` says the air reading is the smaller number
  under the `100 - ADC%` conversion; an earlier version of this file said the
  larger. Neither was verified on hardware, and it cannot be settled without
  lifting a probe out of the soil. It does not need to be: both the two-point
  calibration and the classifier's ordering gate accept either direction, which
  is why the disagreement never produced a bug.
- **A floating input is not "in air".** A disconnected probe reads whatever the
  pin floats to, which is not a usable dry anchor. Do not assume it is a rail:
  the earlier "~94 here" in this file was not reproducible — measured
  2026-08-24, an unplugged probe on GPIO 32 sat at **52.6** with variance 0.01,
  squarely inside the range wet soil produces.
- `scripts/moisture_calibration.py` runs the same analysis off-device on any
  channel/field and states which question the data can answer. It refuses to
  report a drying rate over a window shorter than three days, after an early run
  extrapolated a 0.2-day window into "704 points/day".

### The fit moved to the workstation, and it fitted nothing

`scripts/moisture_fit.py` (with `scripts/moisture_stats.py` beside it) fits the
same parameters here, off the whole archive rather than off the 24 h the board
can hold, and emits them as JSON in a dry run. The device keeps its incremental
half: `moistureModelTrain()` decays the stored statistics and folds the new day
in, so a seed ages out on its own and is never a freeze.

**Run against the live device and the full archive on 2026-09-03 it proposed
NOTHING, and that is the honest answer.** Every refusal is named, per probe,
exactly as `/moisture.json` names its gates:

- **Zero watering events on either probe's own pump since the sensor change.**
  The last `Zona 1` and `Zona 3` starts were both 2026-08-31 19:46; every relay
  activation since is `Reservatorio`, which feeds no probe. The classifier
  labels from watering events, so there is nothing to bound a cycle with —
  `only 0 watering events, 6 needed`.
- **Both probes were handled on 2026-09-03 and are still equilibrating.** The
  tool finds this itself rather than being told: a sustained drift whose change
  across 30 minutes dominates the residual spread. Measured on probe 0 that
  night, `+20.74 points` over the last two hours at `+0.536/5 min`; on probe 1,
  `+15.05` at `+0.537/5 min`. So there is no settled plateau after the last
  unexplained step, and an anchor read now would be precise and wrong.

Two findings from the run are worth keeping whatever happens next:

- **`moisture2` changed meaning on 2026-09-02 13:41 and nothing in the series
  says so.** The archive keys probes and relays POSITIONALLY, so deleting
  `Umidade Zona 2` renumbered everything after it: before that minute
  `moisture2` is the pot Zona 2 waters, after it the pot Zona 3 waters. The
  archive states the relay half outright — the `relay`/`relayName` pair shows
  the reservoir pump moving from index 3 to index 2 — and states the probe half
  not at all, because the moisture keys carry no name. **The tool therefore
  treats any seam as a hard boundary for every probe**, which is blunt and
  deliberate: `--since` can narrow that window and nothing on the command line
  can widen it.
- **This garden is also watered by hand, so the relay record is not a complete
  label source.** Between 12:14 and 12:25 on 2026-09-03 both probes
  rose together — probe 0 from 72.8 to 86.5, probe 1 from 75.0 to 83.9 — with
  no relay event anywhere near it, and the reservoir pump was the only thing
  that ran that day. Every such rise is an unlabelled wetting, and the
  classifier scores it "humid".

**`/moisture_model.bin` is deliberately NOT written from here.** Five reasons
are in the block comment above `MODEL_FILE_DECLINED`; two of them are
structural and survive the data improving. The archive's `moistureN` is the
accumulator MEAN over one publish period — 300 s since 2.8.0 — while the device
fits from its own 60 s history records, and `J = Δμ² / (σ²_wet + σ²_dry)` is a
ratio to a variance that averaging shrinks, so a J fitted here would pass the
device's gate for the wrong reason. And `consumedUntil` — the epoch past which
the device skips its own history — has no correct value from another data
source: zero double-counts the day the board already holds, a current epoch
throws it away. **The fix for both is to fit from `GET /history.json`**, which
is the device's own 60 s record, and that is the first thing to change when
there is finally something to seed.

---

## Cloud cover: an empirical clear-sky reference, and what the data could not settle

**Nothing here has run on hardware.** The model is fitted, host-tested, builds
in all five envs and is **off by default** (`cloud.enabled`). Everything below
is a measurement on the ARCHIVE, or arithmetic from one — not on the board.

Two sources, in this order, exactly as the moisture badge ladders:

1. **The clearness index** `k = measured / clearSkyReference(time of day)`,
   turned into `clear` / `partly cloudy` / `overcast`.
2. **Nothing.** Outside the fitted daylight window, and whenever
   `cloud.enabled` is false, `/data.json` omits `state` entirely rather than
   showing a badge it cannot support.

### The archive contains TWO sensor geometries, and that is the biggest finding

Between **2026-08-27 and 2026-08-28** the luminosity channel stepped. First
light moved from 06:16-06:19 to 06:36-06:51, and every hour's maximum fell by a
factor that depends only on the time of day:

```
hour   08-24  08-25  08-26  08-27 | 08-28  08-29  08-30  09-01  09-02  09-03
   7    2.53   2.38   2.82   2.20 |  0.54   1.03   0.90   0.73   0.74   0.88
  10    1.34   1.32   1.20   1.36 |  0.39   1.00   0.60   0.61   0.84   0.66
  13    1.42   1.41   1.31   1.36 |  0.92   1.01   0.95   0.91   0.99   0.97
  16    1.07   1.17   1.15   1.08 |  0.96   0.95   0.93   0.95   0.92   1.01
        (that hour's maximum divided by the fitted envelope's)
```

**That is not weather.** The same time-of-day-locked attenuation repeats across
six days whose conditions had nothing in common — 31 % RH and 70 % RH, clear
afternoons and cloudy ones — and the dim mornings are SMOOTH: minute-to-minute
|delta| p90 of 0.25-0.99 points, against 0.98-2.54 on the bright ones. Fog at
31 % RH does not exist, and a smooth ramp to a low ceiling is a shadow, not a
cloud. Something began shading the sensor's morning, or it was moved, around
the maintenance window that also produced eleven watchdog reboots between 01:03
and 02:16 on 08-28.

**What this costs:** the usable archive is **six daylight days**, not ten, and
the morning half of the envelope rests on the two clearest of them. Every number
below carries that. `scripts/cloud_fit.py` prints the table above on every run
for exactly this reason — it is how the NEXT geometry change becomes visible,
and nothing on the device can detect one.

### Why the reference is empirical and not astronomical

The repo owner chose this, and the data agrees. This sensor peaks at about
**15:00 local, three hours after solar noon**, and its 07:00 reference is 20 %
of its 15:00 one. A solar-position model would attribute all of that to cloud,
every morning, for ever — geometry wearing a weather label, which is the same
failure as a classifier that reads the pump schedule.

The reference is the **98th percentile of every reading in each 10-minute bin**,
smoothed once with a (1, 2, 1) kernel and **interpolated between bin centres**.

- **144 bins of 10 minutes.** Measured against 5 / 15 / 20 / 30-minute bins,
  this gave the flattest per-bin median clearness index (sd 0.106), and it is
  288 bytes.
- **A moving maximum was tried first and withdrawn.** It drags the brightest
  sample in a bin up to a bin earlier, and on the morning ramp — which climbs
  1.6 points a minute at its steepest — that shifts the whole reference. With
  it, the last bin of the day had a median k of 0.31; without it, 0.48.
- **Interpolation is not decoration.** At that ramp rate, treating a 10-minute
  bin as a constant biases every reading in it by up to eight points, which is
  larger than the gap between two of the three states.
- **The daylight window is the run of bins around the peak whose reference is
  at least a quarter of it** — fitted at **07:40-17:39 local**. Below that the
  ratio stops meaning anything, and this garden's artificial light reaches 13-14
  points on its own between 19:30 and 21:30, which a plain value floor would
  have let straight in.

Fitted on 2026-08-28..09-03: peak **95.88 %** at 15:20, k p5/p50/p95 =
**0.389 / 0.733 / 0.995**, and **3.97 %** of readings sit above the reference —
which is the point. It is an envelope, not a ceiling.

### The thresholds are on the SENSOR'S scale, not the solar literature's

`k` here is not an irradiance ratio. An LDR read as a fraction of full scale
compresses irradiance, so the overcast day in this archive sits at k around
0.52 where a true irradiance ratio would put it near 0.2. Borrowing the solar
literature's K_t bands would have called every ordinary day overcast.

| | Threshold | What it came from |
|---|---|---|
| **clear** | `k >= 0.85` | Four settled fine days (2026-08-24..27, the OTHER geometry, fitted with their own envelope) sit at k p25..p95 = **0.83..1.00** |
| **overcast** | `k < 0.50` | The one overcast day spent **41.5 %** of its daylight below 0.50; the three brightest post-break days spent **0.3-3.9 %**, and the four fine days 1.5-8.0 % |
| hysteresis | +-0.05 with a 5-minute dwell | Cuts state changes from 14/day to 2-10/day. Every flap is a published datapoint |
| smoothing | EWMA alpha = 0.30 on k | |

**The two internal boundaries are the weak part and it should be said plainly.**
They are fitted to six days, one of which supplies the entire overcast evidence.
What would move them is more days — specifically a genuinely overcast one and a
genuinely clear one under the CURRENT geometry.

### The transient is an event, and it had to be

A cloud edge lasts seconds to minutes; the periodic payload is built every
300 s. [Sampling vs events](#sampling-vs-events--the-rule-and-where-it-was-broken)
says that must be an event or a sticky flag and never a sample — so it is an
**episode**: one message when it opens, one when it closes, carrying the length
nothing else could reconstruct. Publishing per-minute would have been ~500
datapoints a day on a budget this repo had just cut by 83 %.

- The statistic is an **EWMA (alpha = 0.30) of |dk| between consecutive
  minutes**: enter above **0.070**, exit below **0.035** held for **3 minutes**.
- **|dk| and not |d(reading)|.** Dividing by the reference is what removes the
  diurnal ramp. Measured: over 5-minute windows with a range of 5 points or
  more, a monotonic ramp and a flickering sky are **indistinguishable by the
  spread of the level** — both at sd 3.2 — while the step statistic separates
  them 1.10 against 3.61. `probe_health.h` records making exactly this mistake
  once, on exactly this kind of signal.
- **It goes quiet in settled weather, which is the property that matters.**
  Replayed against the four fine pre-break days with their own envelope:
  **0, 1, 1, 0 episodes a day.** Against the six fitted days: **3, 4, 5, 6, 7,
  7** — about 14 events a day at the worst, 0.3 % of the daily budget.

### The 300 s publish period, measured

The question was what the periodic payload's window MEAN costs now that
`mqtt.publishSec` is 300, and whether publishing the within-window spread buys
it back. Resampling the 60 s archive into aligned 300 s windows inside the
fitted daylight window (619 windows), against a ground truth of "the clearness
index moved by 0.10 or more inside this window" (18.6 % of them), each detector
thresholded to give 1 % false positives:

| What is published | Recall | Correlation with the within-window k-range |
|---|---|---|
| the difference between consecutive 300 s means — **what ships today** | **5.1 %** | 0.455 |
| the within-window spread of the LEVEL | 86.1 % | 0.958 |
| the within-window mean \|dk\| — **what was added** | 80.0 % | 0.958 |

**The mean alone loses nineteen transients in twenty.** So `cloudVariability`
rides the periodic tick: it is a level, and a level is what the third mechanism
in the sampling table is for. It costs **+288 datapoints/day** on a ~4 320/day
base, **+6.7 %**.

The level spread scores marginally higher here only because this ground truth is
itself a within-window quantity; across times of day it cannot tell a ramp from
a flicker at all, and a stored series whose meaning depends on the hour is worth
less than six points of recall.

**Two limits on this measurement, both structural.** The 60 s archive is exactly
the mean of each minute's 1 Hz samples — which is why the device's own minute
bucket reproduces the fitted quantity with nothing assumed — but the device sees
300 samples per publish window where this test saw 5, so the recovered recall is
a **lower bound**. The same fact cuts the other way: now that the device
publishes at 300 s, **the transient thresholds can no longer be re-fitted from
the archive** without temporarily dropping `mqtt.publishSec` back to 60.

### What it costs on the chip

Measured from `espgarden2`'s ELF symbol table, not estimated:

| | Bytes |
|---|---|
| `g_clearSkyTable` (144 x `uint16_t`, hundredths of a percent) | **288** flash |
| `g_cloudParams` | 48 flash |
| the pure classifier's code (8 functions) | 873 flash |
| the Arduino half's code (4 functions) | 1 032 flash |
| `CloudModel` + the snapshot + the minute bucket, resident | **100 RAM** |
| **whole change, every file:** 1 242 585 -> 1 246 489 | **+3 904 flash, +136 static RAM** |

**No task was added.** It rides the 1 Hz io task, and 59 ticks in 60 do nothing
but add to a running sum — which matters, because `addTask()` silently drops
past `CRITICALTASKSCHEDULER_MAX_TASKS`.

### What the data could not settle

- **Whether the 2026-08-28 break is a moved sensor or a new obstruction.** It is
  certainly geometry; which one needs somebody standing at the garden.
- **The overcast threshold**, resting on one overcast day.
- **A genuinely clear day under the CURRENT geometry.** The best fitted day has
  a daylight median k of 0.913, so the envelope's morning is anchored on two
  days, not six.
- **Anything seasonal.** Ten days at the end of one winter, of which six are
  usable. Day length and solar elevation move underneath this table and nothing
  re-fits it automatically.
- **The last bin of the day.** 17:30-17:39 has a fitted median k of 0.48 against
  0.73 across the window, so the model leans toward `overcast` in the final ten
  minutes. Four of the six fitted days genuinely had cloudy late afternoons, and
  six days cannot separate that from a reference set too high.

## Evapotranspiration: the fit said no, and that is the result

**Nothing here has run on hardware, and unlike the cloud model it is not merely
unproven — it was measured and it failed.** `et0.enabled` defaults to false
because `scripts/et0_fit.py`, run against this device's own archive and a public
station, could not show the estimate beating a constant. The model ships anyway,
correct and host-tested, because the failure is in this board's SENSOR SITING
and not in the arithmetic: fix the exposure, or put the firmware on a board in
free air, and the same code becomes useful. What must not happen is the number
being believed today.

Everything below is a measurement on the ARCHIVE against Open-Meteo, or
arithmetic from one — not on the board.

### Where this garden is, and how that was settled

`postalCode` is `70675-506`. ViaCEP resolves it to *Quadra QRSW 5 Bloco A-6,
Setor Sudoeste, Brasília - DF*; OSM Nominatim holds that street as way
**125381662, "QRSW 5"**, at **-15.7885, -47.9297**. Nominatim's `postalcode`
search returns nothing for Brazilian CEPs, and the verbatim `logradouro` misses
too — a Brasília address is "Quadra QRSW 5 Bloco A-6" where OSM has the plain
road "QRSW 5", so `et0_fit.py` strips the `Quadra `/` Bloco ` affixes before
querying. Without that strip it falls back to the neighbourhood centroid, 1.3 km
away; without the neighbourhood it would fall back to the city, 12 km away.
**A wrong geocode is a silent error** — nothing downstream looks wrong when the
latitude is off, the numbers are simply for somewhere else.

**The public record is a model analysis, not a thermometer in this garden.**
Open-Meteo snaps to a grid point at -15.7821, -47.9717, elevation 1151 m —
**4.6 km away**. And it is revised: two fetches an hour apart reported 2026-08-24
as 1.2 mm and then 0.9 mm, moving the eleven-day total 2.4 → 2.1 mm and the
count of days reaching 1 mm from one to zero. Anything fitted tightly to these
numbers is fitted to a revision.

### Rain: there is none, so nothing here models it

Eleven days at the grid point, in Brasília's peak dry season:

```
08-24  1.20 mm / 3 h      08-29  0.00      09-02  0.70 mm / 4 h
08-25  0.00               08-30  0.00      09-03  0.30 mm / 2 h
08-26  0.10 mm / 1 h      08-31  0.00
08-27  0.10 mm / 1 h      09-01  0.00      total 2.4 mm, ONE day at 1 mm
```

**No rain model was fitted and none should be.** One marginal wet day is not a
positive class, and a rain classifier trained on it is the same defect that
blocks the moisture classifier here — a model with nothing to learn from that
returns confident answers anyway. `et0_fit.py` prints this table first, before
anything else, and refuses in its own output.

### The finding that decided everything: the thermometer is not in free air

Matched hour by hour against the station over 235 hours:

```
hour   station T   device T      dT      station Td  device Td     dTd
   3      20.76      26.44    +5.68        12.52       16.88     +4.36
   6      19.92      25.59    +5.67        12.57       17.11     +4.54
  10      26.73      26.91    +0.18        12.01       16.51     +4.50
  13      30.22      28.53    -1.69        10.42       14.24     +3.82
  15      30.74      30.66    -0.08         9.46       12.98     +3.53
  20      26.66      28.72    +2.06        10.04       14.72     +4.68
  23      23.69      28.02    +4.33        11.67       15.58     +3.92

device T  = 0.346 * station T  + 18.98   r = +0.722
device Td = 1.052 * station Td +  3.61   r = +0.909
```

**The device follows 35 % of the outdoor temperature swing while its DEWPOINT
follows 105 % of the outdoor dewpoint.** That pair is the whole diagnosis.
Dewpoint is conserved when air is merely heated or cooled, so a sensor tracking
it at slope ~1 is breathing the same air mass as the station; a sensor tracking
the TEMPERATURE at slope 0.35 is sitting behind thermal mass. Warm by +5.7 K
before dawn, cool by -1.7 K at midday: that is an enclosure, a roof or an indoor
spot, not a screened instrument.

**This is fatal to Hargreaves-Samani specifically, and it is worth being precise
about why.** HS has exactly one mechanism: `sqrt(Tmax - Tmin)` stands in for
solar radiation, because a clear day heats hard and radiates away at night. This
board's diurnal range is **5.7 K mean against the station's 11.0 K**, and — the
number that settles it — the device's range correlates with the real range at
**r = +0.147 over 8 days**. The proxy is not merely biased, it is close to
uninformative. A model whose only mechanism has been disconnected is not
repaired by scaling its output.

*(The dewpoint offset is +3.6 K, i.e. the device also reads genuinely moister
than the grid point. Some of that is a DHT11's ±5 % RH, some is plausibly a
watered garden's own microclimate. It is not diagnosed here and nothing depends
on it.)*

### The validation numbers

Complete days only — a day needs 22 of 24 local hours, which refuses 3 of the 11
(08-24 at 20 h, 08-27 at 21 h, and 08-31 at 6 h, the board-swap day):

| predictor | n | mean vs PM | bias | MAE | RMSE | r |
|---|---|---|---|---|---|---|
| **HS(station T)** vs station PM | 8 | 4.46 vs 4.99 | -0.53 (**-10.6 %**) | 0.83 | 1.10 | +0.774 |
| **HS(device T)** vs station PM | 8 | 3.88 vs 4.99 | -1.11 (**-22.3 %**) | 1.86 | 2.05 | **+0.050** |
| HS(device T) vs HS(station T) | 8 | 3.88 vs 4.46 | -0.58 (-13.0 %) | 1.32 | 1.47 | +0.051 |

The first row is the ceiling and it is the reassuring one: **with a correct
thermometer, this implementation of Hargreaves-Samani sits 10.6 % under FAO-56
Penman-Monteith**, which is ordinary HS behaviour in a windy semi-arid dry
season. The maths is right. The second row is the device, at **r = +0.050** — no
relationship at all.

**A fitted scale factor was tried and refused by cross-validation**, which is the
check that matters on eight days:

```
scale removing the mean bias        x1.286   (LOO spread x1.208 .. x1.492)
LOO RMSE, scaled HS(device)          2.56 mm/d
LOO RMSE, constant climatology       1.31 mm/d   <- the null model wins
station PM over those days           mean 4.99, sd 1.14 mm/d
```

So `et0.scale` ships at **1.0** and `et0.enabled` at **false**. The key exists
because a correctly sited device deserves the lever; the default is the
measurement.

### One 38-minute sun patch moved a day by 120 %, and that is the estimator

2026-09-03 is in the table above at **device Tmax 45.04 °C against the station's
29.6** — HS 7.35 mm against PM 4.57. Drop that one day and the same estimate
scores r = **+0.911** instead of +0.050. Eight days is small enough that one day
owns the answer, and this is the day.

**It is not a bad sample and it was not filtered out.** The archive says what
happened, and it resolves what this file recorded as unexplained:

```
time     T      RH     Td     luminosity
14:40   28.41  50.28  17.07     39.11
15:14   37.44  30.15  15.82     96.42   <- saturated
15:30   44.90  16.00  13.39     95.41
15:51   35.18  33.27  16.47     65.68
16:19   32.60  37.40  16.65     72.98
```

The luminosity channel hits **96 for 27 minutes**, a level it reaches on no other
post-2026-08-28 day (the rest ceiling at 72-76), temperature rises and falls with
it over 38 minutes, humidity mirrors it, and **both soil probes are flat to 0.8
points throughout** — so nobody was handling the hardware. A patch of direct sun
reached the sensor package at one solar azimuth. It was the only time in the
whole archive the device exceeded 35 °C.

The lesson is about the estimator, not the sample. **A daily maximum has no
averaging in it**: 38 minutes out of 1440 more than doubled the day's ET0, where
the same excursion moves a daily mean by 2 %. `test_evapotranspiration` pins this
down with `test_a_single_short_excursion_owns_the_whole_day`, so anything that
later tries to "clean" the extremes has to argue with a red test — and this repo
does not clamp real readings away, which is exactly why the DHT humidity floor
was lowered to 5 % a commit earlier.

### A correction to this file's own humidity claim

The DHT-gate entry above says the 261 sub-20 % humidity points are real
dry-season readings. **243 of them are; 18 are not.**

- **2026-09-01: 243 points, 13:03-17:40, device 31.7-34.2 °C.** Ambient. The
  station's daily mean RH that day is 31 % and its ET0 the window's highest at
  7.21 mm. The minimum of 15.00 % is here, so the decision to lower the floor to
  5 % rests on these and stands.
- **2026-09-03: 18 points, 15:20-15:38, device 41.3-45.0 °C.** Inside the sun
  patch. The dewpoint drops 17.1 → 11.5 °C and recovers, which pure heating of a
  fixed air parcel cannot do — the humidity element was being cooked, and those
  readings measure nothing.

The conclusion does not move; the attribution does.

### What it costs on the chip

Measured from `espgarden2`'s build, not estimated:

| | Bytes |
|---|---|
| `evapotranspiration.cpp.o` — the pure maths | **1 609** flash |
| `et0_model.cpp.o` — clock, rollover, snapshot | 1 756 flash, 8 data, 64 bss |
| `et0ExtraterrestrialRadiation` alone | 552 flash |
| `et0Hargreaves` alone | 336 flash |
| `Et0Day` + `Et0Report` + the mux, resident | **72 RAM** |
| **whole change, every file:** 1 247 157 → 1 254 501 | **+7 344 flash, +72 static RAM** |

70.5 % → **70.9 %** of the 1.69 MB app slot. The gap between the 3 365 bytes of
the two new translation units and the 7 344 total is the `JSONVar` event, the
`/data.json` row, the config parsing and the `String` formatting in the setup
log — the plumbing costs more than the physics, as it did for the cloud model.

**No task was added.** It rides the 1 Hz io task and does one float comparison
per tick, 86 399 ticks in 86 400 doing nothing but a min/max — which matters,
because `addTask()` silently drops past `CRITICALTASKSCHEDULER_MAX_TASKS`.

**And it is published once a day, as an EVENT.** ET0 updates once in 24 h, so
riding the periodic payload would restate it 287 times a day and riding the step
publisher would re-send it on every 900 s heartbeat for the same nothing. One
message, carrying `et0` with `et0TempMin`, `et0TempMax` and `et0Hours` beside it
— the evidence stored with the answer, for the reason the `Sd` error bars exist.

### What the data could not settle

- **Whether the sensor is indoors, in a box, or under a roof.** The statistics
  say "behind thermal mass" and cannot say which. Somebody has to look.
- **Whether a correctly sited DHT11 on this board would work.** The station-fed
  row (-10.6 %, r 0.774) says the formula and the location are fine; nothing says
  what a ±2 °C sensor in a proper screen would score, because there has never
  been one here.
- **Anything seasonal.** Eight usable days at the end of one dry season. HS's
  premise is that the diurnal range tracks cloudiness, and this window has almost
  no cloud variation to track — the STATION's own range correlates with its own
  ET0 at only r = +0.392 here. The formula may well look better in a wet season,
  and nothing in the archive can say.
- **The 22-hour coverage gate's number.** Physics says both turning points must
  be inside the day; it does not say 22. The gate demonstrably refuses the 6-hour
  board-swap day, whose 3.43 K range was pure artefact, and admits everything
  else. That is the only evidence for it.
- **Whether the +3.6 K dewpoint offset is the garden or the sensor.**
- **A second board.** Every number here describes one DHT11 at one mounting, and
  the archive already contains one undiagnosed geometry change (the luminosity
  step of 2026-08-28). Nothing on the device can detect the next one.

## Web assets — bundled and gzipped, because requests are the scarce resource

`devices.html` stopped loading, and the browser said
`ERR_CONTENT_LENGTH_MISMATCH` on `devices_render.js`. It was not a corrupt file:
**served one at a time, every asset came back byte-identical.** Under the six
parallel requests a browser makes for one page, the largest contiguous free heap
block collapsed from ~45 KB to as little as 1 KB and ESPAsyncWebServer truncated
whichever response it was filling — so the victim changed on every reload, which
is what a resource ceiling looks like from outside.

LittleFS is what pushed it over: every open file carries a 4 KB cache where
SPIFFS used 256 B pages. CLAUDE.md's own 2.2.1 load test — six endpoints in
parallel, eight rounds, zero failures, largest block never below 49 KB — was
measured on SPIFFS and does not carry over.

**Gzip alone was not enough.** Compressing the five assets the page loads cut
them ~70 % and took it from failing every load to failing one in three, because
the pressure scales with the NUMBER of open files, not their size. What fixed it
was going from 7 requests to 4: **10/10 clean page loads, and the largest free
block stayed at 53 KB instead of collapsing.**

`scripts/build_assets.py` produces `.pio/assets/` from `data/`:

- **The scripts a page loads are read from its HTML, in order**, and
  concatenated into the LAST one's name. Deriving the order from the markup
  rather than from a manifest is what stops the two drifting apart, and reusing
  the last name means **the route table does not change** — `/devices.js` was
  already a route and now carries `auth + model + render + devices`.
- **A script that exists only as `<name>.gz` in `data/` is vendored** (jQuery,
  sha256, SparkMD5, Bootstrap) and stays a separate file: it is shared across
  nine pages, and bundling one into each would cost 30 KB of flash per copy.
- **A script shared by several pages is also emitted standalone.** `auth.js` is
  in all nine bundles, so a browser holding cached markup still asks for it by
  name — and a 404 there takes out the login page. One kilobyte closes that
  window.
- **`.json`, `.pem`, `.txt` and `.bin` are copied verbatim, never compressed.**
  The firmware opens those itself through `FILESYSTEM.open()`, which has no
  gzip fallback. Compressing `/config.json` or `/thingspeak.pem` bricks the
  device at the next boot.
- Measured: 250 KB of sources become **143 KB** on flash, and every page drops
  from 6–7 requests to 4.

`data/` stays plain: diffable in git, counted by `check_lines.py`, and served
as-is by `scripts/dev_server.py`, so the simulator needs no build step and shows
the unbundled files a developer is actually editing.

**The upload guard had to become exact.** `uploadPathIsProtected()` was
`startsWith("/config")` and friends, which also refused `/config.html`,
`/config.js`, `/users.html` and `/users.js` — ordinary web assets. It surfaced
the moment assets started being deployed one at a time instead of by rewriting
the partition: four files answered 400 for no reason but their name, and the
only remaining way to update them was the whole-image path that wipes
`/config.json`. A guard that pushes you toward the more destructive tool is
worse than the one it replaced. The `/spiffs` BROWSE shadows stay prefix matches
deliberately — refusing to read `/spiffs/config.html.gz` costs nothing, since
the same bytes are served from its own public route.

## Filesystem — LittleFS, and why the name appears once

`include/core/filesystem.h` is the only file that says `LittleFS`. Every other
source says `FILESYSTEM`, so changing driver again is one line instead of the
sixty substitutions across nineteen files it took to leave SPIFFS.

- **`board_build.filesystem = littlefs` in `platformio.ini` is the matching
  half.** Without it `-t buildfs` still runs mkspiffs, the image is a SPIFFS
  one, and the device formats it away on first mount — silently, because
  `begin(true)` formats on failure and the boot then looks normal apart from
  every file being gone. The build artefact is now `littlefs.bin`; a build
  still producing `spiffs.bin` means the flag did not apply.
- **The migration cannot be done over the air, in either order.** Firmware
  first mounts a SPIFFS partition as LittleFS, fails, formats, and takes
  `/config.json` — and with it the WiFi credentials — on a device that is now
  unreachable. Filesystem first is erased by the old firmware's own format at
  the next boot. It is a USB job with a verified `?secrets=1` backup in hand,
  which is how this one was done.
- **It costs space rather than saving it.** Measured on 9e7c: SPIFFS 333 of 463
  usable KB (72 %), LittleFS 388 of 512 KB (76 %) — 4 KB blocks against 256 B
  pages, so thirty small web assets pay ~55 KB of internal slack and free space
  went 130 → 124 KB. What was bought is the removal of SPIFFS's degradation
  cliff past ~75 % full, which this partition was about to reach, and
  copy-on-write metadata on a board whose supply is marginal enough to print
  `flash read err, 1000` every boot.
- **The URL prefix is still `/spiffs/`** and stays that way. It is a contract
  with `data/*.js` and with anything anyone has scripted against the device;
  renaming a route to match an internal driver change is a breaking change
  bought for nothing.

### The ring buffer does not survive the move, and the reason generalises

**`IoHistory::append()` panicked the board every 60 s under LittleFS**, from the
first history write after every boot, deterministically. Verified 2026-08-24 on
9e7c, three consecutive panics 73-76 s apart, all with the same PC and a clean
backtrace:

```
Guru Meditation Error: Core 1 panic'ed (IntegerDivideByZero)
lfs_alloc              lfs.c:689
lfs_ctz_extend         lfs.c:2892  (inlined by lfs_file_flushedwrite:3557)
lfs_file_flush / _sync_ / _close_ / lfs_file_close / vfs_littlefs_close
IoHistory::append      src/io_history.cpp:178   <- the close()
historyTaskHandler     src/tasks.cpp:489
```

What is NOT wrong, each measured on the device rather than assumed:

- Block allocation. A 1256-byte `/config.json` save allocated and committed.
- Sequential writes at any size. 4 KB and 69 136 B uploaded through
  `/spiffs/upload`, read back **byte-identical**, deleted, space reclaimed, with
  the board never missing a beat.
- The image. The superblock built by `tool-mklittlefs` reads `block_size 4096`,
  `block_count 128`, version 2.0 — correct for a 512 KB partition.

What IS wrong is the access pattern, and it is a design mismatch rather than a
bug to patch. **LittleFS stores file data as a copy-on-write CTZ skip-list,
which is built for appending.** Rewriting bytes in the MIDDLE of a file makes it
copy the chain forward from that point. `append()` seeks to
`16 + head * 48` and writes 48 bytes, then seeks to 0 and rewrites the header —
so with `head` near the start it rewrites **the whole 69 KB file**, every minute,
forever.

That is fatal twice over. It crashed here, and even if it had not: 69 KB of
rewrite per 48-byte record, once a minute, is ~100 MB of flash writes a day on
blocks rated for ~100 000 erases. The ring would have destroyed the partition in
weeks while looking like it worked.

**The design was correct for SPIFFS and is wrong for LittleFS.** SPIFFS rewrites
one 256-byte page for an in-place overwrite, which is what "a fixed-size ring
with in-place appends" was built on. The lesson is not about littlefs: any
copy-on-write filesystem turns random writes into whole-file rewrites, so
*in-place mutation of a large file is the thing to design out*, not a knob to
tune.

The fix is append-only segments — records written to the END of a file, which
touches only the last block, rotated across a few files with the oldest deleted.
Fixed flash usage is preserved and the wear is far below even the SPIFFS design.

`history.records` was set to 0 through `POST /config.json` on 2026-08-24 to
stop the panic loop without a reflash — that is what a panicking garden
controller costs, because every reset floats the GPIOs and on these active-low
boards pulses every pump for the length of a boot. **It went back to 1440 the
same day**, once the append-only rewrite was flashed; see the verified entry
above.

## I/O history — append-only segments

`/hist0.bin` .. `/hist7.bin` hold the last N snapshots of every input and
output. Records are appended to the END of the newest segment; when it fills,
the oldest segment is truncated, stamped with a new sequence number and written
into. **Nothing is ever rewritten in place — not a record, not a header** — and
that is the whole design, because in-place mutation of a large file is what
[panicked the board under LittleFS](#the-ring-buffer-does-not-survive-the-move-and-the-reason-generalises).

Flash usage stays bounded at `8 * (12 + recordsPerSegment * 48)` bytes forever,
which is what the ring bought, without the writes that made it fatal.

- **The segment header carries NO record count.** The count is the file length.
  Storing one would mean rewriting the header on every append — reintroducing
  exactly the mid-file write this design removes — and would turn a torn append
  into a disagreement between two places instead of a short tail that does not
  divide evenly and is simply ignored.
- **Retention became granular, and that is the price.** A whole segment is
  dropped at once, so the number of records held swings between `7/8` and `8/8`
  of capacity rather than sitting exactly at it. At the default 1440 that is
  1260–1440 records.
- **Both parameters are still config-driven**: `history.records` (0 disables the
  feature) and `history.periodSec`. `history.records` is the TOTAL; it is
  divided across the eight segments, rounded up, so the history is never
  shorter than what was asked for.
- **Nothing is preallocated any more.** The ring wrote its whole file at
  `begin()` so an append could never fail for space; segments take space as
  they use it and release a whole segment at a time, and after the first full
  cycle no new space is ever needed.
- **The record layout is fixed regardless of build flags.** A board with one
  probe still writes four moisture slots, filled with NaN, which serializes as
  `null`. Making the layout depend on the fitted probe count would mean two
  firmwares disagree about how to read the same file with no way to tell which
  wrote it. Each segment header carries `recordSize`, and a segment that
  disagrees is deleted rather than reinterpreted.
- **`history.records` is capped in RECORDS, so the cap comes down whenever the
  record grows** — 6000 → 5000 at 48 bytes, then 5000 → **2500** under LittleFS,
  because 240 KB no longer fits in the 189 KB free before any history exists.
- **`/history.json` caps a response at 200 records** (`g_historyMaxResponse`).
  1440 records is 57 KB of raw struct and roughly 170 KB rendered as JSON, more
  than half this chip's DRAM with WiFi already holding a share. The handler also
  keeps a `static IoRecord buffer[200]` — 9.6 KB of DRAM, visible in the build.
- **`forEach()` walks in ABSOLUTE coordinates**, not logical ones. It releases
  the mutex every 64 records so a daily training run does not block the
  `async_tcp` task for seconds, and a rotation landing in that gap shifts every
  logical index by a whole segment — 180 records, where the ring shifted by one.
  The walk therefore tracks the ordinal since the first append ever and
  subtracts `evicted()`, so a rotation costs exactly the records it discarded.
- **The index arithmetic lives in `include/core/segment_index.h`**, deliberately
  free of Arduino and LittleFS so `test_segment_index` can reach it. A wrong
  answer there reorders history instead of failing, which is why it is the one
  part with unit tests — including that `counts` is indexed by SLOT while
  `order` lists slots, a confusion that gives right answers until the first
  rotation and wrong ones forever after.

### The boot-loop interlock

The history writer has already reboot-looped this board once, and recovering
needed `POST /config.json` — which needs the device to stay up long enough to
answer. A config key cannot guard against that, because a config change only
takes effect at the NEXT boot: turning history back on and being wrong leaves a
device that panics before it can be told otherwise, and with no USB attached
that is unrecoverable.

So the strike count lives in **RTC memory** (`RTC_NOINIT_ATTR`), which survives
a panic reset and not a power cut. Three consecutive boots ending in a panic or
a watchdog and `setup()` refuses to start the writer, logs why, and lights the
error blink — leaving `history.records` untouched, so a clean power cycle is a
fresh attempt. `historyTaskHandler()` clears the count once it has been running
ten minutes, which a writer that crashes on its first append never reaches.

---

## Runtime hardware — the build no longer knows what is fitted

`HAS_MOISTURE_SENSOR` and friends are **gone**, along with `RELAY_COUNT` and
`MOISTURE_SENSOR_COUNT`. Every driver compiles into every image; what a board
actually has comes from `config.json` and is counted at load:

| Was | Is | Decided by |
|---|---|---|
| `-D RELAY_COUNT=4` | `config.relayCount`, capped by `RELAY_MAX` (8) | length of `io.relays` |
| `-D MOISTURE_SENSOR_COUNT=3` | `config.moistureCount`, capped by `MOISTURE_MAX` (4) | length of `io.soilMoisture` |
| `-D HAS_DHT_SENSOR` | `config.dhtFitted` | `io.dht` key exists |
| `-D HAS_LUMINOSITY_SENSOR` | `config.luminosityFitted` | `io.luminosity` key exists |
| `-D HAS_WATER_LEVEL_SENSOR` | `config.waterLevelFitted` | `io.waterLevel` key exists |
| `-D HAS_FLOW_SENSOR` | `config.flowFitted` | `io.flow` key exists |
| `-D HAS_FLOAT_SWITCH` | `config.floatFitted` | `io.floatSwitch` key exists |
| `-D MOISTURE2_FIELD=n` | `config.thingSpeakMoisture2Field` | `thingSpeak.moisture2Field` |

**Presence IS the key.** There is no separate `enabled` flag, because one would
drift out of step with the pin it names. Deleting a sensor in `/devices.html`
means removing its key.

Measured before it was chosen: the minimal board was 1.166 MB and the fully
populated one 1.187 MB, so the whole `HAS_*` split was buying **21 KB out of a
1.69 MB slot**. What it cost was 75 `#ifdef` sites, five build shapes to keep in
step, and a sensor you could not add without a toolchain.

**What is still compile-time, and must stay so:** the set of KINDS. A DHT needs
the DHT driver linked in; no web page adds a kind there is no code for.
`GET /capabilities.json` publishes the kinds, the maxima and the usable pins so
the UI never restates a rule the firmware owns.

Traps this created, every one of which was a real defect first:

- **`relayPinsSafeInit()` runs over `RELAY_MAX`, not `relayCount`** — at its
  first call, before the config is read, the count is not known and a floating
  pin on an active-low board reads as energise. So slots past the count must
  have **no pin**: `clearUndeclaredRelayPins()` sets them to `kNoPin` after the
  load, or the second call force-drives the compiled defaults over whatever the
  config gave those GPIOs to.
- **`kNoPin` has to be honoured everywhere, not just at init.** A relay row
  saved without a pin is startable otherwise: the index check passes,
  `startRelay()` reports success, the dashboard counts down, and
  `digitalWrite(255, …)` does nothing.
- **Dropping an `#ifdef` is not the same as deleting it.** Every guard removed
  needs a runtime test put back in its place. Missing ones made `/data.json`
  report a confident `0.00` for sensors that were not there, and made the
  history record write `FLOAT_VALID` with "lowered" on a board with no float —
  the exact case that flag exists to distinguish.
- **A count constant is not a presence test.** `MOISTURE_MAX == 1` is always
  false, so the single-probe `"Soil Moisture"` label — the `/data.json` key
  dashboards have read for years — silently became `"Soil Moisture 1"`. The
  test belongs where `moistureCount` is known.
- **Bounds that were compile-time are now per-device.** A schedule aimed at
  relay 3 on a two-relay board has to be rejected at load, not fire daily into
  `startRelay()`'s index check.
- **`config.floatInterlock` cannot outlive `io.floatSwitch`.** `loadFile()`
  clears it when no switch is declared; otherwise removing the sensor leaves a
  veto that refuses every watering on a reading nothing produces.

Pin rules live in exactly one place — `pinIsADC1`, `pinIsInputOnly`,
`pinIsFlash`, `pinIsStrapping` in `config.cpp`. `validatePins()` uses them at
boot, `documentPinsAreUsable()` refuses a bad map at **save** time (boot is too
late: the document is already on flash), and `/capabilities.json` derives the
UI's pin lists by walking every GPIO through the same predicates.

## Sampling vs events — the rule, and where it was broken

**Anything whose duration is shorter than the publish period must be recorded as
an EVENT or as a sticky flag. Never sampled.** A watering lasts five seconds and
the payload is built once a minute, so an instantaneous read of `relayIsOn()`
misses roughly eleven activations in twelve. The history record learned this
when `g_relaySticky` was added; the telemetry kept sampling for months longer.

Three mechanisms, and which to use:

| | Use for | Example |
|---|---|---|
| **Event** | A transition an operator needs timestamped | relay started/stopped/refused, reservoir emptied, reboot, `fw_state`, a cloud transient, the daily `et0` |
| **Sticky flag** | "Did this happen at all during the period" | `relayNRan`, `relayRanMask`, `IO_HISTORY_FLAG_FLOAT_RAISED` |
| **Accumulator** | A quantity, where the mean or total is the answer | moisture, luminosity, flow rate, `flowTotalLitres`, ping |

Events reach the broker through `tbPublishEvent()`, which appends to the same
outbox `tbLoop()` drains every `loop()` iteration — so latency is milliseconds,
not a minute.

**The critical runner never builds one.** `relaysTick()` sets a bit in
`g_relayPending[]` under `g_relayMux` and nothing else; the io task turns bits
into messages at 1 Hz, where allocating a String is allowed. A 50 ms deadline
and a JSON serialiser do not belong in the same function — that is the same
mistake that panicked this board with an interrupt watchdog.

**The sticky mask is per CONSUMER.** It is take-and-clear, so one shared mask
means whichever reader arrives first steals the event from the other: the
history record would silently lose every watering the telemetry happened to
report first. `relayStickyTake(RELAY_STICKY_HISTORY | _TELEMETRY)`.

**`relayN` and `relayNRan` answer different questions** and both are published.
The first drives a live indicator; the second is the only one that can see a
watering that started and finished between two publishes.

**The relay event carries NO bare `relay` key any more.** It held a ZERO-based
index while every `relayN` telemetry key is one-based, so the stored series
contains records reading `{relay: "2", relayName: "Reservatorio", relay3Event:
"started"}` and anything joining the two was off by one. `relayNEvent` already
names the relay at the correct index, which makes the bare key redundant as well
as wrong. It was DELETED rather than renumbered, deliberately: renumbering would
give a key already in the stored history a second meaning with nothing in the
data to say where the change happened, which is the more expensive of the two
mistakes. The refusal event still carries a bare `relay`, and it has the same
zero-based problem — left alone because dropping it would leave a refusal with
no index at all, and there is no `relayNRefused` key to replace it without
opening a new seam.

### Step values are published on CHANGE, and never sampled

The third mechanism above is for a *quantity*. A relay state, a reservoir
contact, a reboot counter and a cumulative litre total are none of those: they
sit still and then step. Sampling one into every periodic payload spends a
datapoint per key per tick restating what the cloud already knows — a value
re-sent unchanged carries no information — and it still reports the step LATE,
by up to a whole publish period. **An asynchronous change belongs in
asynchronous telemetry.**

So these leave through `tbPublishEvent()` the moment they move, from the io task
at 1 Hz, and are absent from the periodic payload entirely: `relay1..N`,
`relay1..NRan`, `relayMask`, `relayRanMask`, `connectionLoss`,
`wateringCycles`, `dhtErrorRate`, `flowRate`, `flowTotalLitres`,
`reservoirRaised`, `cloudState`.

**`cloudState` carries one exception the others do not**, and it is the same
one `moistureNFault` carries: it is published only while the model HAS an
answer. Outside the fitted daylight window there is no clear-sky reference and
therefore no state, and a heartbeat restating "no reference" every fifteen
minutes all night is precisely the waste this mechanism exists to remove. The
transition BACK is still spent — one publish of `no reference` at dusk — or an
operator reading "latest" at midnight is told the sky is clear.

- **`mqtt.heartbeatSec` (default 900) is the floor under it.** A key that never
  changes would otherwise be absent from the cloud for as long as it stays put,
  and an operator reading "latest" cannot tell a stable value from a device that
  died a week ago. Every step key is re-sent at least this often whether it
  moved or not.
- **A float is compared against a DEADBAND, never with `!=`.** Two doubles
  representing the same measurement differ in their last bits for ever, so an
  exact comparison would publish on every tick and buy nothing. The deadband is
  the smallest change worth a datapoint on that channel — a question about the
  sensor, not about floating point. `flowRate` 0.05 L/min, `flowTotalLitres`
  0.005 L, `dhtErrorRate` 0.5 points; counters and contacts pass 0.0, which is
  exact because their values are whole numbers a double holds exactly.
- **The heartbeat comparison is unsigned**, so `millis()` wrapping at 49 days
  costs one early heartbeat rather than stalling every key for another 49.
- **It runs on the io task, not the critical runner**, for the reason
  `publishRelayEvents()` does. `relaysTick()` still only sets a bit.
- **The comparison itself lives in `include/core/step_publisher.h`**, free of
  Arduino, so `test_step_publisher` can reach it — the same reason
  `segment_index.h` exists, and for the same kind of arithmetic.
- **`relayStickyTake(RELAY_STICKY_TELEMETRY)` moved with it**, and there must
  stay exactly one caller: it is take-and-clear, so a second one would steal
  activations from the first. The step publisher returns before the take on the
  ThingSpeak backend, where `tbPublishEvent()` is a no-op — which is what it
  already did, since `buildThingsBoardPayload()` was never reached there.

### `firmware` is not periodic telemetry

It was published in every payload: a string that changes at a reboot and at no
other moment, restated ~1400 times a day. It is **gone from the periodic
payload**, and nothing was added to replace it, because two places already carry
it: `tbOnConnect()` publishes `current_fw_version` as a CLIENT ATTRIBUTE — the
key ThingsBoard itself reads to decide an update landed — and the boot event it
sends on the same connection carries `firmware` as TELEMETRY, under the same key
name. So the timeseries still gets a `firmware` point stamped at every moment
the version could possibly have changed.

### Still sampled, and whether that is fine

- **Moisture, luminosity, temperature, air humidity, water level, ping** —
  accumulated over the publish interval, and now published TWICE: the window
  mean under the original key, and the instantaneous value under the same name
  plus **`Now`** (`temperature` + `temperatureNow`, `moisture1` +
  `moisture1Now`, …), and a third under **`Sd`**. The mean is what a trend is
  read from; the instantaneous value is the only one that can show a spike, and
  at a 300 s period a 1 Hz window is 300 samples deep — easily enough to flatten
  a watering into nothing.

  **`Sd` is the error bar, and it is a standard deviation rather than the
  variance the accumulator stores** — an error bar is drawn in the
  measurement's own units, and units-squared does not overlay a chart. It exists
  because the archive could not answer *"had this reading settled?"*. On
  2026-09-03 a whole probe calibration had to be done by polling the live device
  at 30 s intervals, because every stored number was a mean with no spread
  beside it, and a mean of 74.0 at sd 0.05 is a different claim from a mean of
  74.0 at sd 30. Nothing already past could be re-examined. Cost: one key per
  continuous channel, +288 datapoints/day each — on this board's six live
  channels, ~4 320/day becomes ~6 050, still a quarter of the 25 100 measured
  before any of this.

  It is NOT the cloud detector's statistic and must not be confused with it.
  `cloudVariability` is a normalised sample-to-sample STEP; `Sd` is the spread
  of the LEVEL. Measured over 5-minute windows, a smooth ramp and a flickering
  sky are indistinguishable by level spread — both sd 3.2 — while the step
  separates them 1.10 against 3.61. Two statistics, two questions.

  **The AVERAGE keeps the original key name, and that is the load-bearing half
  of the decision.** Every one of those keys has carried `getAverage()` since
  the ThingsBoard payload existed and there is already stored history under
  them. Redefining one would leave a single series whose meaning changes
  part-way through with nothing in the data to say where — the same defect as
  renumbering a relay index, and harder to notice, because a number that merely
  shifts still looks like a reading.
- **`moistureNFault`** — the probe-health verdict, and the one key that is
  published only when there IS a fault, exactly as `/data.json` omits the field
  and as `state` is omitted rather than sent empty. A key saying "fine" on every
  healthy probe every period is the waste this section is about, and it trains
  the eye to skip the one case that matters. The exception is the transition
  BACK: without one publish of the cleared verdict the cloud shows the last
  fault for ever, so that single datapoint is spent.
- **`Min Free Heap`** — already a low-water mark, which is the sticky form of a
  quantity. A spike between publishes is caught. `Free Heap` alone would not.
- **`cloudVariability`** — the mean |dk| per minute over the publish period,
  and the only cloud quantity that is a LEVEL rather than a transition. It is
  here because at 300 s the window mean alone recovers 5.1 % of the transients
  in the window against the spread's 80 %; see
  [Cloud cover](#cloud-cover-an-empirical-clear-sky-reference-and-what-the-data-could-not-settle).
  It is take-and-clear with exactly ONE caller, for the reason
  `relayStickyTake()` has exactly one.
- **The moisture CLASS** (Dry/Humid/Wet) — sampled. Defensible while the soil
  moves slowly, but a watering causes a fast transition, so a class-change
  event is the obvious next one if a badge is ever seen to skip a state.
- **Login failures and per-IP lockouts** — only in the 8 KB rolling log, which
  a busy device overwrites within hours. These are security events on a device
  exposed to a LAN and they have no durable record anywhere.
- **Config writes and restarts** — same: an audit trail that lives only in a
  log that rotates.

## Relay seams — why `startRelay()` is the only door

`src/relays.cpp` owns switching and nothing else. Two seams connect it to the
rest of the firmware, both implemented in `tasks.cpp`:

- **`relayStartAllowed(index, reason)`** — asked BEFORE the relay is claimed.
  Today it is the reservoir interlock (`io.floatSwitch.interlock`): it refuses
  a pump while the float reads empty, and exempts `fillRelay`, because blocking
  the only thing that fixes an empty tank would deadlock the interlock against
  itself. Put a new veto here, never at a call site — there are five paths to a
  pump (web `/control`, TalkBack, a schedule, a ThingsBoard RPC,
  `checkMoisture`) and the one that gets forgotten is the one that runs dry.
- **`relayStartedHook(index, duration)`** — called after energising, for the
  watering bookkeeping (ThingSpeak field 2, the pre-watering moisture snapshot,
  arming `checkMoisture`) that switching itself knows nothing about.

`startRelay()` returns `bool`. A refusal used to be a log line and silence;
every caller can now say why nothing happened, which is what makes the RPC
answer `{"ok":false,"error":...}` instead of a lie.

**The interlock defaults to off** and must stay that way: an unwired float sits
at the internal pull-up, which reads as empty, so a default-on interlock would
stop every watering on every board that declares `io.floatSwitch` without a
sensor actually wired to it. There is deliberately no auto-refill — one float failing toward
"empty" would hold the fill relay on, and that failure mode is a flood.

## ThingsBoard downlink — RPC and firmware over MQTT

`src/thingsboard.cpp`. Inert unless `config.mqttBackend == "thingsboard"`; the
ThingSpeak path never reaches it. Two config gates: `mqtt.rpc` and
`mqtt.fwUpdate`, both default true, plus `mqtt.fwTitle` (default `esp-garden`).

| Topic | Direction | Purpose |
|---|---|---|
| `v1/devices/me/attributes` | both | client attributes out (`current_fw_title`/`current_fw_version`), shared-attribute push in |
| `v1/devices/me/attributes/request/<id>` → `.../response/<id>` | out/in | ask for the `fw_*` shared keys on every connect |
| `v1/devices/me/rpc/request/+` → `.../response/<id>` | in/out | `getStatus`, `getRelays`, `startRelay`, `startWatering`, `stopRelay`, `getFirmware`, `checkFirmware`, `restart` |
| `v2/fw/request/<id>/chunk/<n>` → `v2/fw/response/...` | out/in | the firmware stream, 4 KB per packet |

**TRAP — nothing may publish from inside the PubSubClient callback.**
PubSubClient uses ONE buffer for both directions, and the `payload` handed to
the callback points into it; publishing from there overwrites the packet being
processed. `tbHandleMessage()` therefore only records intent and every outbound
message leaves from `tbLoop()`, which `mqttLoop()` calls *after*
`mqttClient.loop()` returns. The outbox is a bounded `std::vector` and a full
one logs an error rather than dropping silently — a lost RPC reply looks like a
device that ignored the command.

**The receive buffer must hold a whole chunk.** `mqttSetup()` calls
`tbRequiredBufferSize()` before connecting. Undersize it and PubSubClient
discards every chunk without a word: the download sits at 0 % forever.

Guards that are not in fullbot's version and should not be removed:

- **`fw_title` must match `config.mqttFwTitle`.** One tenant holds every device
  an operator owns; assigning the wrong package to a device profile is one
  wrong click, and an image built for another board needs USB to recover from.
- **A download will not start while any relay is energised.** It ends in a
  reboot, and on an ESP32 reset every GPIO floats until `relayPinsSafeInit()`
  runs — which on these active-low boards means the pump switches back on for
  the length of the boot. `tbLoop()` holds the announcement until relays idle.
- **Both OTA paths share one `Update` object.** `/updateEnable` answers `409`
  while `tbFotaInProgress()`, and the cloud path waits on `Update.isRunning()`.
  Letting both run corrupts whichever finishes second, and `Update` reports
  success for it — the damage only shows at the next boot.
- **A stalled download aborts after 5 retries**, releasing `Update`. Otherwise
  an abandoned cloud FOTA locks out the browser OTA that exists to recover from
  a bad cloud FOTA.
- **`fwVersionDiffers()` accepts a downgrade.** Rolling back is an operator
  decision made in ThingsBoard, and it is the only recovery that does not need
  USB. `test/test_fw_version/` covers the comparison, including the
  lexicographic trap (`2.10.0` vs `2.9.0`).

The filesystem image cannot be delivered this way: `handleUpdateUpload` picks
`U_SPIFFS` from the uploaded *filename*, and the FOTA path always writes
`U_FLASH`. `littlefs.bin` still goes through `/update.html`.

`mqttLoop()` now backs off exponentially (1 s → 60 s) instead of retrying every
`loop()` iteration, which used to bury the reason for a refused connection in
hundreds of identical log lines a minute.

## Porting from fullbot-firmware

What the user asked for. Ordered by value, with the real blockers.

| Bring over | From | Status |
|---|---|---|
| Custom partition table | `partitions/hw_v2_0_4mb.csv` | **Done** — `partitions/esp_garden_4mb.csv`. Needs a serial reflash on every existing board |
| `CustomLogin` + `UserStore` + `Role` | `src/network/CustomLogin.cpp`, `src/core/UserStore.cpp` | **Done** — `src/custom_login.cpp`, `src/user_store.cpp`, seeded from `ota.*` instead of a compiled password |
| Critical-task pattern | `src/core/Controller.cpp` + `docs/task-scheduler.md` | **Done** — relay timing and the error blink |
| Cached status payload | `handlers/StatusHandlers.cpp` | **Done** — `webUpdateDataCache()` |
| `WebServer` + `handlers/` split | `src/network/` | **Done** — `web.cpp` keeps the route table; `web_data`, `web_config`, `web_ota`, `web_users`, `web_files`, `web_capabilities`, `web_moisture` hold the handlers |
| `FOTA` class | `src/core/FOTA.cpp` | Not done. The free functions work but do not share fullbot's structure |
| `ConfigFile::saveFile()` + `POST /config.json` | `src/core/ConfigFile.cpp`, `handlers/ConfigHandlers.cpp` | **Done** — plus a masked `GET`, an id check and a refuse-to-save guard that fullbot does not have |
| `/users.json` + `POST /users` management UI | `handlers/UsersHandlers.cpp` | **Done** — plus guards fullbot lacks: last-admin demotion, password length, and session invalidation on delete |
| `Logger` with `webRead()` + size-based rotation | `src/core/Logger.cpp` | Not done. This repo's logger rotates only on boot and always returns the whole 8 KB buffer |
| `TelemetryAggregator` | `src/core/TelemetryAggregator.cpp` | **Superseded** — `AccumulatorV2` was rewritten as a fixed-size ring instead, which buys the same no-allocation property while keeping the `val`/`avg`/`var` shape the UI depends on |
| Webpack frontend | `~/solarbot/fullbot-frontend` | Not done, but the reason it mattered is gone: Bootstrap and jQuery are vendored and gzipped, so nothing degrades offline. What a bundler would still buy is dead-CSS elimination — 29.9 KB of Bootstrap for the handful of classes these pages use |
| `SelfTest` | `src/core/SelfTest.cpp` | Not done. A boot that reports a dead moisture probe is worth more than a season of unusable data |
| `test/` harness (`[env:native]`) | `platformio.ini` + `test/support/native_includes/` | **Started** — env, CI job and `test_accumulator`. The stub layer for LittleFS/JSON/FreeRTOS is still to transplant |

Concrete gotchas measured in this tree:

- **The partition change is the reason anything else fits.** On the stock table `espgarden5` sat at 83.8 % of a 1.31 MB app slot (~210 KB free); on `partitions/esp_garden_4mb.csv` it is at **63.1 % of 1.69 MB (~652 KB free)**. The whole auth stack cost ~10 KB. **This cannot be delivered over OTA** — the running image would be rewriting the partition map underneath itself, so every board already flashed needs one serial upload to move over.
- **`lib_deps` used to point at a bare `me-no-dev/ESPAsyncWebServer` git URL with no tag.** That repo moved to the ESP32Async org, so the dependency silently tracked whatever HEAD was. It is now pinned to `ESP32Async/ESPAsyncWebServer@^3.7.8` + `ESP32Async/AsyncTCP@^3.4.4` (resolves to 3.12.0). **3.7+ is required for `AsyncURIMatcher`**; middleware alone works from 3.3.
- **3.7+ no longer pulls in `WiFi.h` transitively.** `web.cpp` includes it explicitly; a file that used to compile on the old fork may now need the same.
- **`Arduino_JSON` is bumped to `^0.2.0`** to match fullbot, so code copied across needs no adaptation. Its quirks apply here too: build nested payloads by writing through `parent[key][...]`, never by returning a `JSONVar` by value, and never subscript a `const JSONVar`.
- **`JSONVar::operator[]` returns BY VALUE, and this has already cost a live incident.** Casting a chained subscript straight to `const char*` — `(const char*)doc["wifi"]["password"]` — reads a buffer the temporary has already freed and silently yields an **empty String**, not the value. In `handleConfigPost` that made every mask comparison fail, so `POST /config.json` wrote eight asterisks over the real WiFi, OTA, ThingSpeak, TalkBack and MQTT credentials; the device kept running on its in-memory config and would only have died at the next boot. **Always bind to a named `JSONVar` local before converting**, and assign `String::c_str()` rather than another `JSONVar` (move-assign from an rvalue yields a null child). The handler now also refuses to save a document that still carries a mask.
- **Verify a config write by the byte count, not by reading it back.** `GET /config.json` masks secrets, so a clobbered credential and an intact one look identical. `saveFile()` logs `Saved /config.json (N bytes)` and `/logs` is ADMIN-readable — compare N against the compact length of the known-good document. That check is what caught the incident above before a reboot.
- Fullbot runs on **ESP32-S3 with PSRAM**; these boards are plain ESP32, 320 KB DRAM, none. Anything sized for PSRAM (the ML models, large static buffers) does not come across.
- **Both brokers are supported now**, selected by `mqtt.backend` (`thingspeak` | `thingsboard`). The transport block is shared; only the topic and the payload differ. ThingsBoard authenticates with the device access token in `mqtt.username` and an empty password, and `mqtt.useTLS = false` selects a plain `WiFiClient` for a self-hosted broker on 1883 — TLS costs 30–45 KB of heap for the handshake.
- **ThingsBoard is the way out of the 8-field ceiling.** Its JSON payload carries every probe, each probe's Dry/Humid/Wet state, every relay, the DHT error rate and `FW_VERSION` with no field numbering to negotiate — which is what keeps `thingSpeak.moisture2Field` an open question on ThingSpeak and not on ThingsBoard.
- Still to take from fullbot's `MQTTClient`: shared-attribute FOTA. The reconnect backoff is done. **The file-backed publish queue was examined and declined** — its design is SD-card-shaped and unbounded, and this board has no SD; see the publish-queue note under [Sensors, accumulators & telemetry](#sensors-accumulators--telemetry) for what a safe version would have to look like and why the trade is poor.

---

## Conventions

- **English in code** — identifiers, comments, log strings, commit messages. Reply to the user in the language they write in (Portuguese here).
- **`.clang-format` is Mozilla base, 4-space indent.** Nothing enforces it; do not reformat lines you did not touch.
- Includes are prefixed: `core/`, `network/`. `BuildConfig.h` carries no prefix.
- Feature flags live in `include/BuildConfig.h` (`FW_VERSION`, `USE_WEBSERVER`, `USE_MQTT`, `USE_OTA`, `USE_TALKBACK`, `USE_WATERING_PWM`), along with the capacity caps `RELAY_MAX` and `MOISTURE_MAX`. **`platformio.ini` carries no hardware flags at all** — what a board has is in its own `config.json`, and the envs differ only by `board`.
- `FW_VERSION` in `BuildConfig.h` is the version ThingsBoard compares an offered package against and the string `/data.json` reports as `Status.Firmware`. **Bump it in the same commit as the change it names**, or a FOTA of that change is a no-op the broker reports as success.
- **No AI co-author trailers** in commits. **Never `git add -A`** — stage explicitly. **Commits and pushes no longer need to be asked for** (standing authorization from the repo owner), but the gate does not move: five envs build, `pio test -e native` passes and `python scripts/check_lines.py` is green before a commit exists. Never commit `data/config.json` or anything under `backups/`.
- Commit format: imperative + conventional tag (`feat | fix | refactor | chore | docs | test | perf | style`), e.g. `fix(tasks): construct DHT after config load`.
- **Never overwrite `data/config.json`** (gitignored, holds a real device's credentials) and never commit real credentials — only `data/config.template.json` is tracked.
- Do not create new `.md` files unless asked; update this file or `README.md`.

## End-of-change checklist

- [ ] `pio test -e native` passes; new behaviour in testable code has a regression test
- [ ] `pio run` builds all five envs clean (they share one shape now, so a break in one is a break in all)
- [ ] `-t buildfs` run if `data/` changed, and **flash usage still under 100 %** of the app slot
- [ ] `/data.json` payload changes mirrored in `data/index.js` **and** the simulator (`scripts/sim_state.py` for the payload, `sim_config.py` for the document, `sim_moisture.py` for the model, `dev_server.py` for the route and `PUBLIC_PATHS`)
- [ ] New config key: all 5 edits, template included; new sensor KIND: all 8
- [ ] `data/config.json` untouched, no credentials introduced
- [ ] `python scripts/check_lines.py` passes
- [ ] **[What has actually run](#what-has-actually-run) is still honest** — anything that ran moved up *because it ran*, and anything newly written went into the unverified list
- [ ] No number in this file without an execution behind it
- [ ] Conventional commit message suggested — **no commit unless the user asked**
