# Re-ask the ET0 question once an SHT40 is actually sited

`et0.enabled` ships false because the estimate was **measured** to have no skill
here: the thermometer follows 0.346 of the outdoor swing while its dewpoint
follows 1.052 — sitting behind thermal mass, which disconnects the one mechanism
Hargreaves-Samani has.

The failure was **siting, not accuracy**, so a ±0.2 °C part in the same enclosure
reports the same wrong slope more precisely. 001 correctly left the default alone.

**Done means:** with a board whose SHT40 is in free air, `scripts/et0_fit.py`
re-run against a public station, and the default changed only if it beats a
constant climatology under leave-one-out — the same bar that refused the ×1.286
scale factor.

Blocked on hardware existing and being mounted somewhere honest.
