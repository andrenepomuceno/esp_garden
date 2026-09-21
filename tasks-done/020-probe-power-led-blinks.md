# 020 — Does GPIO 14 actually stay up while `powerAlways` is true?

**Opened** 2026-09-18. **Board** `espgarden-s3` (b580), firmware 2.19.0.
**Needs** somebody at the bench with eyes on the probe module, or a meter.

## The contradiction

`io.soilMoisture[0].powerAlways` provably changes behaviour: with it true Zona 1
reads **0.00 flat** (ADC 4095, `var` 0.00, `stepSd` 0 across 38 samples), with it
false it reads **44.1** at `var` 0.01-0.29. Three reboots, both directions.

So `moisturePowerDown()` is skipping pin 14 — the coalescing rule in
`include/core/probe_power.h` is doing its job, and the flag is parsed.

**But the operator reports the module's power LED BLINKING**, which is a 1 Hz
switch, which is exactly what a held-high pin should make impossible. He did not
watch it during the always-on window, so it is not known whether it blinks there
or only while the flag is false.

**Nobody has put a meter or a scope on GPIO 14.** "The pin stays up" is an
inference from a reading, not an observation of a pin.

## What to do, in order of cost

1. **Look at the LED with `powerAlways` true.** One glance settles it.
   - Solid → the pin holds, and the 0.00 is the probe saturating. The
     polarisation story in CLAUDE.md stands as an inference.
   - Blinking → the pin does NOT hold, the mechanism recorded in CLAUDE.md is
     wrong, and the 0.00 has another cause that has to be found.
2. **Meter on `SOIL_PWR_EN` / GPIO 14** against GND, both flag states.
3. If the pin does hold: sweep `settleMs` 10 → 250 with the flag false and watch
   where the reading breaks. That is what would turn the polarisation mechanism
   from an inference into a measurement.

## State it was left in

`powerAlways: true` on the device AND in `templates/config.espgarden_s3.json`,
at the operator's instruction. **Zona 1 is therefore reporting 0.00 and not soil
moisture** — the badge, the history record and `moisture1` telemetry from this
board are the rail, not the pot, until this is resolved or the key is set false.

## Related

- `/devices.html` silently drops `powerAlways` in `buildDocument()`. Edit the key
  in `/config.html`. Deliberately not fixed; see CLAUDE.md.
- The hardware repo's electrolysis warning ("scrap in weeks") is about the
  probe's LIFE and is a separate, unmeasured concern that still applies while the
  bank is held up.

---

## Closed 2026-09-21 — the LED is SOLID with the flag on

The operator looked, with `powerAlways: true` in force: **the LED is on, not
blinking.** So GPIO 14 does stay up, `moisturePowerDown()` really is skipping
it, and the coalesced rule in `include/core/probe_power.h` works on hardware.

The contradiction is explained the way it was guessed: for part of the window
the key had been deleted by a `/devices.html` save, so blinking was correct
behaviour at the time it was seen. That page no longer drops the key.

**Still not done, and cheap if anyone ever wants it:** nobody has put a meter
on GPIO 14, so "the pin is high" rests on an LED and a reading rather than a
measurement of the pin. Good enough to close this.
