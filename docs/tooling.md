# Off-device tooling

Everything here runs on a workstation, never on the ESP32. All are standard
library only, and all of them are allowed to answer "the data does not support
this" — several currently do.

| script | what it is for |
|---|---|
| `provision_config.py` | Turns a board template into one device's `data/config.json`. Reads the `ID:` line off the serial port — the number `loadFile()` actually compares against — and refuses anything that would be rejected at boot: a placeholder left in, a credential under four characters, a probe pointing at a relay that does not exist, or a CA that does not match the chosen backend. |
| `tb_export.py` | Incremental ThingsBoard → SQLite archive under `backups/`. ~20 s per run; the only durable copy of the 300 s **publish**. |
| `history_export.py` | Incremental `GET /history.json` → SQLite archive under `backups/`. The device's own **60 s record**, which nothing collected before and which the buffer recycles in about two days. Pages by logical index and refuses to believe one: a segment rotation shifts every index at once, and a walk that trusts one silently loses a whole segment. `--plan` prints the request count, the byte total and the cadence against no network at all. |
| `telemetry_ui.py` | Local browser for that archive on **:8090** — charts, key inventory with dead-key detection, boot and gap timeline. Read-only. |
| `moisture_fit.py` | Fits the soil-moisture parameters off the whole archive instead of the 24 h the board holds. Emits JSON in a dry run; refuses per probe and names the check that refused. `--history-db` fits from the device's own 60 s record instead, which needs no device: the archive carries the `(index → name, pin, relay)` binding it was collected under. |
| `moisture_thermal.py` | Asks whether ambient temperature biases a resistive probe. Ships two admission GATES and no correction, because this archive cannot measure the coefficient. |
| `drying_fit.py` | Fits the drying curve — linear against exponential against double-exponential — and refuses an asymptote the data does not constrain. It has refused every segment. |
| `cloud_fit.py` | Fits the clear-sky reference and writes the generated header. Prints the regime table on every run, which is how the next sensor-mounting change becomes visible. |
| `et0_fit.py` | Geocodes `postalCode` and scores the device's ET0 against a public station. Allowed to answer "do not enable this", and did. |
| `moisture_calibration.py` | The older ThingSpeak-channel inspector; refuses a drying rate over a window shorter than three days. |

Each carries `--self-test` where it has pure logic worth pinning; those run in
Python because `pio test -e native` is C++ Unity and cannot reach them.
