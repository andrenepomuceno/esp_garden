# The migration off ThingsBoard Cloud

On **2026-09-17 at 17:19:40 local** both boards stopped publishing to
`thingsboard.cloud` and from **17:21:40** they publish to a self-hosted
ThingsBoard **CE 4.3.1.4**. Everything before that instant existed only in
`backups/telemetry.sqlite`, the archive `scripts/tb_export.py` has been filling
since 2026-08-24. `scripts/tb_import.py` is the writer that put it back, and it
ran — see [What has actually run](verification-log.md) for the numbers.

**The archive is one device by construction, and that is how "which device" was
settled.** There is no device column anywhere in that schema; `meta` names
`espgarden1` and the cloud uuid, and the `deviceId` client attribute reads
`6224`, which is the WROOM-32 the target's `espgarden1` also reports. So a
second device would have been a second file, and the `espgarden2` that existed
on the Cloud tenant with no telemetry contributes nothing to migrate. Nothing
was written to `espgarden-s3`.

## The typing rule is the whole of the difficulty

`POST /api/plugins/telemetry/DEVICE/{id}/timeseries/ANY` takes
`[{"ts": ms, "values": {...}}]` and will happily backdate. What it does NOT do
is take a type: **ThingsBoard decides a datapoint's type from the JSON literal
the publisher sent.** `0` is a long. `66.62857055664062` is a double.
`0.47609522938728333` — seventeen significant digits — is a **string**, and
**74 702 of the archive's 404 142 rows are in that class**, because
`addContinuous()` publishes a `double` and Arduino_JSON prints as many digits as
it takes to round-trip a float widened to one.

So the type is a property of the POINT, not of the key. 18 of the 60 keys hold
both integer and decimal texts, which means `luminosity` is genuinely a long at
one instant and a double at the next, in the Cloud archive and in the live
self-hosted series alike.

**The writer therefore re-emits the archived TEXT verbatim as a JSON literal**
and lets the server make the same decision it made the first time. That is why
`build_body()` is hand-assembled rather than `json.dumps`-ed: parsing to a
Python float and formatting it back is shortest-round-trip, so a 17-digit
literal comes out at 16 and a string series silently becomes a double one,
part-way through, with nothing in the data to say where — the same defect class
as renumbering a relay index.

**And it is what makes the verification possible.** A non-strict REST read
returns the server's own stringification, so a value that comes back
character-identical to the archive is a value the server typed the same way: a
17-digit literal stored as a double would read back 16 digits long, and `0`
stored as a double would read back `0.0`. Comparing types against the LIVE
series would be the wrong check and would report two false failures, because a
key's type moves point to point.

## Counting is the verdict, not the HTTP status

Every key is counted on both sides in the same window and the pairs are printed,
because this repository already knows what a success report over a silent
failure looks like. A count alone is not enough — it can match while every value
is the wrong type — so four values per key are fetched back and compared
character by character, including the LONGEST literal that key ever held, which
is the one whose typing hangs on a digit.

## Resume, and the one measurement it rests on

**A re-write of the same `(entity, key, ts)` is an overwrite, not a duplicate.**
That was measured against the instance before anything bulk was written, and it
is what lets a dead run simply be re-run. The ledger under `.pio/` is an
optimisation on top of it, fingerprinted by device, server, upper bound and row
count so it can never be a ledger of a different plan.

**The census survives a ledger discard, and that is not tidiness.** "What was
the first self-hosted timestamp for this key" is answerable exactly once, before
the first imported point lands. The batch list is cheap to rebuild; the seam
evidence is not rebuildable at all.

## The seam is reported and never closed

The device-level gap is **119.562 s**. Per key it is larger, for a structural
reason: continuous keys resume **7.0 min** later because the first periodic
payload lands 300 s after boot, step keys resume on the boot event at
**15.6 min**, and event keys wait for an event — up to **2.05 days**. Sixteen
keys are retired and have no successor at all. Nothing is interpolated into any
of it, and `--verify` counts the seam window to prove nothing was.

## What the tool refuses

- **A timestamp at or after the archive's own newest point.** Both boards are
  publishing right now; an older value landing on a newer one corrupts the
  series this exists to preserve.
- **An `--upper` past the archive's newest point**, and an `--upper` that
  reaches the target's oldest live point. The second cannot fire against this
  data, which is exactly why it is a pure function with its own tests.
- **Attributes, by default.** An attribute is current state that overwrites, not
  a series. The five the device republishes on connect are already right on the
  target and a backdated copy could only ever be staler — after an OTA it would
  put `2.17.0` back over the version that just flashed. The five ThingsBoard
  writes itself are connectivity facts, and forging one can raise or clear an
  inactivity alarm on evidence nobody observed. Only `wateringMs` and
  `wateringTime` — operator settings the new server lacked — are carried, and
  only with `--write`.
