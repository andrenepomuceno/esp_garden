# Soil moisture classification

Each capacitive probe has its own gain and offset, so Dry / Humid / Wet cannot
come from a shared threshold — nor from channel history. A month of stored data
was fitted and rejected for this: a drying trend alone explains 86 % of the
variance, so clustering it into three groups just returns two arbitrary slices
of that trend plus the near-zero readings of a disconnected probe as a third
"state".

Two mechanisms replaced it, and the device uses the first one that has earned
the right to answer:

1. **A trained model** — Gaussian naive Bayes fitted per probe from its own
   watering history. Used once it passes its gates.
2. **Two-point calibration** — the air and water anchors, split into equal
   thirds. Used while the model is still accumulating evidence.
3. **Nothing.** With neither available the probe reports no state, and the
   dashboard shows no badge rather than a made-up one.

`moistureState()` in `src/sensors.cpp` is that ladder, and everything that
displays a band goes through it: the `state` key on each `/data.json` input, the
dashboard badge, and the `moisture<N>State` telemetry key on ThingsBoard.

## Two-point calibration

1. Hold the probe **in air** (connected — a disconnected probe floats to a rail
   and that value is useless) and note the value on the dashboard. That is `dry`.
2. Submerge it to its marked line in water. That is `wet`.
3. Enter both in **`/devices.html`**, save and restart. The generic config
   editor shows `moisture` read-only — it is a top-level array of objects, and
   that editor would destroy one on save.

Until `dry` and `wet` differ this probe is uncalibrated and contributes nothing
to the ladder above. No ordering is assumed: whichever end is air, the span is
split in thirds from it.

## The trained model

What clustering could not do, the relay record can. A watering is an **event**,
and an event is a label: the soil is wettest shortly after its pump ran and
driest just before the next time it runs — the second is arithmetic rather than
an assumption, since moisture only decreases between waterings. That turns an
unsupervised problem nobody could solve on this data into a weakly supervised
one with a physical basis.

Each probe gets one Gaussian per class over a single feature, the reading
itself, and a classification is the maximum-a-posteriori class plus the winning
posterior as a confidence. Three numbers per class is the entire model, which is
why training can stream the history file one record at a time instead of holding
it in RAM, and why the parameters are something a human can read and disagree
with rather than a threshold that appeared from nowhere.

Labels come out of the I/O history buffer by time relative to each watering
edge on **that probe's own relay**:

| Label | Window |
|---|---|
| Wet | the **30 min** after a watering starts |
| Dry | the **60 min** before the next watering starts |
| Humid | everything between the two |

Where the two windows overlap — a zone watered less than 90 min apart — wet
wins. A reading only counts when a cycle can be placed around it: ahead of the
buffer's first watering nothing is humid, and after its last one only the wet
window counts. Everything else in those two tails is left out of the fit, as is
every reading from a probe the buffer holds no watering for.

**Training accumulates; it does not refit.** The history buffer holds 24 h and a
zone is watered once or twice a day, so a from-scratch daily fit would have one
or two events in it — a description of yesterday, not a model. The daily run
(the `moistureModel` task) instead multiplies the stored evidence by **0.93**
and folds the new day into it. All three sufficient statistics scale together,
so the mean and variance are unchanged and only the *confidence* in them decays:
yesterday's soil is still evidence about today's, just less of it. The resulting
**half-life is about ten days** — long enough to accumulate the events a single
day cannot supply, short enough that a probe moved to a different pot stops
being described by the old one inside a fortnight.

A run makes three passes over the buffer: one to find the watering edges, one to
fit, and one to refit while discarding every sample more than **3 σ** from the
first fit's mean for its class. The second pass is needed because the rejection
threshold is itself a function of the fit it protects; the rejection is needed
because a disconnected probe reads at a rail, which is precisely the component
BIC found when this history was clustered blind. At most 32 watering edges per
probe are tracked per run. The state is persisted to `/moisture_model.bin`, so
weeks of evidence survive reboots; a firmware whose struct layout no longer
matches discards the file and starts over rather than reinterpreting old bytes
as parameters.

## When it refuses, and why refusing is the feature

A probe gets **no band from the model at all** until every one of these holds,
and a freshly set-up probe will fail them for days:

| Gate | Threshold | Why |
|---|---|---|
| Watering events seen | ≥ **6** (cumulative, decayed with the rest) | A model fitted to one cycle describes that cycle. Six is roughly a week of once-daily watering |
| Accumulated weight, per class | ≥ **20** | At a 60 s history period one 30-minute wet window is 30 samples, so this is about two or three cycles seen |
| Fisher separation, `J = (µ_wet − µ_dry)² / (σ²_wet + σ²_dry)` | ≥ **4** | J = 4 puts the two means two pooled standard deviations apart. Below it the bands overlap enough that a badge is a coin toss wearing a posterior |
| Ordering | humid strictly between dry and wet | If humid is not between them the labels disagree with the physics that produced them, and every classification from the fit is noise |

Polarity is not assumed — `dry < humid < wet` and `dry > humid > wet` both pass,
exactly as the two-point calibration assumes nothing about which end is air.

A probe watered so often that it never dries out, or one whose relay assignment
is wrong, lands near J = 0 and stays blank. **That is the intended output, not a
degraded one.** A week with no badge means the device has not yet seen enough of
this pot to say anything; the failure it exists to prevent is already on record
here, where three confident clusters turned out to be an artefact of a drying
trend.

## Which pump feeds which probe

`moisture[i].relay` is the index into `io.relays` of the pump that waters probe
`i` — on a planter with one pump per zone, probe `i` is not necessarily fed by
relay `i`. It defaults to `i`; a value outside `-1 .. relayCount - 1` is logged
and ignored, and a default that lands past the last relay this board has is
forced to `-1` — claiming a relay that does not exist would label every reading
against an event that never fires.

**`-1` means no pump feeds this probe.** Nothing labels its readings, so it
never gets a model at all; it falls back to the two-point calibration, and
`/moisture.json` reports `no relay assigned: nothing labels this probe`.

No page edits this key yet. Set it with `POST /config.json`, or in
`data/config.json` before a filesystem upload — and **re-apply it after every
save from `/devices.html`**, which rebuilds the `moisture` array from its probe
rows carrying only `dry` and `wet`. A dropped key is not an error anywhere: the
probe silently reverts to the default "probe `i` is watered by relay `i`", and
the only symptom is a model trained against a pump that waters something else.

## Inspecting it

**`/moisture.html`** shows, per probe: each class's mean, standard deviation,
prior and accumulated weight; the separation; the live classification with its
confidence; and, when there is no classification, which gate blocked it. It also
reports what the last training run scanned — records, samples used, outliers
dropped, and each probe's absorption time constant. `GET /moisture.json` is
the same data, and it carries the gate
constants themselves so the page never hardcodes a threshold the firmware might
have moved.

A blank badge with no reason is what makes a classifier impossible to debug from
outside, which is why the reason is always there.

To inspect a ThingSpeak channel's history and see what it does and does not
support:

```bash
python scripts/moisture_calibration.py --days 30
python scripts/moisture_calibration.py --days 30 --end 2023-03-08   # a past window
python scripts/moisture_calibration.py --dry 94.0 --wet 12.0        # emit bands
```

## Soil moisture: a classifier trained on watering events

Three sources decide the Dry/Humid/Wet badge, and each rung is only reached when the one above refuses:

1. **The trained model** (`src/moisture_model.cpp`) — Gaussian naive Bayes, one Gaussian per class per probe, fitted from the probe's own history.
2. **The two-point calibration** (`moisture[i].dry` / `.wet`) — thirds of the probe's measured span.
3. **Nothing.** An empty string, and `/data.json` omits `state` entirely.

### Why the labels come from the relays and not from the data

Clustering the history was tried first and it produces confident nonsense. Measured on 29.7 days / 20 417 samples: a linear drying trend alone explains **86.4 %** of the variance (-0.323 points/day); with the trend removed, BIC on the residuals prefers k=2 and that second component sits at **-24.5** — the near-zero readings of a disconnected probe, not a soil state. Clustering the *raw* series returns three groups (8.3, 35.9, 41.3) that are the outliers plus two arbitrary slices of the trend, so thresholds from them label "dry" whatever comes late in any drying period. The relay record is what changes the problem. A watering is an **event**, and an event is a label:

| Window | Label | Why |
|---|---|---|
| `(T, T + 30 min]` after a watering | **Wet** | Not from T itself: absorption takes time, and the reading at the pump's own edge is still the old soil |
| `[T_next - 60 min, T_next)` | **Dry** | Arithmetic, not assumption — moisture decreases monotonically between waterings, so the minimum of a cycle is just before the next one |
| everything between | **Humid** | |
| before the first event in the buffer | *none* | There is no cycle to place the reading in | **Every sample carries a CONFIDENCE, not just a label**, passed as the weight `gaussianAdd()` has always accepted. Until it was used, a reading taken at the instant the pump started counted as firmly "wet" as one taken twenty minutes later — and boundary samples are precisely what blurs the class means together, which is what fails the separation gate. **Wet** follows the probe's own absorption curve (below); **Dry** ramps up as the next watering approaches, since the closest sample is the driest; **Humid** tapers toward both neighbours over `g_taperSec` (10 min).

**The watering RESPONSE is tracked and reported** — the mean rise from the dry window to the wet one, decayed like everything else. It is free, because the labels the fit already needs are what it is made of, and it is the cheapest evidence that a probe is in the pot its pump waters. A response near zero after two waterings is reported as `blockedBy` **before** the statistical gates, because it names a physical cause — disconnected probe, wrong pot, pump not running — where "bands overlap" would arrive days later and name only the symptom.

**What is deliberately NOT a feature: time since watering.** Adding it would raise accuracy and destroy the point. The model exists to say what the SOIL is doing; a classifier that leans on the pump schedule is a timer wearing a posterior, and it would report "wet" confidently through a disconnected probe or a failed pump — the exact failures the separation gate and the response check exist to catch. The watering record LABELS the training data and is never an input at classification time. `moisture[i].relay` says whose pump matters. **-1 means no pump feeds this probe**, and such a probe never gets a model at all — nothing labels its readings — so it falls through to the two-point calibration.

### Why training accumulates instead of refitting

The history buffer holds 24 h and a zone is watered once or twice a day, so a from-scratch daily fit has **one or two events**: a description of yesterday rather than a model. `moistureModelTrain()` multiplies the stored sufficient statistics by `g_moistureDecayPerRun` (0.93, a half-life of about ten days) and folds the new day in. All three moments decay together, so the *estimate* is unchanged and only the confidence in it ages — which is the intent. Gaussian naive Bayes was chosen partly for this: it needs only `weight`, `sum` and `sumSq` per class, so training **streams** over the history (`IoHistory::forEach`) instead of holding 69 KB of records in RAM, and the whole model is 12 doubles per probe.

### Absorption: the soil is a diffusion process, not a step

Water reaching a probe takes time, and how long depends on the soil, the pot and how well the probe touches either — a property of ONE probe, which is what `g_absorptionLagSec` was not: a five-minute guess applied to every probe on the board. Treated as first-order diffusion the reading approaches `m(t) = baseline + rise * (1 - e^-(t-T)/tau)`, and `tau` is estimated per probe from its own rises at the 63.2 % crossing, interpolated between the two samples that straddle it. `moistureTimeConstant()` is the whole of it: host-tested, free of Arduino.

- **The baseline is the last reading BEFORE the pump, not the first one after.** At the pump's own edge the soil still reads its old value; starting from the first post-watering sample measures a rise from a point already part of the way up it and returns a `tau` that is too small.
- **Interpolation is not decoration.** At a 60 s history period the raw crossing is quantised to a whole minute, so a probe that answers in three minutes is measured with a 30 % error.
- **It refuses more often than it answers, on purpose.** Fewer than `g_riseMinSamples` (5) samples, a rise under `g_riseMinPoints` (1 point), or no crossing inside the 30-minute wet window all return 0, which means *unmeasured* and never *instant*. A probe that does not answer its pump must not be handed a confident time constant.
- **Polarity is not assumed**: the crossing is tested in the direction the rise actually went.
- **Only pass 2 measures it.** Pass 3 walks the same records and would count every curve twice, and 3σ outlier rejection does not apply to a rise at all — a transient is a shape, and rejecting its samples against a class mean would flatten the thing being measured.
- **It is decayed like every other statistic**, so one watering measured through noise cannot rewrite the estimate, and a probe slowly losing contact with its soil shows up as a drifting `tau` well before the separation gate refuses it. `moistureAbsorptionConfidence(dt, tau)` is the weight a wet-window sample carries, so a fast probe reaches full confidence in three minutes and a slow one is still discounted at fifteen; the 5-minute ramp remains as the stand-in for a probe with no measurement yet. **`tau` is a WEIGHT on training samples, never a feature at classification time** — same line the watering schedule is held to, same reason.

### The gates — refusing is the feature

`moistureModelIsUsable()` reports nothing unless **all** of these hold:

- **`g_moistureMinEvents` = 6 watering events** (decayed). A model fitted to one cycle describes that cycle.
- **`g_moistureMinWeightPerClass` = 20** accumulated weight in every class.
- **`g_moistureMinSeparation` = 4** — Fisher's `J = (μ_wet - μ_dry)² / (σ²_wet + σ²_dry)`, so the means are at least two pooled standard deviations apart. A probe watered so often it never dries lands near 0 here.
- **Ordering**: humid must lie *between* dry and wet. Not decoration — if it does not, the labelling disagrees with the physics that produced it and every classification is a coin toss wearing a posterior. Polarity is not assumed, exactly as the two-point calibration does not assume it.

`/moisture.json` reports **which gate refused**, per probe. A classifier that silently declines is indistinguishable from one that is broken.

### Traps

- **The fit is two passes, and it has to be.** Pass one fits everything; pass two refits rejecting samples beyond 3σ of that first fit. The rejection threshold is a function of the fit it protects, so one pass cannot do it. This is what keeps an out-of-family reading from dragging a class mean to a value the soil never had.
- **It does not catch every disconnected probe, and it does not have to.** Measured here: an unplugged probe on floating GPIO 32 read 52.6 at variance 0.01 — mid-scale, quiet, indistinguishable from soil by value alone, and the 3σ test never fires on it. What catches it is the SEPARATION gate: a disconnected pin does not respond to its pump, so all three classes land on one number, J collapses toward 0 and the model is refused — the probe rejected for the reason it is actually broken.
- **All probes are trained in the same three passes**, not three passes each. Background tasks are cooperative: twelve passes over 1440 records on a four-probe board would stall MQTT and TalkBack for ten seconds a day.
- **The sticky relay mask means one watering spans several records.** Only the rising edge counts, or a long watering becomes several events.
- **Variance is floored** at `g_gaussianVarianceFloor`. A class whose samples are identical has zero variance, an infinite log-likelihood, and wins every comparison regardless of the reading.
- **Likelihoods are computed in the log domain.** They differ by many orders of magnitude here; computed directly they underflow to zero for every class at once, which reads as a tie.
- **The model file is discarded on a layout change** (`MOI1` magic + `sizeof(MoistureModelState)`), not reinterpreted. Weeks of evidence are cheaper to rebuild than a wrong band is to notice.
- Training also runs **5 minutes after boot**, not only every 24 h: a device power-cycled each evening would otherwise never reach its daily tick.

`test/test_moisture_classifier/` covers the maths on the host — the ordering gate, the separation gate, inverted polarity, the variance floor, the outlier z-score, that confidence tracks how much the classes overlap, and the absorption estimator (`tau` recovered from a synthetic rise, slow probe distinguished from fast, a non-responding probe refused, inverted polarity giving the same `tau`, too few samples refused, and the confidence following the exponential rather than the old straight line).

### The probe is hardware, and the firmware stopped assuming which

**Polarity — `moisture[i].invert`.** `sensorsReadIo()` did `100 - ADC_TO_PERCENT(...)`, one sign for the whole board. That is right for the capacitive v2 modules (their 555 output FALLS as the soil wets); a resistive divider does the opposite. The classifier never cared — both the calibration and the ordering gate accept either direction — but the number on the dashboard and in the stored history would have run backwards, and every chart with it. Default `true`.

**Power gating — `io.soilMoisture[i].powerPin` / `powerOn` / `settleMs`.** The difference between a resistive probe lasting a season and lasting weeks: two electrodes in wet soil with a DC potential across them are an electrolysis cell, the anode dissolves and the probe is scrap. Driving the module's VCC from a GPIO takes the duty cycle from 100 % to under 1 % at a 1 s period.

- **Coalesced.** Two probes on one MOSFET is the normal wiring, so every power pin is switched on, ONE settle delay is paid — the longest any probe asked for — and all probes are read.
- **The first conversion after power-up is discarded.** The input was floating a moment ago and the SAR capacitor carries charge from the previous channel.
- **`settleMs` is capped at 250.** The delay runs inside the 1 Hz io task, the same cooperative pump MQTT and TalkBack share. A probe needing more is one to read less often.
- **The pin is parked OFF in `sensorsSetup()`**, before it is ever driven on, for the same reason `relayPinsSafeInit()` is the first statement of `setup()`.
- **`validatePins()` checks it as an OUTPUT**, and a pin shared between two probes is not reported as a duplicate — that is the intended wiring.
- **Current is the operator's problem, and the UI says so.** An LM393 module with its LED draws around 20 mA, most of what one ESP32 GPIO should source; two want a small MOSFET. **Swapping a sensor now discards its model.** `MoistureProbeModel` identified a probe by pin and relay, and replacing a capacitive module with a resistive one changes neither — same hole, same pump — while inverting the transfer curve. Weeks of Gaussians would have carried over reporting a confident badge, and the separation gate would NOT have caught it: the bands stay well separated, they are simply the wrong bands. The identity now carries `sourceInvert` and `sourceTag`, a hash of `moisture[i].kind`. That free-text label is the manual lever — relabel a probe and its statistics are thrown away — and it is why `kind` is only written when non-empty, so an empty one does not discard a model on every save.

### Is there a sensor on the pin? One test, and only one

**Every obvious statistic fails.** 200 history records over 4.3 h with all three probes unplugged: means 87.2 / 61.1 / 50.2, sd 4.37 / 2.65 / 1.29, all mid-scale. **Not the value** (an earlier disconnected probe sat at 52.6, later 86.7). **Not the correlation between probes** — 0 and 1 track at +0.87, but real probes in one garden correlate just as hard. **Not the spread of those per-minute means** — that is what averaging removes.

What works is the spread **between CONSECUTIVE conversions**, measured live against the luminosity channel on the same ADC at the same moment: connected 0.02 var / 0.14 sd, three floating probes ~800–1990 var / 27.9–44.6 sd. Four orders of magnitude, with a control on the same chip. Soil cannot jump between one conversion and the next however fast it is watered; a floating pin does nothing else. **The threshold is 400 ADC counts**, and the same number separates every case measured or modelled:

```
watering, 1200 counts over 5 min at 1 Hz        4
connected probe, level sd 80 (white noise)    113
probe moved from soil to air by hand          204
---- threshold ----                           400
floating pin alternating rail to rail        3910
``` A single large step contributes `S/sqrt(N)`, which is why moving a probe by hand stays well under the line while continuous swinging does not.

**Three other tests were built and removed, because they accused working hardware.** With two probes connected and a third pin tied to 3V3, the rail/coupling/flatline checks reported `railed` on a healthy probe merely lifted out of the soil, `railed` on the real fault, and `floating` on a healthy probe sitting in wet soil — two false accusations out of three. A capacitive module in air reads full scale and is genuinely indistinguishable from a pin shorted to 3V3. **A detector that cries wolf on a good sensor is worse than one that occasionally stays quiet.** The statistic itself was changed for the same reason: it measured the spread of the LEVEL, and a healthy probe lifted from wet soil into air went 287 → 4095 counts and was reported `noisy` at sd 1889 — a watering does the same thing more gently, which would have made the most important event in the system look like a fault. **What it deliberately does not catch:** a disconnected pin that happens to sit quiet — this board has held one at variance 0.01, quieter than any connected probe. That is the price of not accusing working sensors; such a probe is still caught days later by the watering-response check and the separation gate.

### The two-point fallback

The rung below the model, and the only one an uncalibrated device has. Thresholds come from **two reference readings per probe**, because each probe has its own gain and offset and they cannot share a threshold. `config.json` carries a `moisture` array parallel to `io.soilMoisture`: `[{"dry": …, "wet": …, "relay": <index>}, ...]`.

- `moistureState(i)` asks the model first, then falls back to thirds of that probe's own span, and returns **an empty string while `dry == wet`**. An uncalibrated probe with no model shows no badge rather than a fabricated one, and `/data.json` only carries `state` when it is non-empty.
- **Ordering is not assumed anywhere, and the docs should not assert it either.** `include/core/config.h` says the air reading is the smaller number under the `100 - ADC%` conversion; an earlier version of this file said the larger. Neither was verified, and it does not need to be: both the calibration and the ordering gate accept either direction, which is why the disagreement never produced a bug.
- **A floating input is not "in air".** A disconnected probe reads whatever the pin floats to, which is not a usable dry anchor, and it is not a rail: measured 2026-08-24, an unplugged probe sat at **52.6** with variance 0.01, squarely inside the range wet soil produces.
- `scripts/moisture_calibration.py` runs the same analysis off-device and states which question the data can answer. It refuses to report a drying rate over a window shorter than three days, after an early run extrapolated a 0.2-day window into "704 points/day".

### The fit moved to the workstation, and it fitted nothing

`scripts/moisture_fit.py` (+ `moisture_stats.py`) fits the same parameters off the whole archive rather than the 24 h the board holds, and emits JSON in a dry run; the device keeps its incremental half, so a seed ages out on its own and is never a freeze. **Run against the live device and the full archive on 2026-09-03 it proposed NOTHING, and that is the honest answer** — every refusal named per probe, exactly as `/moisture.json` names its gates: zero watering events on either probe's own pump since the sensor change (`only 0 watering events, 6 needed`), and both probes still equilibrating after being handled that day (+20.74 points over two hours at +0.536/5 min on probe 0; +15.05 at +0.537 on probe 1), so an anchor read then would be precise and wrong.

Two findings from that run are worth keeping whatever happens next:

- **`moisture2` changed meaning on 2026-09-02 13:41 and nothing in the series says so.** The archive keys probes and relays POSITIONALLY, so deleting `Umidade Zona 2` renumbered everything after it. The archive states the relay half outright (the `relay`/`relayName` pair) and the probe half not at all, because moisture keys carry no name. **The tool therefore treats any seam as a hard boundary for every probe** — blunt and deliberate: `--since` can narrow that window and nothing can widen it.
- **This garden is also watered by hand, so the relay record is not a complete label source.** Between 12:14 and 12:25 on 2026-09-03 both probes rose together with no relay event anywhere near it. Every such rise is an unlabelled wetting the classifier scores "humid".

**`/moisture_model.bin` is deliberately NOT written from here**, and the five reasons in the `MODEL_FILE_DECLINED` comment were re-checked on 2026-09-16 once the device's own 60 s record could actually be read. See [The 60 s record](#the-60-s-record-what-it-unblocked-and-what-it-did-not).

### The 60 s record: what it unblocked, and what it did not

`scripts/history_export.py` collects `GET /history.json` into `backups/history.sqlite`, and `moisture_fit.py --history-db` fits from it — the change `MODEL_FILE_DECLINED` itself named as "the first thing to change". **The verdict on seeding did not move. It is still no**, and the score on the five reasons is now 1, 4 and 5 standing, 3 resolved, 2 **withdrawn as a mis-statement of what the two series are**:

- **Reason 2 was wrong, and it was the one this file leaned on.** It claimed the archive's `moistureN` is "the accumulator MEAN over one publish period" while "the device fits from its own 60 s history records", so averaging shrinks the variance a J is a ratio to. **Both series are the same statistic.** `historyTaskHandler()` writes `record.moisture[i] = g_soilMoisture[i].getAverage()`; `addContinuous()` publishes `moistureN` from the same `getAverage()` on the same accumulator, whose window is sized once in `sensorsSetup()` as `mqttPublishPeriodMs() / g_ioTaskPeriod` = **300 samples**. So the device's own 60 s record is a 300-second trailing mean read out every 60 s, and the archive is that same mean read out every 300 s — a **5× decimation**, not a smoothing, with the same marginal variance in expectation. What genuinely differs is the SAMPLE COUNT per class window (30 at 60 s against 6 at 300 s), and it cuts the **opposite** way: `weight` is a sum of per-sample confidences, so the archive under-counts it fivefold against `MIN_WEIGHT_PER_CLASS` — the 300 s path is HARDER to pass, not easier. The extra 60 s samples are heavily autocorrelated (consecutive records share 240 s of one window), so most of that weight is fictitious independence — which is fine, because it is the **same** fictitious independence the device's own trainer has, and matching the device's arithmetic is the whole point.
- **Reason 3 is resolved, exactly as predicted.** `consumedUntil` is the newest rising edge across every probe in the records that were scanned (`scan->consumeUntil` in `moistureModelTrain()`), and from the device's own records that is computable. `history_archive.consumed_until()` computes it, `--history-db` prints it, and **0 is correct precisely when no edge exists.** That was this garden until the first live collection: on 2026-09-17 the device's own record carried three edges on probe-mapped pumps and `consumedUntil` answered **1789560077 (2026-09-16 09:01:17)**, ignoring the two newer Reservatorio edges because no probe maps to that pump.
- **Reason 1 stands and is the binding one, and it is not a tooling problem.** It was written when both probes had **zero** watering events on their own pump. The first live collection (2026-09-17) found **1 and 2** — so the gate is still failed before any statistic is computed, but the number is no longer zero. **And the same run showed that the event count is the shallower problem**: with the labels those few events supply, `wet` comes out 0.07 points BELOW `dry` on probe 0 and 0.02 above it on probe 1, J = **0.252** and **0.008** against 4, and every one of 2787 records sits in the WET third of both probes' spans. Six waterings would satisfy the gate and not the physics. **What would change it is a zone that actually dries between waterings** — six edges on a pot that never leaves the wet end buys a confident model of nothing.
- **Reasons 4 and 5 stand unchanged**: the struct layout still has to be verified byte-for-byte against a file the device wrote (`/spiffs/moisture_model.bin`, never read), and a push is still upload-then-reboot on a board whose reset pulses every pump.

**Two thresholds in `moisture_stats.py` were rate-blind and one had to change.** `STEP_MIN_POINTS` = 5.0 is a delta *per sample* standing in for a *rate*, and until there was a second sample rate the two were the same number. `find_steps()` now takes the observed period and scales (`step_threshold()`), with a floor of 1.0 point; the 300 s archive path is byte-identical, because it passes the same 300 s. **The scaling buys AGREEMENT, not leniency** — a re-seating that moves 17.5 points over five minutes is one 17.5-point step at 300 s and five 3.5-point steps at 60 s, so the archive's unscaled number MISSES it on the finer record. `MAX_GAP_SEC` = 1800 was left alone and is now 30 periods rather than 6; that is noted, not fixed, because nothing has been measured at 60 s to fix it against.

**`--history-db` needs no device and no saved config.** Every collection run stamps the `(index → name, pin, relay)` binding it saw into the archive, so a seam is a **stated disagreement between two sessions** rather than the inference `usable_from()` has to make from the ThingsBoard archive's `(relay, relayName)` pairs. The rule stays blunt — the most recent seam of any kind bounds every probe — because the archive still cannot say which SLOT moved, only that the `io` block was edited.

### Drying IS a decay. It is the wrong decay, and its asymptote is not there

The question was worth asking and the prize was real: the `dry` anchor is an UPPER bound taken by carrying a probe into a different pot, and if drying approaches an asymptote then fitting it estimates the dry end without waiting for a zone to dry out. `scripts/drying_fit.py` asked. **The answer is no, and the reason is more useful than a yes would have been.** With `--stitch-seam`, ten genuine drying segments come out of the archive, 7.9 to 61.4 h long, five flagged CONFOUNDED because they begin within six hours of a probe being handled. **All ten were refused an asymptote.** *(The segment count is an argument of the run, not a property of the archive: the DEFAULT invocation treats the 2026-09-02 index seam as a hard boundary and finds **eight**, also all refused. Ten is the stitched run, and it is the one every number below comes from.)*

**Everything in this section was re-measured on 2026-09-04** after a code review found two bugs that corrupted the model comparison — see [what the bugs did](#the-gate-that-decided-nothing) at the end. The numbers that follow are the corrected run.

**There ARE two timescales, and only the fast one is real.** The two longest segments are described by `exp + linear`: a fast exponential of **tau 1.96 h** (`moisture1`, 61.4 h) and **1.93 h** (`zona3`, 36.3 h), on top of straight declines of **-2.86** and **-1.73 points/day**. Those two agree to 1.5 % and they are different probes in different pots. The two 8-hour segments after the 2026-09-03 hand watering give **1.49 h** and **0.27 h**, which is NOT the same number — so what is reproducible is that a fast component of order an hour exists, not its value. That pair matters anyway: nothing was touched there, only water was poured, so the fast limb is soil and not a probe settling. It is the drying counterpart of the absorption `tau` the firmware already estimates.

**The slow limb has no measurable curvature, which is why there is no asymptote.** Fit a single exponential to it and tau runs to **56.4 h** on the 61.4 h segment and **87.7 h** on the 21.9 h one: an exponential that long IS a straight line, and its "asymptote" is the line's intercept. Fit a double exponential and the second tau runs to **245 h** and **145 h** against those windows while buying an in-sample RMSE of 0.496 against `exp + linear`'s 0.491 — so `exp2` is not resolving two soil timescales, it is spending its second exponential on being a line and paying nothing for the privilege of also reporting an asymptote of 45.67. **`exp + linear` is in the model list for exactly this reason**: without it, `exp2` simply "wins" and its m_inf gets read as a physical number.

Three checks condemn the asymptote on segment [6] (`moisture1`, 61.4 h, the longest undisturbed stretch): the **95 % profile likelihood at n_eff reaches 0**, the floor of the scale — every asymptote fits; the **residual moving-block bootstrap** says [42.59, 71.35], and on segment [7] says [78.01, 79.12], **77x tighter than a profile that is unbounded**; and **trimming the first 12.2 h and refitting moves the asymptote 21.2 points** (67.7 → 46.5 → 50.6). **The bootstrap's narrowness is the trap**: it resamples around the fitted curve, so it measures noise and not model error, and where the model is wrong it reports ±0.5 for a number the profile cannot bound at all. It is printed only next to the profile, never alone. **AIC and BIC are computed and deliberately not obeyed.** Both assume independent residuals, and these are heavily autocorrelated — across the ten segments lag-1 rho runs **0.53 to 0.98**, and the segments worth fitting sit at the top of that range (the 61.4 h one reports 0.969, which is n_eff 11.6 out of 737 points) — so the effective sample size `n(1-rho)/(1+rho)` is a small fraction of the point count and the criteria cannot separate the models they claim to rank. They are reported at `n_eff` and the primary criterion is **held-out error**. *(An earlier version of this line said 0.92 to 0.99. That was never the tool's own range; `drying_models.py` already recorded 0.53-0.98 and this section had not been corrected to match.)*

**And asymptotes WERE identified — which is what proves the point.** Three of the ten single-exponential profiles come out bounded, and the clearest pair are the two 8-hour segments after the hand watering: tau 1.44 h with a 95 % interval of [71.19, 74.16], and tau 0.35 h with [74.66, 75.99]. **Every one of them is refused**, by a gate added after the tool passed one without it: the asymptote sits **within 0.3 points of the last reading**. Four segments fall to that gate, at +0.14, -0.29, -0.13 and +0.19 points. A decay watched to completion asymptotes at the level the pot settles at BETWEEN waterings, which the last sample already gave, and calling that "fully dry" would have written a humid baseline into `moisture[i].dry`. **On a partial decay, identified and informative are the same dial turned opposite ways** — that is the honest content of the whole exercise, and it does not improve with more of this data.

#### The gate that decided nothing

A code review on 2026-09-04 found two bugs in the fitting code, both confirmed by running it, and the audit that followed is worth more than the fixes.

- **`fit()` returned sorted time constants beside coefficients fitted against the UNSORTED order.** The refinement loop can walk `taus[0]` past `taus[1]`, after which sorting re-pairs every amplitude with the wrong exponential. `predict()` then draws a curve the fit never saw while `sse` describes one no longer reachable, and *everything* downstream reads the wrong one — residuals, the criteria, the held-out ranking, the bootstrap. Measured: one `exp2` fit stored sse 0.361 while `predict()` gave **10 618**.
- **The profile gate could be defeated.** `profile_asymptote()` fitted `level + amp*exp(-t/tau)` whatever the model was, so an `exp2` model was profiled against a family that could not reach its SSE. No level came out "inside", the bisection collapsed onto its anchor, and a nonexistent asymptote was reported as a **bounded interval of width 0.000** that did not contain its own point estimate. On a straight line: `[2.00, 2.00]` beside an estimate of 61.03.

**The verdict did not move: ten segments, ten refusals, no dry anchor, before and after** — every headline number reproduced to the digit, and only the bootstrap intervals shifted, by hundredths. **But it survived by luck, and that is the finding.** All ten are refused by a gate that runs BEFORE the profile: `model` five times, `uninformative` four, `degenerate` once, so **not one segment ever reached the profile gate** and the broken check decided nothing on this archive — it was never asked. *(**That sentence is no longer true of today's archive, and the correction is dated 2026-09-21.** The ThingsBoard record has since grown from ten segments to thirteen, and segment [13] — `moisture1`, 2026-09-14 19:29 → 09-17 11:42, 64.2 h — passes `model`, `degenerate` and `uninformative` with a non-degenerate 9.65 h tau and 0.1 points of trim drift, and is refused BY THE PROFILE. So the corrected check is now load-bearing on real data, which it was not when it was corrected. It was still refusing, and the ACCEPT path is still unreached.)* Correcting it flips three segments' winning profiles from `bounded` to unbounded (widths 46.08, 51.79, 75.41), **two of which had been reporting a width of 0.000**; each was saved by an earlier gate, not by the check the tool leans on. The mispaired-tau bug likewise inflated one `exp2` held-out RMSE from 0.890 to 2.507 — second place to last — without changing a winner. **So a tool whose headline gate was broken produced the right answer ten times out of ten, and nothing in its output said so.** The layered refusal is doing real work: the cheap physical gates caught everything before the expensive statistical one was consulted, which is the order they should be in — and an ACCEPT would have been the first output that actually exercised the profile gate, exactly what [the unverified list](verification-log.md) says about it. `--self-test` now asserts the round trip that was missing (`predict()` must reproduce the fit's own `sse`, every model, every curve) and that no straight line yields a bounded, precise asymptote; both are red against the code as it stood.

**The 86.4 % linear finding above is not contradicted**: that is a NET rate over 29.7 days containing waterings, each putting the level back up, on an earlier probe generation, where -1.7 to -2.9 points/day are drying-only rates between them. **Weather does not rescue it either** — against the `exp + linear` residual the device's own temperature reaches r² 0.128 on one segment and 0.040 on another **with the sign reversed** (+0.36 against -0.20), luminosity 0.011-0.089, air humidity 0.019-0.068, where a real driver would push both probes in one garden the same way; a separate reason from the one that keeps ET0 disabled, pointing the same direction.

**What the archive cannot answer at all:** the observation that started this — a probe holding an apparent plateau for about three minutes on the evening of 2026-09-03 before resuming its fall. That evening is stored at the **300 s** publish period, 25 points over 2.1 h, so a three-minute feature is below the sampling; the tool flags any tau under two sample periods as UNRESOLVED rather than reporting it. Answering it needs `GET /history.json` at 60 s, or a deliberate 1 Hz capture. **`scripts/history_export.py` now collects that record** (see [The 60 s record](#the-60-s-record-what-it-unblocked-and-what-it-did-not)) — but it does not answer this question retroactively: the 2026-09-03 evening was never in a collected buffer and is gone from the device. A 60 s archive helps the NEXT such feature, and even then only down to two 60 s periods; a three-minute plateau is right at that floor, so the 1 Hz capture is still the design that would settle it.

**`drying_fit.py --history-db` now READS that 60 s record** (2026-09-21), so the UNRESOLVED floor is **2 minutes rather than 10** and this paragraph's arithmetic is no longer hypothetical — a three-minute plateau would be reported rather than flagged. It still cannot reach back to 2026-09-03. **And a limit underneath the sample rate has become the binding one:** `drying_models.TAU_MIN_FRACTION` puts the fit's tau grid at span/500, which on a 40 h segment is 4.8 minutes, so on a long 60 s segment it is the GRID and not the record that a short tau meets first. Left alone deliberately — it is a fraction of the window rather than a count of samples, and widening it would move every 300 s answer in this section for a resolution no segment has yet asked for.

**Nothing was added to the firmware, and that is the result.** No C++ changed, no flash spent, `moisture[i].dry` still holds the hand-measured 53.1. The fast `tau` is the one thing worth having and it is not this question's answer: nothing on the device consumes a drying time constant, and the archive's 300 s samples are the wrong record to fit one from — the device's own 60 s history is, the same conclusion `moisture_fit.py` reached about seeding a model. **What would change the verdict:** one zone actually drying out, so the curve bends where the asymptote lives instead of being extrapolated to it. What would NOT change it is more days of the same — the profile is flat because the curvature is absent, not because the record is short.

### A zone DID dry out, and the verdict did not move — for the opposite reason

**2026-09-21.** `Umidade Zona 4 (Arranjo)` on the S3 carrier fell **43.4 → 9.0 over 71.4 h** with nothing watering it, in the device's own 60 s record, and `drying_fit.py --history-db` produced **no dry anchor**. Six segments across four probes, six refusals. The full measurement is in [What has actually run](verification-log.md); what belongs here is what it does to the paragraph above.

**The prediction was half right and the important half was wrong.** The curve bent — and it bent AWAY from an asymptote. Slope over successive fifths of the fall: **−3.70, −4.43, −11.46, −23.07, −18.04 points/day**. A drying pot on this probe does not decelerate into a floor, it **accelerates**, and it was still falling at about 18 points/day at the last sample. So an exponential fitted to it puts m_inf **below zero** — −11.85 on the 26 h piece, −3.02 on the 42.8 h one, **−121.49** on the two joined by hand — and both Arranjo segments are refused by the FIRST gate, `model`, because `linear` wins the held-out extrapolation outright. The profile never got a chance to be flat.

**This is a stronger result than another refusal, and it should be read as one.** The whole premise of this section was that a partial decay is ill-conditioned but a complete one would not be. A complete one arrived and the premise failed at a level below conditioning: **there is no asymptote in this fall to be ill-conditioned about.** More drying will not fix that, and neither will a longer record.

**A physical reading, offered as interpretation and not as measurement.** These are resistive dividers read as `100 − ADC·100/4095` with `invert`. As soil dries, conduction falls and the divider walks toward the open-circuit rail, which is 0 on this scale — so the *physical* asymptote of a resistive probe in drying soil is plausibly the rail itself, not a soil-moisture floor, and the profile's refusal to exclude anything down to 0 is then correct rather than merely uninformative. The last reading of the segment, 9.01, is **ADC 3726 of 4095**. Nothing here measures that mechanism; what is measured is the sign of the curvature and where the reading sits on the scale.

**What this costs the original prize:** `moisture[i].dry` still cannot be got by extrapolation, and now for a reason that does not go away with patience. The remaining honest routes are the two that were always there — read a probe in a pot that has genuinely stopped changing, or accept `dry` as an operating bound and say so, which is what Zona 1's 53.1 already is.

## Temperature IS a plausible confound. This archive cannot measure it, and the reason is the design

These probes are **resistive**, soil conduction is **ionic**, and ionic conductivity rises about **2 %/K** — so the same water reads differently warm and cold. Pushed through the measured transfer curve (wet ADC 1002, dry 1920, under `100 - ADC%` with `invert: true`), that predicts **+0.37 points/K at the wet end and +0.50 at the dry end** of *soil* temperature, and a **positive** sign: warmer soil conducts better, so it reads **wetter**. A falsifiable prediction, not a fitted parameter, and it is the one `scripts/moisture_thermal.py` tests against. **The overnight natural experiment is degenerate, and that is the first finding.** Between 20:00 and 06:00 the soil moves slowly while air temperature swings — but air temperature also falls almost monotonically, so it *is* elapsed time: measured across the ten nights, **r(T, t) = -0.81 to -0.97**, VIF 2.9 to 17.7. A falling temperature and a falling drying trend are the same regressor to within a few per cent, and the coefficient that comes out is whatever the trend model declined to absorb. **All four in-soil probe-nights are refused on collinearity alone** — `moisture1` +0.016 and `moisture3` +0.044 on the night of 09-01 (VIF 11.7), `moisture1` +0.120 and `moisture2` +0.163 on 09-02 (VIF 31.3). Four positive slopes, the direction the physics predicts, and not one of them is a measurement.

**Longer windows ARE identified — and the placebo kills them.** Over a full day the covariate cycles while drying is monotone, so the design is no longer degenerate. Four clean in-soil stretches, none straddling the seam, none disturbed:

| window | orders 1/2/3 | best lag | verdict |
|---|---|---|---|
| `moisture1` 09-01 04:00..09-02 13:00 | -0.058 / -0.101 / -0.007 | 4 h, +0.088 (r² 0.18) | accepted, **wrong sign**, placebo 1/7 |
| `moisture3` 09-01 04:00..09-02 13:00 | +0.043 / +0.016 / +0.062 | **4 h, +0.149 (r² 0.38)** | placebo **6 of 7** |
| `moisture1` 09-02 14:00..09-03 11:00 | +0.022 / +0.045 / +0.078 | 4 h, -0.182 (r² 0.21) | placebo **8 of 8** |
| `moisture2` 09-02 14:00..09-03 11:00 | +0.030 / +0.021 / +0.038 | **4 h, +0.204 (r² 0.11)** | placebo **8 of 8** | **The pattern is genuinely suggestive and it still does not survive.** Three of four slope **positive** at **+0.02 to +0.08 points/K**, the predicted sign and the right order of magnitude for a damped air proxy; and **all four peak at a 4-hour lag**, three positive — the signature a soil-thermal effect should have and a spurious trend residual should not. Then the **placebo** — the same window regressed against **another day's** temperature at the same clock time, keeping the diurnal shape and destroying the identity — explains as much on **6 of 7 and 8 of 8** shifts. Any smooth day fits a smooth drying residual, whoever's day it was: `--self-test` pins that with a series carrying *no* temperature dependence at all, scoring slope +0.268 at partial r² **0.874** and refused because 5 of 6 foreign days do as well. Only the window with the *wrong* sign passes the placebo, at 1 of 7. **The magnitude is not stable either**: across trend orders 1..3 on those same windows it moves by **3x to 14x** (-0.007..-0.101, +0.016..+0.062), and on longer spans including the 09-03 handling the **sign** flips outright. The data did not change; the trend model did.

**The tool's own default run: 2 of 31 windows produce a coefficient, and both are evidence against.** `moisture_fit.py` screens every window with the detectors it already owns — railed, stuck, a watering inside it, an unexplained step, an equilibrating probe — then applies collinearity, sign stability, the placebo and cross-probe sign agreement. The two survivors are both the night of **2026-08-26**, at **-0.098** and **-0.411 points/K**, the sign ionic conduction does **not** predict, and both fall in the stretch this file records all three probes as **unplugged**. A floating ADC pin's leakage tracks temperature hard: real physics, wrong physics. **The archive carries no probe-health verdict**, so the tool cannot refuse them for the reason they are actually wrong and says so in its own report; `moistureNSd` (shipped 2.9.1, never published) would close that gap. **This does not contradict the drying analysis; it explains it.** `drying_fit.py` asked whether temperature explains the *shape of a decay within* a segment and got r² 0.128 at **+0.36** on one and 0.040 at **-0.20** on another. Same instability, now named: the coefficient is set by the flexibility of the trend, not by the garden. The questions stay different — a bias near-constant *inside* a segment is invisible in that residual and would still ruin comparison *across* days — and the answer to both is that this record cannot separate them.

**Two things the archive CAN bound.** The 2026-09-03 sun patch put **16.67 K** through the DHT in two hours; `moisture1` moved 1.49 points and `moisture2` 0.42, partial slopes **-0.027** and **-0.008** — so the module's own electronics contribute **≤ 0.03 points/K**, and any real confound is in the soil, not on the board. And **the shipped calibration is not threatened**: the wet-anchor window averaged 29.11 °C against the dry anchor's 28.42 °C, **0.69 K apart**, which at the 0.2 points/K upper bound is **0.14 points of a 20.4-point span** — 0.7 % of a 6.8-point badge band.

**The thermometer makes a correction impossible even if the coefficient were known.** This DHT follows **0.346** of the outdoor swing while its dewpoint follows 1.052 — it is behind thermal mass ([Evapotranspiration](evapotranspiration.md)) — and what a probe responds to is *soil* temperature, damped and lagged again. A coefficient fitted here is fitted to a proxy of a proxy, with an attenuation nothing in this archive can measure, and it describes **one enclosure at one mounting**. This file already records a sensor geometry changing undetected (the 2026-08-28 luminosity break), and **nothing on the device can detect the next one**.

**So the tools GATE and never correct**, the conservative option and the one that matches how this repo treats a seam. Two admission checks, both scored against `WORST_CASE_SENSITIVITY` = 0.2 points/K — an **upper bound** the archive supports, not an estimate it does not:

- **`anchor_thermal_refusal()`** — a two-point `dry`/`wet` pair whose anchors were measured more than a tenth of their span apart in thermal terms. Inert at 0.69 K; it fires on a dry anchor read at a 45 °C afternoon against a wet one at a 25 °C dawn.
- **`class_thermal_refusal()`** — the same for the classifier. The labels come from watering events, so a pump on a schedule puts every WET sample in one part of the diurnal cycle and every DRY sample in another, and Fisher's J then separates **the clock**. Exercised in `--self-test` by a pump that always runs at the hottest hour. **Correcting was considered and rejected for a specific reason**: a correction is applied to *every* reading including the two that define the badge, so a wrong coefficient moves the band boundaries systematically rather than adding noise — strictly worse than the honest silence `moistureState()` already keeps.

**The lag scan is reported and deliberately not gated on**, and it is the hardest call here. All four clean windows peak at 4 h, which is what soil thermal lag looks like and what a spurious trend residual should not do. But maximising over nine lags is nine chances at a peak; the fourth window peaks *negative* there while the others peak positive; and the placebo beats the peak too. A statistic allowed to pick its own lag has stopped being a test — so it is printed as evidence, marked `(reported, not tested)`, and no verdict rests on it.

**Should temperature ever be a term in the ON-DEVICE model? No — and not for the reason time-since-watering is excluded.** That one is a shortcut the model could exploit; this is a confound one would want to remove, so the objection is not circularity but **failure coupling**. The device already carries `dhtErrorRate` and a plausibility gate precisely because this DHT misreads, and it has been cooked by direct sun for 38 minutes on record. A moisture verdict that consumed temperature would let a thermometer fault become a soil fault — and the badge would stay confident, exactly the failure the separation gate and the watering-response check exist to catch. The correction would also be worth **≤ 1.0 point** against a 6.8-point band at the measured 5.09 K diurnal spread of hourly means: a sixth of a band, bought by coupling the one output an operator acts on to the worst-sited sensor on the board. **If it is ever done, it belongs off-device** — in `moisture_fit.py`, against a soil thermometer, with a coefficient somebody measured.

**What would change the verdict:** a **soil** temperature probe, which makes the covariate the right one and breaks the collinearity in one move; or a deliberate bench experiment — one pot, constant water, a controlled temperature ramp — which is the only design here that varies temperature without varying time. **What would NOT change it is more days of this**: the collinearity is a property of nights, not of the record length.
