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
