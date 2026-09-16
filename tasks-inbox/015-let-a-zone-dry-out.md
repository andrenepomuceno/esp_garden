# The moisture model needs one zone to actually dry out

**This is a garden action, not a code change.** Nothing in the firmware or the
tooling can substitute for it.

With the archive refreshed to 2026-09-16 the event gate finally cleared — **20
watering events on Zona 1's own pump, 17 on Zona 3**, where 2026-09-03 had zero.
The fit still refuses, and now the reason is diagnostic:

| | dry | humid | wet | J (needs 4) |
|---|---|---|---|---|
| Zona 1 | 72.75 | 73.07 | 74.09 (sd 5.62) | **0.05** |
| Zona 3 | **76.14** | 74.91 | 75.22 | **0.04** |

Twenty pump runs moved Zona 1 by **1.34 points**, with the wet class's own
scatter four times the signal. On Zona 3 the "dry" class is the WETTEST of the
three. Zona 1's calibration is dry 53.1 / wet 73.5, and all three classes sit
**above the wet anchor** — the pot never leaves the wet end. The two-point path
refuses in the mirror image: *"no settled plateau follows a watering of this
probe's own pump, so the wettest reading is a wet day and not a saturated pot"*.

This garden is also watered by hand, which the relay record cannot label.

**Done means:** one zone left to dry on its own — no hand watering — long enough
for the archive to hold a real fall. Then re-run `moisture_fit.py`.

**What will NOT change it:** more days of the same. The separation is absent, not
merely short. Alternatives not excluded by the data: a probe sitting outside the
pot its pump waters, or a pump not delivering — but both probes track near their
wet anchors, and a dead probe would not.
