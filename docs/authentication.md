# Authentication

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
| `/data.json`, `/history.json`, `/moisture.json` | any signed-in user |
| `/schedules.html` | ADMIN (the page reads and writes `/config.json`) |
| `/control` | OPERATOR |
| `/config.json`, `/logs`, `/updateEnable`, `/update`, `/users.json`, `/users`, `/spiffs/*` | ADMIN |

`/logout` needs only a valid token. `/spiffs/users*`, `/spiffs/sessions*` and
`/spiffs/config*` answer 403 even to an ADMIN — the browse handler would
otherwise serve the salted password hashes, the live bearer tokens and the
plaintext WiFi and MQTT credentials.

## Changing settings without reflashing

`GET /config.json` returns the current configuration with every secret
(`wifi.password`, `ota.password`, `thingSpeak.apiKey`, `talkBack.apiKey`,
`mqtt.password`) replaced by `********`. Edit the document, send it back as the
`config` form parameter of `POST /config.json`, and any field still carrying the
mask keeps its stored value — so credentials never leave the device.

The write replaces the whole file, and the document's `id` must match the
device. Nothing re-reads the configuration at runtime: the response carries
`restartRequired: true`, and the change takes effect on the next boot
(`POST /control` with `reset=1`).

## Partition table

The firmware ships a custom 4 MB layout (`partitions/esp_garden_4mb.csv`) with
1.69 MB per OTA slot. **This cannot be delivered over OTA** — a board flashed
with the stock table needs one serial upload (`pio run -e <env> -t upload`) to
move to it.

## Reservoir interlock

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
