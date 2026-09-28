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
