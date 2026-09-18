# Run the 60 s archiver against the garden for the first time

Built and gated in task 003; **not one byte has been sent to 192.168.1.55 by it.**

```
python scripts/history_export.py --plan     # arithmetic only, no network
python scripts/history_export.py            # the real collection
```

Cost, as reported: GETs only, ONE login, strictly sequential, one logout — **one
session slot**. First run 19 requests / ~445 KB; every run after, at the
recommended 6 h cadence, 6 requests / ~56 KB.

**Why the cadence matters:** guaranteed retention is `recordsPerSegment x 7 x
period` = **43.75 h** on 6224, so 6 h tolerates six consecutive failed runs
before a record is lost. Planning against the 50 h capacity instead loses one run
in eight, silently.

Do not run it during an OTA or while the browser UI is in use — the board serves
HTTP from a single `async_tcp` task.

Note it does **not** answer the 2026-09-03 drying plateau retroactively: that
evening was never in a collected buffer and is gone.

---
**Done, 2026-09-17.** 2787 records in 19 requests / 455 KB / ~4.9 s, one session
slot. Neither guard fired — and that is the guard meeting a real eviction and
having nothing to say, not a test of it: no rotation lands inside a 4.9 s walk.

**The 17.6 h clock-bug hole is confirmed at 17.72 h**, and attributed to the
device not having written rather than to nobody collecting, by arithmetic: 2787
records at 60 s cover 46.45 h inside a 64.41 h span.

The fit still refuses, and the gate moved from "zero events" to "1 and 2" —
but the measured watering response is +0.16, −0.21 and −0.07 points. See
[015](../tasks-inbox/015-let-a-zone-dry-out.md): six more events would satisfy
the gate and not the physics.
