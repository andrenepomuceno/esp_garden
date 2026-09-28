# Reference evapotranspiration (ET0)

Off by default (`et0.enabled`), it has **never run on hardware**, and on the
device it was written for the fit says **do not turn it on**. That last part is
the point of the section.

ET0 is how much water a reference grass surface lost in a day, in millimetres —
the number that says what a garden actually needs. **Hargreaves-Samani** gets
there from the day's temperature extremes and extraterrestrial radiation, and Ra
is a closed form in latitude and day of year, so nothing on the device ever
calls a weather API. FAO-56 Penman-Monteith is more accurate but needs wind,
which this board does not have.

The estimate is published once a day as an **event**, the moment the local day
closes — it updates once in 24 h, so putting it in the five-minute payload would
restate it 287 times a day — and it carries `et0TempMin`, `et0TempMax` and
`et0Hours` beside `et0`, so the archive holds the evidence and not just the
answer. `/data.json` shows it under `Status.ET0`, absent until a complete day
exists.

**A day must be watched before its extremes mean anything.** Tmin lands near
sunrise and Tmax mid-afternoon, so a device that booted at noon still has a min
and a max and they look entirely ordinary — unlike a mean, an extreme carries
nothing that reveals half the day is missing. A day with fewer than 22 of 24
local hours is refused and logged.

## Validate before enabling — the fit can say no, and here it did

```bash
python scripts/et0_fit.py                          # reads postalCode from config
python scripts/et0_fit.py --lat -15.7885 --lon -47.9297
```

It geocodes the device's `postalCode`, pulls the public daily record for that
spot from Open-Meteo, and scores the device's own Hargreaves-Samani estimate
against the station's FAO-56 Penman-Monteith one. It checks three things in
order and refuses to skip the first:

1. **How much rain fell in the archive's window.** A rain model fitted on a
   window with no rain in it is a classifier with no positive examples.
2. **Whether the thermometer is in free air at all.** Hargreaves-Samani is
   driven by the diurnal RANGE, so a sheltered sensor breaks it in a way no
   scale factor repairs — it removes the bias and leaves the estimate
   uncorrelated with what it is estimating.
3. **Whether a fitted correction survives leave-one-out**, or merely fits the
   handful of days it was measured on.

On this garden's archive it answered: 2.1 mm of rain in eleven days, a sensor
following 35 % of the outdoor temperature swing, and a scaled estimate that
loses to a constant. So `et0.scale` ships at **1.0** — the textbook formula,
nothing fitted — and `et0.enabled` ships **false**. A `scale` far from 1.0 is a
siting fault wearing a calibration coefficient; the firmware logs a warning when
it sees one.

## Evapotranspiration: the fit said no, and that is the result

**Nothing here has run on hardware, and unlike the cloud model it is not merely unproven — it was measured and it failed.** `et0.enabled` defaults to false because `scripts/et0_fit.py`, run against this device's own archive and a public station, could not show the estimate beating a constant. The model ships anyway, correct and host-tested, because the failure is in this board's SENSOR SITING and not in the arithmetic: fix the exposure, or put the firmware on a board in free air, and the same code becomes useful. **What must not happen is the number being believed today.** Everything below is a measurement on the ARCHIVE against Open-Meteo.

### Where this garden is, and how that was settled

`postalCode` `70675-506` → ViaCEP → OSM Nominatim way **125381662, "QRSW 5"**, at **-15.7885, -47.9297**. Nominatim's `postalcode` search returns nothing for Brazilian CEPs and the verbatim `logradouro` misses too, so `et0_fit.py` strips the `Quadra `/` Bloco ` affixes before querying; without that strip it falls back to the neighbourhood centroid 1.3 km away, without the neighbourhood to the city 12 km away. **A wrong geocode is a silent error** — nothing downstream looks wrong when the latitude is off, the numbers are simply for somewhere else. **The public record is a model analysis, not a thermometer in this garden.** Open-Meteo snaps to a grid point **4.6 km away**, and it is revised: two fetches an hour apart reported 2026-08-24 as 1.2 mm then 0.9 mm, moving the eleven-day total 2.4 → 2.1 mm and the count of days reaching 1 mm from one to zero. Anything fitted tightly to these numbers is fitted to a revision.

### Rain: there is none, so nothing here models it

Eleven days at the grid point in Brasília's peak dry season total **2.4 mm, with ONE day reaching 1 mm**. **No rain model was fitted and none should be.** One marginal wet day is not a positive class, and a rain classifier trained on it is the same defect that blocks the moisture classifier here — a model with nothing to learn from that returns confident answers anyway. `et0_fit.py` prints this table first and refuses in its own output.

### The finding that decided everything: the thermometer is not in free air

Matched hour by hour against the station over 235 hours: the device runs **+5.7 K warm before dawn and -1.7 K cool at midday**, and the fits are

```
device T  = 0.346 * station T  + 18.98   r = +0.722
device Td = 1.052 * station Td +  3.61   r = +0.909
```

**The device follows 35 % of the outdoor temperature swing while its DEWPOINT follows 105 % of the outdoor dewpoint.** That pair is the whole diagnosis: dewpoint is conserved when air is merely heated or cooled, so a sensor tracking it at slope ~1 is breathing the same air mass as the station, while one tracking TEMPERATURE at 0.35 is sitting behind thermal mass — an enclosure, a roof or an indoor spot, not a screened instrument. **This is fatal to Hargreaves-Samani specifically.** HS has exactly one mechanism: `sqrt(Tmax - Tmin)` stands in for solar radiation. This board's diurnal range is **5.7 K mean against the station's 11.0 K**, and its range correlates with the real range at **r = +0.147 over 8 days**. The proxy is not merely biased, it is close to uninformative, and a model whose only mechanism has been disconnected is not repaired by scaling its output. *(The +3.6 K dewpoint offset — the device reads genuinely moister than the grid point — is not diagnosed here and nothing depends on it.)*

### The validation numbers

Complete days only; a day needs 22 of 24 local hours, which refuses 3 of the 11 (including the board-swap day at 6 h).

| predictor | n | mean vs PM | bias | MAE | RMSE | r |
|---|---|---|---|---|---|---|
| **HS(station T)** vs station PM | 8 | 4.46 vs 4.99 | -0.53 (**-10.6 %**) | 0.83 | 1.10 | +0.774 |
| **HS(device T)** vs station PM | 8 | 3.88 vs 4.99 | -1.11 (**-22.3 %**) | 1.86 | 2.05 | **+0.050** |
| HS(device T) vs HS(station T) | 8 | 3.88 vs 4.46 | -0.58 (-13.0 %) | 1.32 | 1.47 | +0.051 |

The first row is the ceiling and it is the reassuring one: **with a correct thermometer this implementation sits 10.6 % under FAO-56 Penman-Monteith**, ordinary HS behaviour in a windy semi-arid dry season. The maths is right. The second row is the device, at **r = +0.050** — no relationship at all. **A fitted scale factor was tried and refused by cross-validation**: the bias-removing scale is ×1.286 (LOO spread ×1.208..×1.492), LOO RMSE of the scaled estimate **2.56 mm/d** against a constant climatology's **1.31 mm/d** — the null model wins. So `et0.scale` ships at **1.0** and `et0.enabled` at **false**. The key exists because a correctly sited device deserves the lever; the default is the measurement.

### One 38-minute sun patch moved a day by 120 %, and that is the estimator

2026-09-03 is in the table at **device Tmax 45.04 °C against the station's 29.6** — HS 7.35 mm against PM 4.57. Drop that one day and the same estimate scores r = **+0.911** instead of +0.050. Eight days is small enough that one day owns the answer. **It is not a bad sample and it was not filtered out.** The luminosity channel hits **96 for 27 minutes**, a level reached on no other post-08-28 day (the rest ceiling at 72-76); temperature rises and falls with it over 38 minutes, humidity mirrors it, and **both soil probes are flat to 0.8 points throughout** — so nobody was handling the hardware. A patch of direct sun reached the sensor package at one solar azimuth; it was the only time in the whole archive the device exceeded 35 °C. The lesson is about the estimator, not the sample. **A daily maximum has no averaging in it**: 38 minutes out of 1440 more than doubled the day's ET0, where the same excursion moves a daily mean by 2 %. `test_evapotranspiration` pins this with `test_a_single_short_excursion_owns_the_whole_day`, so anything that later tries to "clean" the extremes has to argue with a red test — and this repo does not clamp real readings away, which is why the DHT humidity floor was lowered to 5 % a commit earlier.

### A correction to this file's own humidity claim

The DHT-gate entry says the 261 sub-20 % humidity points are real dry-season readings. **243 of them are; 18 are not.** The 243 are 2026-09-01, 13:03-17:40, device 31.7-34.2 °C, ambient — the station's daily mean RH that day is 31 % and its ET0 the window's highest at 7.21 mm, and the 15.00 % minimum is here, so lowering the floor to 5 % rests on these and stands. The 18 are 2026-09-03, 15:20-15:38, inside the sun patch, device 41.3-45.0 °C: the dewpoint drops 17.1 → 11.5 °C and recovers, which pure heating of a fixed air parcel cannot do — the humidity element was being cooked and those readings measure nothing. **The conclusion does not move; the attribution does.**

### What it costs on the chip

Measured from `espgarden2`'s build: `evapotranspiration.cpp.o` **1 609 B flash**, `et0_model.cpp.o` 1 756 flash + 8 data + 64 bss, resident state **72 B RAM**; whole change 1 247 157 → 1 254 501 = **+7 344 flash, +72 static RAM**, taking the app slot 70.5 % → **70.9 %**. The gap between the two translation units and the total is the `JSONVar` event, the `/data.json` row, the config parsing and the `String` formatting — the plumbing costs more than the physics, as for the cloud model. **No task was added**: it rides the 1 Hz io task doing one float comparison per tick. **It is published once a day, as an EVENT** — riding the periodic payload would restate it 287 times a day — carrying `et0` with `et0TempMin`, `et0TempMax` and `et0Hours` beside it, the evidence stored with the answer.

### What the data could not settle

- **Whether the sensor is indoors, in a box, or under a roof.** The statistics say "behind thermal mass" and cannot say which. Somebody has to look.
- **Whether a correctly sited DHT11 on this board would work.** The station-fed row says the formula and the location are fine; nothing says what a ±2 °C sensor in a proper screen would score.
- **Anything seasonal.** Eight usable days at the end of one dry season, with almost no cloud variation for HS's premise to track — the STATION's own range correlates with its own ET0 at only r = +0.392 here.
- **The 22-hour coverage gate's number.** Physics says both turning points must be inside the day; it does not say 22. The gate demonstrably refuses the 6-hour board-swap day, whose 3.43 K range was pure artefact, and admits everything else. That is the only evidence for it.
- **Whether the +3.6 K dewpoint offset is the garden or the sensor**, and **a second board** — every number here describes one DHT11 at one mounting, and the archive already contains one undiagnosed geometry change.
