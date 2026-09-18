# 021 — The S3 carrier is the GARDEN now, with real pumps

**Opened** 2026-09-18. Stated by the operator: *"troquei espgarden1 por espgarden-s3"*,
and confirmed: the relays drive **real pumps**.

This invalidates the single most load-bearing safety assumption in CLAUDE.md,
which still describes `b580` as a bench board "with nothing on the relay outputs
and nothing planted" and says "no relay has been energised on it".

## What is now true

- **`espgarden-s3` (device `b580`, ESP32-S3 carrier) is the live garden.** Four
  probes are in real soil — all four channels read plausible, quiet values
  (81.90 / 86.95 / 83.84 / 73.71 at `var` 0.03-0.16), where three of them floated
  at `var` 15-68 a day ago. Two are named for plants (`Espada`, `Arranjo`).
- **The WROOM-32 `espgarden1` (`6224`) is no longer the garden.**
- Firmware 2.19.0, broker `tb.espgarden.com.br`, `mqtt.rpc` and `mqtt.fwUpdate`
  both **true**.

## The consequence that matters most

**The carrier's inverted relay safe state is now load-bearing on a live garden,
and it has never been measured.** CLAUDE.md is explicit that it is read off a
netlist: `J200` takes the GPIOs straight to the module's active-low IN pins and
the module's own pull-up is believed to hold them high through a reset, so every
relay should be RELEASED while the ESP32's pins are inputs — *"nobody has put a
scope on a relay coil through a boot."*

On the WROOM boards the opposite is true and documented: every reset pulses every
pump. **If the carrier's assumption is wrong, every reset now runs real pumps** —
and this board took an OTA plus four reboots on 2026-09-18 with pumps possibly
already attached.

**A meter or a scope on one relay coil through one boot settles it.** Until then,
treat every reset, OTA and power cycle on this board as unproven, not safe.

## What can start a pump today — checked, not assumed

Read off the device 2026-09-18:

| path | state |
|---|---|
| `schedules` | **`[]`** — none exist at all, not even disabled |
| `moisture[]` | every probe `dry: 0, wet: 0` — uncalibrated, so no badge and nothing autonomous |
| `io.floatSwitch.interlock` | **false** — vetoes nothing; there is deliberately no auto-refill |
| `POST /control` | live, OPERATOR role |
| **ThingsBoard RPC** | **live** (`mqtt.rpc: true`) — `startRelay` / `startWatering` / `stopRelay` reachable from a dashboard widget on `tb.espgarden.com.br` |

So nothing fires on a clock or on a sensor reading; both remaining paths need a
human. That is the current state and not a guarantee — adding a schedule or
calibrating a probe changes it.

## What to do

1. **Measure the relay safe state through a reset.** Highest value, lowest cost.
2. **Correct CLAUDE.md** — the Hardware v3 section, the 2026-09-17 carrier entry
   and every "no relay has been energised" claim now describe a board that no
   longer exists in that state.
3. **Decide about `mqtt.rpc`.** A pump reachable from an internet-facing broker is
   a deliberate capability, not a defect — but it should be a decision somebody
   made about a live garden rather than a template default carried over.
4. **The telemetry identity moved and nothing says so in the data.** The garden's
   404 142-datapoint history sits under ThingsBoard device `espgarden1`; new
   garden readings land under `espgarden-s3`, whose series begins at the
   2026-09-17 cutover with nothing behind it. Anything reading "the garden's
   history" now has to span two devices. Same defect class this repo already
   records for `moisture2` changing pots — recorded here rather than stitched.
5. **Re-point the tooling.** `DEFAULT_DEVICE` in `scripts/history_export.py` and
   `scripts/moisture_fit.py` is `espgarden1.local`; the garden is now
   `espgarden-s3.local`.
