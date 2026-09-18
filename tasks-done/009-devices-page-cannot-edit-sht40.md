# `/devices.html` cannot add or remove the SHT40

Surfaced by 001 rather than hidden. The page is pin-centric and this kind has no
pin of its own, so `devices_model.js` filters it out via `hasKind()`. Editing it
means `/config.html`.

Not a hazard: a save from that page carries `io.sht4x` / `io.i2c` through
untouched, and a relay wrongly placed on GPIO 8 is still refused at save time by
`documentPinsAreUsable()`. The page just will not *warn* first.

**Done means:** either the page learns to render a bus-attached kind, or the
limitation is accepted and the page says so where an operator will read it — not
only in CLAUDE.md.

Low priority. Nothing is wrong, something is missing.

---
**Partly done, 2026-09-17 — a DIFFERENT defect on the same page was the one
that bit.** `/devices.html` refused to SAVE the carrier's own wiring: four
probes on one switched power bank (GPIO 14) drew the generic duplicate-pin
error. The firmware allows that deliberately and cites the case in
`validatePins()`' own comment; the page did not know. Fixed in `6140887` —
claims carry a kind, and the waiver applies only when every claimant is probe
power.

**The original limitation stands**: the page still cannot add or remove the
SHT40, because it is pin-centric and that kind has no pin of its own.
