# A 60 s history archiver, and what it unblocks

**In flight** — agent, isolated worktree, based on `bac7ad4`. PC-side only.

Nothing captures the device's own 60 s record. `tb_export.py` archives
ThingsBoard, which is the 300 s telemetry, and CLAUDE.md names the consequence
twice: the moisture model cannot be seeded from a mean whose variance averaging
has shrunk, and the three-minute drying plateau is below the sampling that
stored it.

**Done means:** an incremental archiver that cannot double-count or drop across
pulls, a cadence whose arithmetic shows it cannot lose a record before the
device evicts it, a simulator mock rich enough to reproduce paging and eviction,
and an honest verdict on seeding `/moisture_model.bin`.

**"Still no, and here is what would change it" is a complete result.** The device
holds ~2 days and the classifier needs six decayed watering events; this garden
has zero on either probe's own pump.

**Must not touch 192.168.1.55.** The live run is mine.

---
**Done** — `0a38e8f`..`1aa54d5`, merged as `d65bb01`. No C++ changed.

Verdict on seeding `/moisture_model.bin`: **still no**, and the binding reason is
not tooling — both probes have zero watering events on their own pump, so the
six-event gate fails before any statistic is computed.

It also **withdrew one of CLAUDE.md's own five reasons as a mis-statement**: the
archive and the 60 s record are the same statistic (`getAverage()` over a
300-sample window), read out at different rates — a 5x decimation, not a
smoothing. What differs is samples per class window, and it makes the 300 s path
*harder* to pass, not easier.
