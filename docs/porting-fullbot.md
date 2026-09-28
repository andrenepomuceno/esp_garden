# Porting from fullbot-firmware

What the user asked for. Ordered by value, with the real blockers.

| Bring over | From | Status |
|---|---|---|
| Custom partition table | `partitions/hw_v2_0_4mb.csv` | **Done** — `partitions/esp_garden_4mb.csv`. Needs a serial reflash on every existing board |
| `CustomLogin` + `UserStore` + `Role` | `src/network/CustomLogin.cpp`, `src/core/UserStore.cpp` | **Done** — seeded from `ota.*` instead of a compiled password |
| Critical-task pattern | `src/core/Controller.cpp` + `docs/task-scheduler.md` | **Done** — relay timing and the error blink |
| Cached status payload | `handlers/StatusHandlers.cpp` | **Done** — `webUpdateDataCache()` |
| `WebServer` + `handlers/` split | `src/network/` | **Done** — `web.cpp` keeps the route table; `web_data`, `web_config`, `web_ota`, `web_users`, `web_files`, `web_capabilities`, `web_moisture` hold the handlers |
| `FOTA` class | `src/core/FOTA.cpp` | Not done. The free functions work but do not share fullbot's structure |
| `ConfigFile::saveFile()` + `POST /config.json` | `src/core/ConfigFile.cpp` | **Done** — plus a masked `GET`, an id check and a refuse-to-save guard fullbot does not have |
| `/users.json` + `POST /users` UI | `handlers/UsersHandlers.cpp` | **Done** — plus guards fullbot lacks: last-admin demotion, password length, session invalidation on delete |
| `Logger` with `webRead()` + size-based rotation | `src/core/Logger.cpp` | Not done. This logger rotates only on boot and always returns the whole 8 KB buffer |
| `TelemetryAggregator` | `src/core/TelemetryAggregator.cpp` | **Superseded** — `AccumulatorV2` was rewritten as a fixed-size ring, which buys the same no-allocation property while keeping the `val`/`avg`/`var` shape the UI depends on |
| Webpack frontend | `~/solarbot/fullbot-frontend` | Not done, and the reason it mattered is gone: Bootstrap and jQuery are vendored and gzipped, so nothing degrades offline. A bundler would still buy dead-CSS elimination |
| `SelfTest` | `src/core/SelfTest.cpp` | Not done. A boot that reports a dead moisture probe is worth more than a season of unusable data |
| `test/` harness (`[env:native]`) | `platformio.ini` + `test/support/native_includes/` | **Started** — env, CI job and tests. The stub layer for LittleFS/JSON/FreeRTOS is still to transplant |

Concrete gotchas measured in this tree:

- **The partition change is the reason anything else fits.** On the stock table `espgarden5` sat at 83.8 % of a 1.31 MB app slot (~210 KB free); on the new one it is at **63.1 % of 1.69 MB (~652 KB free)**. The whole auth stack cost ~10 KB. **This cannot be delivered over OTA** — the running image would be rewriting the partition map underneath itself, so every board already flashed needs one serial upload.
- **`lib_deps` used to point at a bare `me-no-dev/ESPAsyncWebServer` git URL with no tag**, so after the repo moved to the ESP32Async org the dependency silently tracked whatever HEAD was. It is now pinned to `ESP32Async/ESPAsyncWebServer@^3.7.8` + `ESP32Async/AsyncTCP@^3.4.4`. **3.7+ is required for `AsyncURIMatcher`**; middleware alone works from 3.3. 3.7+ also no longer pulls in `WiFi.h` transitively — `web.cpp` includes it explicitly, and a file that used to compile on the old fork may need the same.
- **`Arduino_JSON` is bumped to `^0.2.0`** to match fullbot. Its quirks apply here too: build nested payloads by writing through `parent[key][...]`, never by returning a `JSONVar` by value, and never subscript a `const JSONVar`.
- **`JSONVar::operator[]` returns BY VALUE, and this has already cost a live incident.** Casting a chained subscript straight to `const char*` — `(const char*)doc["wifi"]["password"]` — reads a buffer the temporary has already freed and silently yields an **empty String**, not the value. In `handleConfigPost` that made every mask comparison fail, so `POST /config.json` wrote eight asterisks over the real WiFi, OTA, ThingSpeak, TalkBack and MQTT credentials; the device kept running on its in-memory config and would only have died at the next boot. **Always bind to a named `JSONVar` local before converting**, and assign `String::c_str()` rather than another `JSONVar` (move-assign from an rvalue yields a null child). The handler now also refuses to save a document that still carries a mask.
- **Verify a config write by the byte count, not by reading it back.** `GET /config.json` masks secrets, so a clobbered credential and an intact one look identical. `saveFile()` logs `Saved /config.json (N bytes)` and `/logs` is ADMIN-readable — compare N against the compact length of the known-good document. That check is what caught the incident above before a reboot.
- Fullbot runs on **ESP32-S3 with PSRAM**; these boards are plain ESP32, 320 KB DRAM, none. Anything sized for PSRAM does not come across.
- **Both brokers are supported**, selected by `mqtt.backend` (`thingspeak` | `thingsboard`). The transport block is shared; only the topic and payload differ. ThingsBoard authenticates with the device access token in `mqtt.username` and an empty password, and `mqtt.useTLS = false` selects a plain `WiFiClient` for a self-hosted broker on 1883 — TLS costs 30–45 KB of heap for the handshake.
- **ThingsBoard is the way out of the 8-field ceiling**, which is what keeps `thingSpeak.moisture2Field` an open question on ThingSpeak and not on ThingsBoard.
- Still to take from fullbot's `MQTTClient`: shared-attribute FOTA. The reconnect backoff is done. **The file-backed publish queue was examined and declined** — see the publish-queue note under [Sensors, accumulators & telemetry](sensors.md).
