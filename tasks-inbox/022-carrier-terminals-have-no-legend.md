# 022 — The carrier's probe terminals carry no legend, and it crossed two probes

**Opened** 2026-09-21. **Board** `espgarden-s3` (b580). **Belongs to** the sibling
hardware repo `../esp-gargen-hardware`, filed here because that is where it was
found and because the firmware is what had to prove it.

## What happened

Two probes were plugged into the wrong terminals — Zona 1 and Zona 3 crossed —
and it took a controlled stimulus test to establish it. See CLAUDE.md's
2026-09-21 entry for the evidence. **Nothing in the firmware or the netlist was
wrong**: `production/netlist.ipc` gives J4→GPIO 1, J5→GPIO 2, J6→GPIO 4,
J7→GPIO 5, matching `config.json` exactly.

## Why the board invites it

- **The only text on each terminal footprint is `${REFERENCE}`**, so the board
  prints `J4 J5 J6 J7` and nothing else. There is no `SOIL1`, no `ZONA 1`.
- **The numbering runs backwards to reading order.** Positions from the PCB:

  | | x ≈ 174.7 | x ≈ 179.4 |
  |---|---|---|
  | **y ≈ 107.4** (top) | `J7` = Zona 4 | `J6` = Zona 3 |
  | **y ≈ 117.7** (bottom) | `J5` = Zona 2 | `J4` = Zona 1 |

  Plugging in from the top left gives the reverse mapping. And `J4`/`J6` —
  Zona 1 and Zona 3 — are stacked in the same column, which is the pair most
  easily crossed and is exactly the pair that was.

## What to do, in the hardware repo

1. **Add a silkscreen legend per terminal** — `SOIL1`..`SOIL4`, or better the
   zone numbers the firmware uses, beside each connector. One silkscreen change,
   no netlist change, and it removes the whole class.
2. **Consider renumbering the references** so `J4`..`J7` run in reading order,
   or renaming them `J_SOIL1`..`J_SOIL4`. A reference that disagrees with the
   layout is a second thing to remember.
3. Nothing here needs a firmware change. Resist the temptation to "fix" a
   crossed probe in `config.json`: swapping names and relay indices closes the
   loop but redefines `moisture1` / `moisture3`, which are positional
   ThingsBoard keys with stored history behind them — the `moisture2` defect
   this repo already paid for.

## Why the firmware could not have caught it

`validatePins()` audits what a GPIO may do; it cannot know what is on the far
end of a wire. `probe_health` reports `connected` for every channel here,
correctly — all four carry real probes, just not the pots their names claim.
**The only instrument that maps a pot to an index is a stimulus somebody
applies to a known pot**, which is what settled it. Worth remembering the next
time a probe reading looks wrong: the question is rarely the pin.
