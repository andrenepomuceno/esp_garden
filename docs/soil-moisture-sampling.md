# Soil moisture sampling period

`io.soilMoisturePeriodSec` — seconds between reads of the probe bank.
Default **1**. Editable in `/config.html`, under the **Io** tab.

Added in firmware 2.21.0.

---

## Why it is its own task

The probe read used to live in the `io` task, at 1 Hz. That task also:

- rebuilds the `/data.json` cache,
- publishes the relay, float, cloud and ET0 events,
- runs the step publisher,
- flushes the deferred `/sessions.json` write,
- computes the flow rate, which divides by the io period.

None of that may slow down because somebody wanted the ADC read less often, so
the probe block moved to a `moisture` task that takes its period from the
config. `sensorsReadIo()` keeps luminosity, water level, flow and the float.

Both are background tasks and the scheduler runs at most one per `execute()`,
so `webUpdateDataCache()` reading accumulators the moisture task writes is the
arrangement the `ambient` task has had since 2.14.0 — not a new race.

Background task count went 11 of 16 (12 with `USE_TALKBACK`).

## Accepted range

**1..10000 s.** Zero and negatives are refused because the value divides; the
upper bound only rejects absurd input. Out of range is a boot warning and the
previous value is kept — the same shape `history.periodSec` has. It is **not**
refused at save time, for the same reason the history capacity clamp is not:
the stored document is the operator's intent.

## What follows the period

The moisture accumulator window is `mqttPublishPeriodMs() / period`, floored
at 1. The floor is load-bearing: `AccumulatorV2::setMaxLen(0)` is a silent
no-op that would leave the window at whatever the constructor set.

`mqtt.publishSec` is clamped to 60..300. A sampling period longer than the
publish period therefore makes the window bottom out at 1, and the published
`moistureN` stops being a mean over a publish interval and becomes a single
sample. That is a consequence to know about, not a refusal.

## What does NOT follow the period

`g_probeHealthMinSamples` (120) and `g_probeHealthWindow` (600) in
`src/sensors.cpp` are counted in **samples**, not seconds. At 1 Hz they are
2 min and 10 min; at 30 s they are 1 h and 5 h. The statistic is unaffected —
the wait for a first verdict grows, and the evidence ages more slowly.

### The 400-count threshold does not bound this setting

`g_probeHealthMaxSd` = 400 ADC counts flags a probe whose reading jumps further
between consecutive conversions than soil can. A slower period concentrates a
physical event into fewer, larger steps, so it is fair to ask whether a
watering starts to look like a floating pin.

It does not. Measured against the real `src/probe_health.cpp`, a synthetic
1200-count rise over 5 minutes in a 600-sample window:

| period | ramp only | ramp + sd-80 white noise |
|---|---|---|
| 1 s | 2.0 | 113.7 |
| 5 s | 6.0 | 113.8 |
| 10 s | 8.7 | 114.0 |
| 30 s | 15.4 | 114.5 |
| 60 s | 21.8 | 115.3 |
| 100 s | 28.2 | 116.8 |
| 300 s | 49.0 | 122.1 |

Noise alone, no watering: **113.7**. Threshold: **400**.

`probeHealthStepSd()` is the *mean-subtracted* standard deviation of
consecutive differences, so a steady ramp cancels almost entirely; what
survives is its edges, spread across the window.

**A correction worth keeping.** An earlier version of this change capped the
period at 60 s on the argument that a watering contributes `1200/300 = 4`
counts per sample at 1 Hz and `4 × P` at period P, crossing 400 at ~100 s.
That reads the `4` in `probe_health.h`'s table — a mean *step* — as if it were
the statistic. The real figure at 100 s is 28, not 400. The scaling direction
was right; the magnitude was wrong by more than an order of magnitude, and the
cap it justified was invented.

## The snapshot lock

`sensorsReadMoisture()` computes its values **outside** `g_moistureSnapshotMux`
and the critical section is a 32-byte `memcpy`. The first version called
`getAverage()` inside it — four probes at a 60-sample window, walked twice
each, with interrupts disabled on that core — and the board panicked with
`InterruptWDTTimoutCPU1`. `probeHealthVerdict()` and `probeHealthStepSd()` are
outside for the same reason.

## Why `GET /config.json` fills the key in

`/config.html` generates its form from whatever the GET returns rather than
from a schema. A key absent from the stored document has no field, so on a
board provisioned before this key existed the setting would be unreachable
through the UI it was added for. `handleConfigGet()` therefore emits
`io.soilMoisturePeriodSec` with the value in force when the document has none,
and a save writes it back explicitly.

`?secrets=1` is unaffected — it returns the file verbatim, because a backup has
to restore what was actually stored.

## What has run, and what has not

**Not run on hardware. Nothing was flashed and no board was touched.**

Verified on the host and against `scripts/dev_server.py`:

- six envs build, `pio test -e native` 211/211, `check_lines.py` green;
- `GET /config.json` → edit → `POST` → re-read returns the new value;
- the simulator's `DeviceState` takes the period and stops sampling the probes
  on every tick;
- Chrome against the simulator renders a number input labelled
  *Soil Moisture Period Sec* in the `Io` tab.

Not verified:

- **any period other than the 1 s default, on a device.** At 1 s the behaviour
  is identical to 2.20.0 — same tick rate, same window, same health constants;
- the save button from a browser. Three clicks failed to register through the
  automation's coordinate frame and it was abandoned; the save path is covered
  by the HTTP round trip above and by nothing a browser did;
- `handleConfigGet()`'s fill branch on hardware — which is the branch every
  existing board will take.

Cost against 2.20.0: `espgarden2` **+1 004 B flash, +40 B static RAM**;
`espgarden_s3` +1 032 B and the same +40 B.
