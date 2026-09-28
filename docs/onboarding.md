# First-boot onboarding — the setup AP, and the marker that keeps a live garden out of it

A board with no usable configuration raises its own access point — SSID **`espgarden-<id>`**, password **`espgarden`** — serves a setup page, and writes its own `/config.json`. Everything else is configured normally through the existing pages once it is on the LAN. **Nothing in this section has run on hardware; see [the unverified entry](verification-log.md).**

## Say the security property first, because it is real

The AP password is in this repository and **`POST /onboarding` takes no token**. While the portal is up, anyone in radio range can write this board's Wi-Fi credentials and admin account and walk away with the device. That is the flow the operator asked for and the one every consumer device ships, and it is stated here rather than left to be discovered on a controller that switches real pumps.

**What bounds it is WHEN the portal can exist**, not what is behind it:

- a board that has ever associated carries no marker and **can never enter the portal again**, whatever happens to the router afterwards;
- so the window is "between flashing and the first successful association", which on a working setup is one boot.

**What does NOT bound it is authentication, and that is on purpose.** There is nothing to authenticate against: `UserStore` has no account on a board whose config never loaded, which is the whole reason the normal UI is unreachable in that state.

Two cheap mitigations were considered and are **not** implemented, because both change the operator's stated flow: a per-device AP password derived from the efuse MAC (printed on the serial console and on a sticker — this removes the shared secret entirely and costs one label), and a portal that closes itself after N minutes and reboots (which on a board that cannot associate is a reboot loop wearing a timeout). The first is the one worth doing if the exposure is ever judged too wide.

## The trigger has two arms, and the second one is where the danger is

| Arm | Fires when | Why it cannot be left out |
|---|---|---|
| 1 | `loadConfigFile()` returned false | Missing, unparseable, an `id` from another chip, or one of the seven length-checked strings under four characters. Terminal today: compiled defaults, `ssid "undefined"`, unreachable without USB |
| 2 | the config loaded but has **never** associated | A mistyped Wi-Fi password produces a perfectly VALID document. `loadFile()` accepts it, arm 1 never fires, and the board is off the network for ever |

**"Failed to associate, therefore raise an AP" is exactly the rule that must not exist.** Every router reboot, channel change and ISP outage satisfies it — on the live garden, which would then broadcast an AP with a published password and an unauthenticated config writer. So arm 2 is gated on a **marker file**, `/provisioned.pending`:

- `POST /onboarding` writes `/config.json` **and then** the marker. That order matters: losing power between the two leaves the new config with no marker, i.e. an ordinary configured board — the failure this feature removes, but not a new one. The other order leaves a marker with the OLD config, which is a working board spending 60 s probing at every boot.
- The first boot that reaches `WL_CONNECTED` **deletes the marker**, and from that moment the board is outside arm 2 structurally — not by a timeout, a counter or a heuristic.
- **`uploadPathIsProtected()` refuses that path**, so an ADMIN cannot re-arm arm 2 on a working device through `POST /spiffs/upload`. An ADMIN can already rewrite the whole config; the difference is that a config write is visible and this would not be.

`onboarding::decide(configLoaded, markerPresent, associated)` is the only thing in the tree that can answer "portal", it lives in an **Arduino-free header**, and `test_onboarding` holds it to a truth table over every input. The row that matters: **config loaded, no marker → `Normal`, with `associated` never read.** `mustProbeAssociation()` is the same guarantee for the WAIT — a configured board does not spend the 60 s either.

## The mode decision is one branch, in one place

`webSetup(configLoaded)` decides once and returns early. The portal's routes are registered by `onboardingBegin()`, which has **exactly one caller** — `grep -n onboardingBegin src/` is the whole audit. A reader checking whether a configured device exposes an unauthenticated `/config.json` writer reads one `if`, not a middleware chain.

| Portal route | Method | Auth | What it does |
|---|---|---|---|
| `/` | GET | **none** | The compiled setup page. No script, no stylesheet, no font |
| `/onboarding.json` | GET | **none** | Device id, chip family, firmware, the AP name, the refusal reason, and the templates this BUILD carries |
| `/onboarding` | POST | **none** | `template`, `ssid`, `password`, `username`, `adminPassword`, `hostname` → validate, write, restart |
| everything else | any | — | `302` to the AP's own address, which is what makes a phone open the sign-in sheet |

`tasksSetup()` returns after registering and starting the two **critical** tasks. It must: the rest of it ends in two bounded waits of up to 60 s each — pinging 8.8.8.8, then re-running NTP — and in AP mode there is no internet by definition, so both run to their deadlines. That is two minutes of a dead portal with somebody standing at the board holding a phone.

## Nothing the portal serves comes off the filesystem

One of the two ways arm 1 fires is `FILESYSTEM.begin(true)` **reformatting** a partition that would not mount, which takes every web asset with it. So the page is a string in `web_onboarding.cpp` and the templates are a table in `onboarding_templates.h`. A portal that needs `/bootstrap.css` to render is blank in half the cases it exists for.

**The templates are compiled in rather than packed into the image, and the family is selected by `CONFIG_IDF_TARGET_*`.** Only the compiler sees that macro; a file chosen by a build script would be a second statement of "this is an S3", free to disagree with the `board` line — the same argument `platformio.ini` makes for carrying no hardware flags. Offering a WROOM-32 template on the S3 carrier is the hazard `config_guard()` in `scripts/pio_assets.py` exists to refuse: relays on `FLOW_PULSE`, `FLOAT_SW` and `BTN_USER`, probes on the octal flash/PSRAM bus. `test_onboarding` walks every declared pin of every template through its own family's `pin_rules` predicates, and asserts that each family's templates are **refused** by the other's rules — without that second half the first proves nothing about the `#if`.

Three templates ship: `wroom32-v2` (hardware v2: relays 19/16/17/18, probes 36/34, DHT 23, LDR 39), `wroom32-minimal` (one relay on 19, one probe on 36), and `s3-carrier` (the `templates/config.espgarden_s3.json` pin map, **including its `io.i2c` bus and `io.sht4x` since 2.18.0**). That parenthesis was a claim of parity the compiled string did not honour from 2.15.0 to 2.17.0: it carried the pins and not the two I²C blocks, so a carrier onboarded through the portal came up with no thermometer at all and nobody noticed until board b580 was on a bench. `test_onboarding` now asserts it. Each template is a whole config document with **six holes** — `id`, `hostname`, `wifi.ssid`, `wifi.password`, `ota.username`, `ota.password` — and the handler fills those and nothing else. A template is a starting pin map, never a claim that the board has these sensors; everything after it is a `/devices.html` edit, which is the point of the runtime-hardware design.

**`mqtt.username` ships empty on purpose.** ThingsBoard carries the device access token there and nobody has one at setup time, so the link reports `down (rc=...)` until an operator pastes it into `/config.html`. An invented token would connect to nothing and report success, which this repo has already paid three years for once.

## The POST refuses before writing, not after rebooting

Otherwise the user submits, the board reboots, the portal comes back and nothing says why. `handleSubmit()` runs, in order: the four typed fields against `g_configMinStringLength` with their own messages; `configDocumentIsUsable()` on the merged document — the same check `POST /config.json` runs, so `documentPinsAreUsable()` is included; the `id` check `handleConfigPost` does; and the **backend/CA pairing**, because crossing `mqtt.backend` with `mqtt.cacert` fails TLS silently while the dashboard keeps saying MQTT is enabled. That pairing table is `onboarding::expectedCaFor()`, host-tested, and its Python twin is `BACKEND_CA` in `scripts/provision_config.py` — whose `refusals()` is the same list expressed for a workstation.

**It calls `requestRestart(3000)`, never `ESP.restart()`.** `request->send()` only queues the response and the `async_tcp` task that flushes it is the one the handler runs on, so a reboot in place guarantees the phone sees a reset instead of the confirmation. 3 s rather than `/control`'s 500 ms because the client is on an AP that is about to disappear.

**The `id` stops being the field that bricks a board.** The firmware knows `ESP.getEfuseMac() % 0x10000`, puts it in the SSID and writes it into the document itself, so nobody reads it off a boot line — which is what the whole [Swapping the board](verification-log.md) section is about.

## What it costs

Measured on `espgarden2`, a clean A/B against `bac7ad4` built from a `git archive` of the same tree: **+27 156 B flash (1 257 533 → 1 284 689), +432 B static RAM (66 592 → 67 024)**, taking the app slot 71.1 % → **72.6 %**. On `espgarden_s3`: **+26 060 and +432** (1 209 805 → 1 235 865; 65 464 → 65 896). Where it goes, per object with `xtensa-esp32-elf-size`: `web_onboarding.cpp.o` **18 331 B** — of which the page is **3 754**, the two WROOM-32 templates ~1 950, and `handleSubmit()` alone 6 612 in text, literals and refusal strings; `web.cpp.o` +543, `tasks.cpp.o` +305, `main.cpp.o` +205, `web_files.cpp.o` +126. The rest, about 7.6 KB, is the soft-AP and DNS code nothing linked before: `WiFiAP.cpp.o` **3 479** and `DNSServer.cpp.o` **2 093**, plus what they pull in.

**The captive portal is 2 093 B of that** and it is worth it: it is the difference between "connect and the page opens" and "connect, then find out the device is on 192.168.4.1". **The unused family's templates are dropped by the linker** — both are compiled, as `pin_rules.h` and `default_pins.h` both are, so one host test can hold both.
