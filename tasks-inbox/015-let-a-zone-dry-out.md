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

---

## 2026-09-21 — A pot DID dry out. Half of this task is answered; the other half moved.

`Umidade Zona 4 (Arranjo)` on the S3 carrier (`b580`, probe slot 3, pin 5, relay 1)
fell **43.4 → 9.0 over 71.4 h** with nothing watering it. The record is
`backups/history-espgarden-s3.sqlite`: 4976 records at 60 s,
2026-09-18 08:59 .. 2026-09-21 23:26, collected minutes before the device's ring
would have evicted the start of it. `scripts/drying_fit.py` can now read that
archive (`--history-db`); the full measurement is in CLAUDE.md's verified list.

**This does NOT close the task, and the reason is worth reading before deciding.**

### What the dry pot answered

The DRYING-FIT question this task was expected to unblock is answered, and the
answer is no:

- Six drying segments across the four probes, **six refusals**. Both Arranjo
  segments are refused by **`model`** — `linear` beats every exponential on
  held-out extrapolation (1.489 vs 1.951 / 2.348 / 4.351 on the 26 h piece).
- **The fall ACCELERATES**: slope over successive fifths is
  −3.70, −4.43, −11.46, −23.07, −18.04 points/day, still ~18/day at the last
  sample. A drying pot on this probe does not decelerate into a floor.
- So the fitted asymptote is **below the scale** (−11.85, −3.02, and −121.49 for
  the two pieces joined by hand), every profile unbounded, the 26 h trim check
  moving the answer 41.4 points, and — unusually for this tool — the block
  bootstrap agreeing rather than misleading: widths 50.79 and 71.49, WIDER than
  the profiles, so the "Nx tighter" warning does not fire on either.
- Not an artefact of the split (joined by hand it is worse) and not of the rate
  (resampled to 300 s the verdict is identical).
- **The pot was not at equilibrium and the probe was not at its floor.** The
  "asymptote within 0.3 points of the last reading" gate never fired on the
  Arranjo; it fired on Zona 1 (0.24) and Zona 3 (0.07), which ARE the settled
  case it was written for.

**Consequence:** CLAUDE.md's *"what would change the verdict: one zone actually
drying out"* is spent. `moisture[i].dry` cannot be had by extrapolating a decay,
and more drying will not change that — there is no asymptote in this fall to be
ill-conditioned about.

### What is still open, and why this stays in the inbox

The CLASSIFIER question this task was written for is **not** answered by a
drying fit. It needs `moisture_fit.py` run against this archive, and two things
have to be settled first:

1. **The probes were re-plugged on 2026-09-21 ~19:34** (slots 0 and 2 were
   crossed; see CLAUDE.md). The Arranjo was not in that swap, so its curve is
   clean — but the archive holds **one** collection session, so it states no
   identity seam and the tool cannot see the rewire at all. A second collection
   run is what makes the seam statable.
2. **The Arranjo's own pump (relay 1) fired three times in this window**
   (09-18 17:32, 09-18 18:40, 09-21 14:36) and the probe did not rise at any of
   them. Six events is still the gate; whether these are events on a pot the
   pump actually waters is a question for the operator, not for the fit.

**Done now means:** collect a second session, re-run `moisture_fit.py
--history-db` on it, and see whether a pot that reached 9 gives the classifier
the separation twenty waterings on a wet pot could not. **Closing it early would
close it on the wrong question** — the drying half is finished, the badge half
is not.
