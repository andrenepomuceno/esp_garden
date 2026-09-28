# Cloud cover

Off by default (`cloud.enabled`), and it has **never run on hardware**.

The luminosity channel is turned into a sky state by dividing each minute's mean
by a **clear-sky reference for that time of day**, giving a clearness index
`k`. Above 0.85 is `clear`, below 0.50 `overcast`, in between `partly cloudy`,
with a +-0.05 hysteresis band and a five-minute dwell so the badge does not
flap. It appears on `/data.json` as `state` on the luminosity input — absent,
not empty, whenever the model has no answer.

Separately, how far `k` moves from minute to minute opens and closes a
**cloud-transient episode**, published as an event the moment it happens rather
than sampled into the five-minute payload, because a cloud edge is shorter than
the payload period. The period's mean step is published as `cloudVariability`.

## The reference is this device's own history, not solar geometry

```
python scripts/cloud_fit.py            # fit, report, write the header
python scripts/cloud_fit.py --dry-run  # report only
```

It reads `backups/telemetry.sqlite` (see `scripts/tb_export.py`) and writes
`include/core/clear_sky_table.h`: 144 ten-minute bins holding the 98th
percentile of everything measured in each, which is 288 bytes of flash.

This device's luminosity peaks around **15:00 local**, three hours after solar
noon, because something shades it in the morning. A solar-position model would
call that cloud every morning for ever, which is why the reference is measured
rather than computed — and why it describes **one sensor at one mounting in one
season**.

**Re-run it after moving the sensor, and read the regime table it prints.** The
archive already contains one mounting change, on 2026-08-28, and the fit
excludes everything before it. Nothing on the device can detect the next one.

## Cloud cover: an empirical clear-sky reference, and what the data could not settle

**Nothing here has run on hardware.** The model is fitted, host-tested, builds in all five envs and is **off by default** (`cloud.enabled`). Everything below is a measurement on the ARCHIVE, or arithmetic from one. Two sources, exactly as the moisture badge ladders: the clearness index `k = measured / clearSkyReference(time of day)` turned into `clear` / `partly cloudy` / `overcast`; else **nothing** — outside the fitted daylight window, and whenever `cloud.enabled` is false, `/data.json` omits `state` rather than showing a badge it cannot support.

### The archive contains TWO sensor geometries, and that is the biggest finding

Between **2026-08-27 and 2026-08-28** the luminosity channel stepped: first light moved from 06:16-06:19 to 06:36-06:51, and each hour's maximum fell by a factor depending only on the time of day (hour 7 ran 2.20-2.82 × the fitted envelope before the break and 0.54-1.03 after; hour 16 barely moved, 1.07-1.17 → 0.92-1.01). **That is not weather.** The same time-of-day-locked attenuation repeats across six days with nothing in common — 31 % and 70 % RH, clear afternoons and cloudy — and the dim mornings are SMOOTH (minute-to-minute |delta| p90 0.25-0.99 against 0.98-2.54 on bright ones). Fog at 31 % RH does not exist, and a smooth ramp to a low ceiling is a shadow. Something began shading the sensor's morning, or it was moved, around the maintenance window that also produced eleven watchdog reboots on 08-28. **What this costs:** the usable archive is **six daylight days**, not ten, and the morning half of the envelope rests on the two clearest of them. `scripts/cloud_fit.py` prints that table on every run for exactly this reason — it is how the NEXT geometry change becomes visible, and nothing on the device can detect one.

### Why the reference is empirical and not astronomical

This sensor peaks at about **15:00 local, three hours after solar noon**, and its 07:00 reference is 20 % of its 15:00 one. A solar-position model would attribute all of that to cloud, every morning, for ever — geometry wearing a weather label, the same failure as a classifier that reads the pump schedule.

The reference is the **98th percentile of every reading in each 10-minute bin**, smoothed once with a (1, 2, 1) kernel and **interpolated between bin centres**.

- **144 bins of 10 minutes**, chosen against 5 / 15 / 20 / 30-minute alternatives for the flattest per-bin median k (sd 0.106); 288 bytes.
- **A moving maximum was tried first and withdrawn.** It drags the brightest sample in a bin up to a bin earlier, and on a morning ramp climbing 1.6 points a minute that shifts the whole reference: with it, the day's last bin had a median k of 0.31; without it, 0.48.
- **Interpolation is not decoration.** At that ramp rate, treating a 10-minute bin as a constant biases every reading in it by up to eight points — larger than the gap between two of the three states.
- **The daylight window is the run of bins around the peak whose reference is at least a quarter of it** — fitted at **07:40-17:39 local**. Below that the ratio stops meaning anything, and this garden's artificial light reaches 13-14 points on its own between 19:30 and 21:30, which a plain value floor would have let straight in. Fitted on 2026-08-28..09-03: peak **95.88 %** at 15:20, k p5/p50/p95 = **0.389 / 0.733 / 0.995**, and **3.97 %** of readings sit above the reference — which is the point. It is an envelope, not a ceiling.

### The thresholds are on the SENSOR'S scale, not the solar literature's

`k` here is not an irradiance ratio: an LDR read as a fraction of full scale compresses irradiance, so this archive's overcast day sits near k 0.52 where a true irradiance ratio would put it near 0.2. Borrowing the literature's K_t bands would have called every ordinary day overcast. **clear** is `k >= 0.85` (four settled fine days under the OTHER geometry, fitted with their own envelope, sit at k p25..p95 = 0.83..1.00); **overcast** is `k < 0.50` (the one overcast day spent 41.5 % of its daylight below it, the three brightest post-break days 0.3-3.9 %); hysteresis ±0.05 with a 5-minute dwell (state changes 14/day → 2-10/day, since every flap is a published datapoint); EWMA alpha 0.30 on k. **The two internal boundaries are the weak part and it should be said plainly** — six days, one of which supplies all the overcast evidence. What would move them is a genuinely overcast and a genuinely clear day under the CURRENT geometry.

### The transient is an event, and it had to be

A cloud edge lasts seconds to minutes; the periodic payload is built every 300 s. [Sampling vs events](telemetry.md) says that must be an event, so it is an **episode**: one message when it opens, one when it closes, carrying the length nothing else could reconstruct. Per-minute publishing would have been ~500 datapoints a day on a budget just cut by 83 %.

- The statistic is an **EWMA (alpha 0.30) of |dk| between consecutive minutes**: enter above **0.070**, exit below **0.035** held for **3 minutes**.
- **|dk| and not |d(reading)|.** Dividing by the reference removes the diurnal ramp. Measured: over 5-minute windows with a range of 5 points or more, a monotonic ramp and a flickering sky are **indistinguishable by the spread of the level** — both sd 3.2 — while the step statistic separates them 1.10 against 3.61. `probe_health.h` records making exactly this mistake once, on exactly this kind of signal.
- **It goes quiet in settled weather, which is the property that matters.** Replayed against the four fine pre-break days with their own envelope: **0, 1, 1, 0 episodes a day.** Against the six fitted days: **3, 4, 5, 6, 7, 7** — about 14 events a day at worst, 0.3 % of the daily budget.

### The 300 s publish period, measured

Resampling the 60 s archive into aligned 300 s windows inside the daylight window (619 windows), against a ground truth of "k moved by 0.10 or more inside this window" (18.6 %), each detector thresholded to 1 % false positives: the difference between consecutive 300 s means — **what shipped** — recalls **5.1 %** (r 0.455); the within-window spread of the LEVEL 86.1 % (r 0.958); the within-window mean |dk| — **what was added** — 80.0 % (r 0.958). **The mean alone loses nineteen transients in twenty**, so `cloudVariability` rides the periodic tick: it is a level, and a level is what the third mechanism in the sampling table is for. It costs **+288 datapoints/day** on a ~4 320/day base (**+6.7 %**). The level spread scores marginally higher only because this ground truth is itself a within-window quantity; across times of day it cannot tell a ramp from a flicker at all. **Two structural limits.** The device sees 300 samples per publish window where this test saw 5, so the recovered recall is a **lower bound**; and now that the device publishes at 300 s, **the transient thresholds can no longer be re-fitted from the archive** without temporarily dropping `mqtt.publishSec` back to 60.

### What it costs on the chip

Measured from `espgarden2`'s ELF symbol table, not estimated: `g_clearSkyTable` **288 B flash**, `g_cloudParams` 48, the pure classifier 873, the Arduino half 1 032, resident state **100 B RAM**; whole change 1 242 585 → 1 246 489 = **+3 904 flash, +136 static RAM**. **No task was added** — it rides the 1 Hz io task and 59 ticks in 60 only add to a running sum, which matters because `addTask()` silently drops past `CRITICALTASKSCHEDULER_MAX_TASKS`.

### What the data could not settle

- **Whether the 2026-08-28 break is a moved sensor or a new obstruction.** It is certainly geometry; which one needs somebody standing at the garden.
- **The overcast threshold**, resting on one overcast day; and **a genuinely clear day under the CURRENT geometry** — the best fitted day has a daylight median k of 0.913, so the envelope's morning is anchored on two days, not six.
- **Anything seasonal.** Ten days at the end of one winter, six usable. Day length and solar elevation move underneath the table and nothing re-fits it automatically.
- **The last bin of the day.** 17:30-17:39 has a fitted median k of 0.48 against 0.73 across the window, so the model leans toward `overcast` in the final ten minutes; six days cannot separate four genuinely cloudy late afternoons from a reference set too high.
