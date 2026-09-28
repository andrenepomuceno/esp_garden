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
