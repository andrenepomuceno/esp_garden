#!/usr/bin/env python3
"""Where a drying trace comes from, and at what rate it was sampled.

Two archives hold this garden's moisture, they are different shapes, and the
difference that matters is not the schema - it is the PERIOD:

  backups/telemetry.sqlite          the ThingsBoard uplink, 300 s, keyed
                                    `moisture1..4` POSITIONALLY with nothing in
                                    the series to say when a slot changed
                                    meaning. tb_export.SEAM_MS is the one seam
                                    anybody wrote down.
  backups/history-*.sqlite          the device's OWN /history.json record,
                                    60 s, written by scripts/history_export.py,
                                    which stamps the (slot -> name, pin, relay)
                                    binding it saw into `sessions` on every run.

drying_fit.py was written against the first and hard-coded its period in three
places. This file is the seam between "which archive" and "what the fit does
with it", and it exists because a 60 s record read with a 300 s threshold is
not a smaller number - it is a different claim about what soil can do.

The 60 s side reuses history_archive.samples_for_probe(), the same adapter
moisture_fit.py --history-db consumes, rather than reading the columns again:
a second reader of one table is a second belief about NaN slots.

Standard library only. Both archives are opened READ-ONLY - deliberately NOT
through history_store.open_archive(), which runs CREATE TABLE IF NOT EXISTS and
therefore takes a write lock on a file this tool only reads.
"""

from __future__ import annotations

import sqlite3
from pathlib import Path

import history_archive as ha
import history_store as store
from history_export import identity_seams, latest_identity
from tb_export import SEAM_MS

# The ThingsBoard archive's publish period since 2026-09-03 18:43. It is the
# resample step for that source and nothing else; the device's own record
# reports its own, measured by history_archive.observed_period().
TELEMETRY_STEP_SEC = 300.0

# What history_export.py writes when it is given no name.
DEFAULT_HISTORY_PERIOD_SEC = 60.0


def open_readonly(path):
    connection = sqlite3.connect("file:%s?mode=ro" % Path(path).as_posix(),
                                 uri=True)
    connection.row_factory = sqlite3.Row
    return connection


# ---------------------------------------------------------------------------
# the 300 s ThingsBoard archive
# ---------------------------------------------------------------------------


def read_series(cursor, key, since=None, until=None):
    query = "SELECT ts, value FROM telemetry WHERE key = ?"
    args = [key]
    if since is not None:
        query += " AND ts >= ?"
        args.append(since)
    if until is not None:
        query += " AND ts <= ?"
        args.append(until)
    out = []
    for stamp, raw in cursor.execute(query + " ORDER BY ts", args):
        try:
            value = float(raw)
        except (TypeError, ValueError):
            continue
        if value == value:
            out.append((stamp / 1000.0, value))
    return out


def telemetry_channels(path, since, until, stitch):
    """The moisture series, and the one place the archive lies about identity.

    tb_export.SEAM_MS records that at 2026-09-02 13:41:29 `moisture3` stopped
    and `moisture2` took over the SAME physical sensor, because the archive
    keys probes positionally and a slot was deleted. Off by default the seam is
    a hard boundary, exactly as moisture_fit.py treats it; --stitch-seam joins
    the two halves into one channel on the strength of that recorded identity.
    The join is checked rather than assumed - the report prints the gap across
    it, and here it is 0.08 points across 76 seconds.
    """
    connection = open_readonly(path)
    try:
        cursor = connection.cursor()
        found = {}
        for index in range(1, 5):
            key = "moisture%d" % index
            series = read_series(cursor, key, since, until)
            if series:
                found[key] = series
    finally:
        connection.close()

    source = {"kind": "telemetry", "path": str(path),
              "periodSec": TELEMETRY_STEP_SEC}
    if not stitch:
        for key, series in list(found.items()):
            if key in ("moisture2", "moisture3"):
                seam = SEAM_MS / 1000.0
                before = [p for p in series if p[0] < seam]
                after = [p for p in series if p[0] >= seam]
                keep = before if len(before) >= len(after) else after
                found[key] = keep
        return found, None, source

    seam = SEAM_MS / 1000.0
    before = [p for p in found.get("moisture3", []) if p[0] < seam]
    after = [p for p in found.get("moisture2", []) if p[0] >= seam]
    if not before or not after:
        return found, None, source
    found.pop("moisture3", None)
    found.pop("moisture2", None)
    found["zona3 (moisture3+moisture2)"] = before + after
    joint = {"at": seam, "lastBefore": before[-1], "firstAfter": after[0],
             "gapSec": after[0][0] - before[-1][0],
             "gapPoints": after[0][1] - before[-1][1]}
    return found, joint, source


# ---------------------------------------------------------------------------
# the 60 s record the device keeps itself
# ---------------------------------------------------------------------------


def channel_name(probe):
    """A probe's own name, carrying the SLOT it was read from.

    The slot is in the label because everything downstream of this file - the
    ThingsBoard key `moistureN`, the history record's positional array, the
    firmware's `moisture[i]` - is addressed by it, and a name is a LABEL an
    operator can change from /devices.html between two collection runs.
    """
    label = (probe.get("name") or "").strip() or "probe"
    return "%s [slot %d]" % (label, probe["index"])


def history_channels(path, since, until):
    """Every probe slot in the device's own record, bounded by its own seams.

    The window rule is the one moisture_fit.py --history-db applies, and it is
    deliberately the blunt one: the most recent STATED disagreement between two
    collection sessions bounds every channel, because the sessions table can
    say that the io block was edited and cannot say which slot moved.

    WHAT IT CANNOT SEE, and the report says so out loud: a seam needs two
    sessions to disagree. An archive collected in ONE run states no seam at
    all, so a probe moved between two terminals, a pot repotted or a
    /devices.html save inside that window is invisible here - it is not that
    the check passed, it is that there was nothing for it to compare against.
    """
    connection = open_readonly(path)
    try:
        identity = latest_identity(connection)
        seams = identity_seams(connection)
        stamped = connection.execute(
            "SELECT COUNT(*) FROM sessions WHERE identity IS NOT NULL"
        ).fetchone()[0]
        device = connection.execute(
            "SELECT value FROM meta WHERE key = 'device'").fetchone()
        records = store.read_records(connection)
    finally:
        connection.close()

    if identity is None:
        raise SystemExit(
            "%s has no collection session in it; run"
            " scripts/history_export.py against the device first" % path)
    if not records:
        raise SystemExit("%s holds no records yet" % path)

    times = [record["t"] for record in records]
    period = ha.observed_period(times) or DEFAULT_HISTORY_PERIOD_SEC

    boundary = 0.0
    reasons = []
    for seam in seams:
        if float(seam["at"]) > boundary:
            boundary = float(seam["at"])
            reasons = ["session %d saw a different binding: %s"
                       % (seam["session"], "; ".join(seam["changed"]))]
    if since is not None:
        boundary = max(boundary, since / 1000.0)
    ceiling = (until / 1000.0) if until is not None else None

    usable = [record for record in records
              if record["t"] >= boundary
              and (ceiling is None or record["t"] <= ceiling)]

    found = {}
    probes = []
    for probe in identity.get("probes", []):
        samples = ha.samples_for_probe(usable, probe["index"])
        if not samples:
            continue
        name = channel_name(probe)
        found[name] = samples
        probes.append({"channel": name, "relay": probe.get("relay"),
                       "pin": probe.get("pin"), "kind": probe.get("kind"),
                       "invert": probe.get("invert")})

    source = {
        "kind": "history",
        "path": str(path),
        "device": device[0] if device else identity.get("id"),
        "periodSec": period,
        "records": len(records),
        "used": len(usable),
        "sessions": stamped,
        "seams": reasons,
        "boundary": boundary,
        "gaps": ha.coverage_gaps([r["t"] for r in usable], period),
        "probes": probes,
        "span": (usable[0]["t"], usable[-1]["t"]) if usable else None,
    }
    return found, None, source


# ---------------------------------------------------------------------------
# what the reader is owed before any number below it
# ---------------------------------------------------------------------------


def describe_source(source, step_sec, tz, local):
    """Which record this is, at what rate, and what the record cannot say.

    `local` is passed in rather than imported so this module stays free of
    moisture_stats; the caller already has it.
    """
    from moisture_stats import step_threshold

    print("\nSource: %s" % source["path"])
    if source["kind"] == "telemetry":
        print("  the ThingsBoard uplink, %.0f s publish period"
              % source["periodSec"])
    else:
        span = source.get("span")
        print("  the device's own /history.json record, device %s"
              % source.get("device"))
        print("  %d records, %d inside the window%s, observed period %.0f s"
              % (source["records"], source["used"],
                 (", %s -> %s" % (local(span[0], tz), local(span[1], tz)))
                 if span else "", source["periodSec"]))
        for gap in source.get("gaps", []):
            print("  GAP %s -> %s  %.0f min, about %d records"
                  % (local(gap["from"], tz), local(gap["to"], tz),
                     gap["seconds"] / 60.0, gap["missing"]))
        for probe in source.get("probes", []):
            print("  %-34s pin %-4s relay %-4s %s"
                  % (probe["channel"], probe["pin"], probe["relay"],
                     probe["kind"] or ""))
        if source["seams"]:
            for reason in source["seams"]:
                print("  IDENTITY SEAM at %s: %s"
                      % (local(source["boundary"], tz), reason))
            print("  Everything before it is excluded, for every channel: the"
                  " sessions table can say the io block was edited and cannot"
                  " say which slot moved.")
        elif source.get("sessions", 0) < 2:
            print("  ONE stamped collection session, so this archive STATES NO"
                  " SEAM - which is not the same as there being none. A probe"
                  " moved between two terminals, a pot repotted or a"
                  " /devices.html save inside this window leaves no trace"
                  " here, because a seam is a disagreement BETWEEN two runs"
                  " and there is only one. Bound it with --since if you know"
                  " of one.")
        else:
            print("  %d stamped sessions and no disagreement between them."
                  % source["sessions"])
    # Two periods, and they are not the same one. Segmentation runs on the RAW
    # samples, so a discontinuity is judged at the rate the record was written
    # at; the fit runs on the resampled series, so a time constant is judged at
    # the rate it was binned to. --step separates them and the report has to as
    # well, or a tau floor of 10 min on a 60 s record reads as a contradiction.
    print("  Segmented at the record's %.0f s, so a step is %.2f points;"
          " fitted at %.0f s, so a tau under 2 periods (%.1f min) is"
          " UNRESOLVED."
          % (source["periodSec"], step_threshold(source["periodSec"]),
             step_sec, 2.0 * step_sec / 60.0))


# ---------------------------------------------------------------------------
# checks() - folded into `drying_fit.py --self-test`
# ---------------------------------------------------------------------------


def self_test():
    """Against a real sqlite3, because the adapter IS the identity rule."""
    failures = []

    def check(name, condition, detail=""):
        if not condition:
            failures.append("%s %s" % (name, detail))

    check("sources/telemetry-step-is-the-thingsboard-period",
          TELEMETRY_STEP_SEC == 300.0, str(TELEMETRY_STEP_SEC))

    # A label the operator can change must not be the whole identity: two
    # probes renamed to the same thing would otherwise collide into one
    # channel and be fitted as if they were one pot.
    same = [channel_name({"index": i, "name": "Zona"}) for i in (0, 2)]
    check("sources/channel-name-carries-the-slot", same[0] != same[1],
          str(same))
    check("sources/channel-name-survives-an-empty-label",
          channel_name({"index": 1, "name": ""}) == "probe [slot 1]",
          channel_name({"index": 1, "name": ""}))

    conn = store.open_archive(":memory:")
    records = ha._fake_records(40)
    store.merge(conn, records, 1)
    identity = {"id": "b580", "historyPeriodSec": 60, "historyRecords": 5000,
                "relays": [], "probes": [
                    {"index": 0, "name": "Zona 1", "pin": 1, "relay": 0,
                     "invert": True, "kind": "resistive", "dry": 0, "wet": 0}]}
    import json as _json
    conn.execute(
        "INSERT INTO sessions (at, host, device, capacity, stored, period_sec,"
        " requests, added, identity) VALUES (?,?,?,?,?,?,?,?,?)",
        (records[0]["t"], "h", "b580", 5000, 40, 60.0, 1, 40,
         _json.dumps(identity)))
    conn.commit()

    # The period comes from the RECORDS, not from the configured number the
    # session also carries: a background task reschedules from the end of its
    # callback, so the delivered period is the configured one or a little more,
    # and it is the delivered one every threshold here is scaled by.
    times = [record["t"] for record in records]
    check("sources/period-is-observed-not-configured",
          abs(ha.observed_period(times) - 60.0) < 1e-9,
          str(ha.observed_period(times)))

    # One stamped session states NO seam. That is the case this archive is in
    # and the case the report has to be loud about.
    check("sources/one-session-states-no-seam", identity_seams(conn) == [])
    conn.close()
    return failures


if __name__ == "__main__":
    import sys

    problems = self_test()
    for line in problems:
        print("FAIL " + line)
    print("drying_sources self-test: %s" % ("FAILED" if problems else "ok"))
    sys.exit(1 if problems else 0)
