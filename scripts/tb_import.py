#!/usr/bin/env python3
"""Writes the SQLite telemetry archive into a ThingsBoard instance.

WHY THIS EXISTS

`tb_export.py` is the reader: ThingsBoard Cloud -> backups/telemetry.sqlite.
This is the writer, and it exists because the garden moved. Both boards
published to thingsboard.cloud until 2026-09-17 17:19:40 local and to a
self-hosted instance from ~17:21 on. The new server therefore starts its life
holding two minutes of history, and everything before that lives only in the
archive.

    backups/telemetry.sqlite  ->  POST .../timeseries/ANY  ->  self-hosted TB

WHAT THIS TOOL IS AFRAID OF

A partial write that reports success. This repository's defining incident is a
dashboard that said "MQTT enabled" for three years over a broker receiving
nothing, and the only thing that gave it away was a counter nobody was reading.
So the verdict here is never an HTTP status:

  * every key is COUNTED on both sides, in the same window, and the pairs are
    printed;
  * a key whose counts disagree is named, with both numbers;
  * a sample of values is fetched back and compared to the archive TEXT
    character by character, because a count can match while every value is a
    string where it should be a double;
  * the run exits non-zero when anything disagrees.

WHAT IT WILL NOT DO

  * It will not write a timestamp at or after the archive's own newest point.
    Both boards are publishing live; an older value landing on top of a newer
    one would corrupt the series this exists to preserve. The upper bound is
    printed on every run.
  * It will not stitch the seam. The gap between the last imported point and
    the first self-hosted point is measured and reported PER KEY, and nothing
    is interpolated into it. A seam this file cannot see is a seam the next
    reader takes for data.
  * It will not import attributes by default. See CLASSIFIED_ATTRIBUTES: an
    attribute is current state that overwrites, not a series, so backdating one
    is either a no-op or a lie about the device now.

THREE THINGS WERE MEASURED AGAINST THE LIVE INSTANCE BEFORE ANY BULK WRITE, and
they are what the design rests on. `--probe` re-runs them on a throwaway device
it creates and deletes.

  1. A BACKDATED WRITE IS ACCEPTED AND READ BACK. 2026-08-24 04:00:28 went in
     and came out. No TTL rejected it. (What cannot be measured in one session
     is a nightly cleanup job; the tenant profile is SYS_ADMIN-only, and
     /api/usage reports every tenant limit at 0.)
  2. A RE-WRITE OF THE SAME ts IS AN OVERWRITE, NOT A DUPLICATE. The same
     (entity, key, ts) written three times with two different values left
     exactly one point holding the last value. THE WHOLE RESUME STRATEGY RESTS
     ON THIS, which is why it is a measurement and not a citation.
  3. THE BATCH CEILING IS FAR ABOVE WHAT THIS NEEDS. 200 000 points in one
     10.5 MB body answered 200; 50 000 points took 2.9 s. So the default batch
     is chosen for a readable progress line and a survivable retry, not to dodge
     a limit.

THE VALUE TEXT IS RE-EMITTED VERBATIM AS A JSON LITERAL, AND THAT IS THE SUBTLE
PART. ThingsBoard decides a datapoint's TYPE from the literal the publisher
sent, and it is not the decision a reader would guess: `0` becomes a long,
`66.62857055664062` a double, and `0.47609522938728333` -- seventeen
significant digits -- a STRING. 74 702 of the archive's 404 142 rows are in
that third class, and the live post-cutover series holds them as strings too,
because the same firmware published both.

The archive stores TB's own stringification of each value, so feeding that text
back in AS A RAW LITERAL reproduces the server's original decision without this
tool having to model it. Parsing to a Python float first would NOT: repr() is
shortest-round-trip, so a 17-digit literal comes out at 16 and silently becomes
a double where the live series has a string. Verified on the instance for all
six shapes -- long, double, 17-digit, boolean, plain string, and a double that
round-trips -- with useStrictDataTypes=true, and the non-strict read came back
character-identical to the archive in every case.

WHERE THE CODE IS

`scripts/tb_client.py` holds the session and the request body; this file holds
the archive, the ledger, the refusals and the commands. They were one file until
the pair crossed the 1000-line gate, and the seam is the same one
history_export.py / history_store.py uses: the tool that DECIDES here, the thing
that talks to the far end there. `--self-test` covers both.

THE CREDENTIAL

Read from a file, inside this process, never printed and never put on a command
line -- the same rule tb_export.py states, for the same reason: a command line
lands in the process list and in shell history. Default
`<repo>/.chave_tb_selfhosted` (gitignored by `.chave_tb*`), overridden with
TB_IMPORT_KEY_FILE. Either a JSON object with `username` and `password`, or two
lines holding the same two values.

Run:  python scripts/tb_import.py --plan             # cost, offline, no network
      python scripts/tb_import.py --census           # what the server holds now
      python scripts/tb_import.py --probe            # re-measure the 3 facts
      python scripts/tb_import.py --import           # the bulk run
      python scripts/tb_import.py --verify           # count both sides, per key
      python scripts/tb_import.py --seam             # the gap, per key
      python scripts/tb_import.py --attributes       # the 2 that are not refused
      python scripts/tb_import.py --self-test        # offline, no credential
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sqlite3
import sys
import tempfile
import time
from datetime import datetime
from pathlib import Path

from tb_client import (ApiError, Client, Fatal, WRITE_BATCH_POINTS,
                       batches, build_body, group_by_ts, json_literal)

ROOT = Path(__file__).resolve().parent.parent

DEFAULT_SERVER = "https://2.25.232.134.sslip.io"
DEFAULT_DEVICE = "espgarden1"
DEFAULT_DB = ROOT / "backups" / "telemetry.sqlite"
DEFAULT_KEY_FILE = ROOT / ".chave_tb_selfhosted"
DEFAULT_STATE = ROOT / ".pio" / "tb_import.state.json"

# Why each archived attribute is or is not written. An attribute is CURRENT
# STATE THAT OVERWRITES -- ThingsBoard keeps one value per key, not a series --
# so "backdating" one means replacing what the live device says about itself
# right now with what a dead session said weeks ago.
#
#   device    the device republished it on its first connect to this server,
#             with the same value; writing the archived copy can only ever
#             overwrite a fresh truth with a stale one. After an OTA it would
#             put 2.17.0 back over the version that just flashed.
#   platform  ThingsBoard writes it itself, from the device-state rule node.
#             Forging it is lying about connectivity, and an inactivity alarm
#             raised or cleared on that evidence is worse than a missing row.
#   operator  somebody typed it, nothing else holds it, and the new server does
#             not have it. These are the only two worth carrying, and they still
#             need --attributes.
CLASSIFIED_ATTRIBUTES = {
    "ambient_sensor": ("device", "the device publishes it from tbOnConnect()"),
    "current_fw_title": ("device", "published on every connect"),
    "current_fw_version": ("device", "ThingsBoard reads this to decide an "
                                     "update landed; a stale copy is a lie"),
    "deviceId": ("device", "published on every connect"),
    "hostname": ("device", "published on every connect"),
    "active": ("platform", "written by the device-state rule node"),
    "inactivityAlarmTime": ("platform", "written by the device-state rule node"),
    "lastActivityTime": ("platform", "written by the device-state rule node"),
    "lastConnectTime": ("platform", "written by the device-state rule node"),
    "lastDisconnectTime": ("platform", "written by the device-state rule node"),
    "wateringMs": ("operator", "a dashboard setting; the new server lacks it"),
    "wateringTime": ("operator", "a dashboard setting; the new server lacks it"),
}

PROBE_DEVICE_PREFIX = "zz-tb-import-probe-"



# --------------------------------------------------------------------------
# The archive
# --------------------------------------------------------------------------

def open_archive(path: Path) -> sqlite3.Connection:
    if not path.exists():
        raise Fatal("no archive at %s; tb_export.py is what fills it" % path)
    return sqlite3.connect("file:%s?mode=ro" % path.as_posix(), uri=True)


def archive_identity(conn) -> dict:
    rows = dict(conn.execute("SELECT key, value FROM meta"))
    if "device_name" not in rows:
        raise Fatal("the archive names no device in its meta table; it was not "
                    "written by tb_export.py")
    return rows


def archive_bounds(conn) -> tuple:
    row = conn.execute("SELECT MIN(ts), MAX(ts), COUNT(*), COUNT(DISTINCT key) "
                       "FROM telemetry").fetchone()
    if not row or row[0] is None:
        raise Fatal("the archive holds no telemetry")
    return row


def archive_key_counts(conn, lower: int, upper: int) -> dict:
    """Rows per key inside the migrated window. The left half of the verdict."""
    return {k: n for k, n in conn.execute(
        "SELECT key, COUNT(*) FROM telemetry WHERE ts >= ? AND ts <= ? "
        "GROUP BY key", (lower, upper))}


def fmt_ts(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000.0).strftime("%Y-%m-%d %H:%M:%S")


def fmt_gap(seconds: float) -> str:
    if seconds < 90:
        return "%.0f s" % seconds
    if seconds < 5400:
        return "%.1f min" % (seconds / 60.0)
    if seconds < 172800:
        return "%.2f h" % (seconds / 3600.0)
    return "%.2f days" % (seconds / 86400.0)


# --------------------------------------------------------------------------
# The ledger
# --------------------------------------------------------------------------

def upper_bound_refusal(archive_max: int, upper: int, first_live) -> str:
    """Why this upper bound may not be written, or None.

    Pure, and separated from the command for one reason: ON THIS MIGRATION IT
    CAN NEVER FIRE. The seam is positive -- the archive ends at 17:19:40 and the
    target's oldest point is at 17:21:40 -- so the branch that stops an older
    value landing on top of a live one is dead code against this data, and dead
    code is exactly what is never exercised until the day it matters. The
    self-test reaches every branch instead.
    """
    if upper > archive_max:
        return ("--upper %d is past the archive's newest point %d"
                % (upper, archive_max))
    if first_live is not None and first_live <= upper:
        return ("the target's oldest point (%d) is at or before the upper bound "
                "(%d); importing would write underneath live data"
                % (first_live, upper))
    return None


def plan_fingerprint(device: str, server: str, upper: int, total: int) -> str:
    """What a stored ledger is a ledger OF.

    A resume that reuses a ledger written for another device, another server or
    another upper bound is a resume that skips work it never did. Cheap to
    compute, and the only thing standing between "I already wrote that" and a
    silent hole.
    """
    h = hashlib.sha256()
    h.update(("%s|%s|%d|%d" % (device, server, upper, total)).encode())
    return h.hexdigest()[:16]


def load_state(path: Path, fingerprint: str) -> dict:
    """The ledger, discarded when it is a ledger of a different plan.

    THE CENSUS SURVIVES THAT DISCARD, and it is the one thing here that must.
    "What was the first self-hosted timestamp for this key" is answerable
    exactly once -- before the first imported point lands -- so a `--upper` that
    changes the plan must not throw away the only record of the seam. The `done`
    list is cheap to rebuild (re-writing a batch is an overwrite, measured); the
    census is not rebuildable at all.
    """
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"fingerprint": fingerprint, "done": [], "census": None}
    if state.get("fingerprint") != fingerprint:
        print("tb_import: the ledger at %s was written for a different plan "
              "(%s, not %s); the batch list starts again, the census is kept."
              % (path, state.get("fingerprint"), fingerprint))
        return {"fingerprint": fingerprint, "done": [],
                "census": state.get("census")}
    return state


def save_state(path: Path, state: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(state), encoding="utf-8")
    tmp.replace(path)


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def cmd_plan(conn, args) -> int:
    """Everything that can be said without a network."""
    meta = archive_identity(conn)
    lo, hi, rows, keys = archive_bounds(conn)
    points = group_by_ts(conn.execute(
        "SELECT ts, key, value FROM telemetry ORDER BY ts, key"))
    body_bytes = sum(len(build_body(b)) for b in batches(points))
    n_batches = sum(1 for _ in batches(points))

    print("archive     %s" % args.db)
    print("device      %s   (uuid in the archive: %s)"
          % (meta.get("device_name"), meta.get("device_id")))
    print("rows        %d datapoints, %d keys, %d distinct timestamps"
          % (rows, keys, len(points)))
    print("span        %s .. %s" % (fmt_ts(lo), fmt_ts(hi)))
    print("upper bound %d (%s) -- nothing at or after this is written"
          % (hi, fmt_ts(hi)))
    print("batches     %d of up to %d datapoints, %.1f MB of request bodies"
          % (n_batches, WRITE_BATCH_POINTS, body_bytes / 1e6))
    print("target      %s, device %s" % (args.server, args.device))
    print("\nNothing has been sent. `--census` reads the target; `--import` writes.")
    return 0


def census(client: Client, dev: str, lower: int, upper: int) -> dict:
    """What the target holds, per key, BEFORE anything is written.

    Run first and stored, because after one imported point the answer to "what
    was the first self-hosted timestamp" is gone -- and that number is the seam.
    """
    out = {"first_live": {}, "in_window": {}, "keys": sorted(client.keys(dev))}
    for key in out["keys"]:
        # Oldest point the server holds for this key, at any time.
        page = client.series(dev, key, 0, upper * 4, limit=1)
        out["first_live"][key] = page[0]["ts"] if page else None
        # And how many it already holds inside the window about to be written.
        out["in_window"][key] = len(client.walk(dev, key, lower - 1, upper + 1))
    return out


def census_refusal(existing, force: bool) -> str:
    """Why a census must not be retaken, or None.

    A SECOND census taken after the import is not a refresh, it is a deletion.
    `first_live` is read as "the oldest point the target holds for this key",
    which before the import is the first self-hosted publish -- the seam -- and
    afterwards is 2026-08-24, because this tool put it there. Overwriting the
    stored answer would leave `--seam` reporting a gap of zero and nothing
    saying it had ever been measured, which is precisely the silent-success
    failure the rest of this file is built against. `--recensus` is for the
    operator who knows the stored one is wrong.
    """
    if existing and not force:
        return ("a census is already stored and retaking it would overwrite the "
                "seam: after an import the target's oldest point is the "
                "archive's, not the first self-hosted publish. Use --recensus "
                "only if the stored one is known to be wrong.")
    return None


def cmd_census(client, conn, args, state, dev) -> int:
    lo, hi, rows, _ = archive_bounds(conn)
    upper = args.upper or hi

    refusal = census_refusal(state.get("census"), args.recensus)
    if refusal:
        print("REFUSED: %s" % refusal)
        return 1

    # The range semantics, measured rather than assumed, on a key that exists.
    probe_key = sorted(client.keys(dev))[0]
    newest = client.series(dev, probe_key, 0, upper * 4, limit=1)
    anchor = None
    if newest:
        anchor = newest[0]["ts"]
        same = client.series(dev, probe_key, anchor, anchor, limit=5)
        wider = client.series(dev, probe_key, anchor, anchor + 1, limit=5)
        print("range check  startTs=endTs=%d -> %d point(s); endTs+1 -> %d. "
              "endTs is %s."
              % (anchor, len(same), len(wider),
                 "EXCLUSIVE" if not same and wider else "inclusive"))

    c = census(client, dev, lo, upper)
    state["census"] = c
    save_state(Path(args.state), state)

    live_firsts = [t for t in c["first_live"].values() if t]
    print("\ntarget device %s (%s) holds %d keys" % (args.device, dev, len(c["keys"])))
    if live_firsts:
        print("first point on the target: %s (%d)"
              % (fmt_ts(min(live_firsts)), min(live_firsts)))
    print("archive upper bound      : %s (%d)" % (fmt_ts(upper), upper))
    occupied = {k: n for k, n in c["in_window"].items() if n}
    if occupied:
        print("\nALREADY INSIDE THE WINDOW (these were imported, or the bound "
              "is wrong):")
        for k in sorted(occupied):
            print("   %-22s %d" % (k, occupied[k]))
    else:
        print("\nNothing on the target lies inside the window to be written.")
    refusal = upper_bound_refusal(hi, upper,
                                  min(live_firsts) if live_firsts else None)
    if refusal:
        print("\nREFUSED: %s" % refusal)
        return 1
    print("\nCensus stored in %s" % args.state)
    return 0


def cmd_probe(client, args) -> int:
    """Re-measures the three facts the design rests on, on a throwaway device."""
    name = PROBE_DEVICE_PREFIX + str(int(time.time()))
    dev = client.call("POST", "/api/device",
                      json.dumps({"name": name, "type": "default"}).encode())
    pid = dev["id"]["id"]
    print("probe device %s (%s)" % (name, pid))
    ok = True
    try:
        old = 1787554828330  # 2026-08-24 04:00:28, the archive's oldest point
        path = "/api/plugins/telemetry/DEVICE/%s/timeseries/ANY" % pid
        client.call("POST", path, build_body([(old, [("probe", "1.0")])]))
        back = client.series(pid, "probe", old - 1, old + 1)
        print("1. backdated write     -> %r  %s"
              % (back, "ACCEPTED" if back else "REFUSED"))
        ok &= bool(back)

        client.call("POST", path, build_body([(old, [("probe", "2.0")])]))
        client.call("POST", path, build_body([(old, [("probe", "2.0")])]))
        back = client.series(pid, "probe", old - 1, old + 1)
        print("2. rewritten twice     -> %d point(s), value %r  %s"
              % (len(back), back[0]["value"] if back else None,
                 "OVERWRITE" if len(back) == 1 else "DUPLICATED"))
        ok &= len(back) == 1

        for n in (1000, 20000, 50000):
            pts = [(old + 1000 + i, [("bulk", str(i))]) for i in range(n)]
            body = build_body(pts)
            t0 = time.time()
            client.call("POST", path, body)
            dt = time.time() - t0
            got = len(client.walk(pid, "bulk", old + 999, old + 1001 + n))
            print("3. batch n=%-6d body=%8d B -> %5.2f s, read back %d  %s"
                  % (n, len(body), dt, got, "ok" if got == n else "SHORT"))
            ok &= got == n
    finally:
        client.call("DELETE", "/api/device/" + pid)
        print("probe device deleted")
    print("\n%s" % ("all three hold" if ok else "SOMETHING DID NOT HOLD"))
    return 0 if ok else 1


def cmd_import(client, conn, args, state, dev) -> int:
    lo, hi, rows, _ = archive_bounds(conn)
    upper = args.upper or hi

    c = state.get("census")
    if not c:
        raise Fatal("no census in %s. Run --census first: the first "
                    "self-hosted timestamp per key is the seam, and one "
                    "imported point destroys the evidence for it."
                    % args.state)
    live_firsts = [t for t in c["first_live"].values() if t]
    refusal = upper_bound_refusal(hi, upper,
                                  min(live_firsts) if live_firsts else None)
    if refusal:
        raise Fatal(refusal)

    points = group_by_ts(conn.execute(
        "SELECT ts, key, value FROM telemetry WHERE ts <= ? ORDER BY ts, key",
        (upper,)))
    total_points = sum(len(v) for _, v in points)
    plan = list(batches(points))
    done = set(state.get("done", []))
    path = "/api/plugins/telemetry/DEVICE/%s/timeseries/ANY" % dev

    print("importing %d datapoints in %d batches, up to %s (%s)"
          % (total_points, len(plan), upper, fmt_ts(upper)))
    if done:
        print("ledger says %d batch(es) already written; re-running them is an "
              "overwrite, but skipping them is faster." % len(done))

    t0 = time.time()
    written = 0
    for i, batch in enumerate(plan):
        n = sum(len(v) for _, v in batch)
        if i in done:
            written += n
            continue
        body = build_body(batch)
        client.call("POST", path, body)
        done.add(i)
        state["done"] = sorted(done)
        save_state(Path(args.state), state)
        written += n
        elapsed = time.time() - t0
        print("  batch %3d/%d  %5d points  %7.1f KB  %s..%s  %6d/%d  %.0fs"
              % (i + 1, len(plan), n, len(body) / 1024.0,
                 fmt_ts(batch[0][0]), fmt_ts(batch[-1][0]),
                 written, total_points, elapsed))

    print("\nwrote %d datapoints in %d requests, %.1f s, %.1f MB sent"
          % (written, client.requests, time.time() - t0,
             client.bytes_sent / 1e6))
    print("Nothing is proved yet. Run --verify.")
    return 0


def cmd_verify(client, conn, args, state, dev) -> int:
    """Count both sides, per key, and compare a sample of the values.

    A summary saying "404142 written" is exactly the report this repository does
    not accept.
    """
    lo, hi, _, _ = archive_bounds(conn)
    upper = args.upper or hi
    want = archive_key_counts(conn, lo, upper)
    c = state.get("census") or {}
    pre = c.get("in_window", {})

    print("%-22s %9s %9s %9s  %s" % ("key", "archive", "server", "pre", ""))
    bad, checked = [], 0
    for key in sorted(want):
        got = len(client.walk(dev, key, lo - 1, upper + 1))
        before = pre.get(key, 0)
        expect = want[key] + before
        flag = "" if got == expect else "  MISMATCH"
        if got != expect:
            bad.append((key, want[key], got, before))
        checked += got
        print("%-22s %9d %9d %9d%s" % (key, want[key], got, before, flag))

    print("\n%d keys, %d archive rows, %d points counted on the server"
          % (len(want), sum(want.values()), checked))

    # A count can match while every value is the wrong TYPE, so sample.
    #
    # CHARACTER IDENTITY IS THE CHECK, and comparing types against the LIVE
    # series is deliberately not. A ThingsBoard datapoint's type is a property
    # of the literal the publisher sent, not of the key: 18 of these 60 keys
    # hold integer texts AND decimal texts in the archive, so `luminosity` is a
    # long at one instant and a double at the next, and the live series does the
    # same. A "type mismatch against the live point" is therefore noise.
    #
    # What character identity DOES prove is the failure that matters. If the
    # server had typed a 17-digit literal as a double it would stringify back as
    # the 16-digit shortest round-trip, and if it had typed `0` as a double it
    # would read back `0.0`. Either way the text differs and this catches it.
    # 74 702 of the archive's rows are in that 17-digit class.
    print("\nvalue spot-check (archive text vs the server's own stringification):")
    mism = 0
    for key in sorted(want):
        rows = conn.execute(
            "SELECT ts, value FROM telemetry WHERE key=? AND ts<=? "
            "ORDER BY ts LIMIT 1", (key, upper)).fetchall()
        rows += conn.execute(
            "SELECT ts, value FROM telemetry WHERE key=? AND ts<=? "
            "ORDER BY ts DESC LIMIT 1", (key, upper)).fetchall()
        rows += conn.execute(
            "SELECT ts, value FROM telemetry WHERE key=? AND ts<=? "
            "ORDER BY ts LIMIT 1 OFFSET ?",
            (key, upper, max(want[key] // 2, 0))).fetchall()
        # The longest literal this key ever held: the most fragile one, because
        # it is the one whose typing hangs on a digit.
        rows += conn.execute(
            "SELECT ts, value FROM telemetry WHERE key=? AND ts<=? "
            "ORDER BY LENGTH(value) DESC, ts LIMIT 1", (key, upper)).fetchall()
        for ts, text in rows:
            page = client.series(dev, key, ts, ts + 1, limit=2)
            got = page[0]["value"] if page else None
            if got != text:
                mism += 1
                print("   %-20s ts=%d archive=%r server=%r" % (key, ts, text, got))
    if mism == 0:
        print("   %d sampled values, every one character-identical."
              % (4 * len(want)))

    if bad:
        print("\n%d KEY(S) DISAGREE:" % len(bad))
        for key, w, g, b in bad:
            print("   %-22s archive %d + pre-existing %d = %d, server has %d "
                  "(%+d)" % (key, w, b, w + b, g, g - (w + b)))
        return 1
    if mism:
        print("\n%d sampled value(s) came back different." % mism)
        return 1
    print("\nEvery key agrees on both sides, and every sampled value is identical.")
    return 0


def cmd_seam(client, conn, args, state, dev) -> int:
    """The gap between the archive's last point and the first self-hosted one.

    Reported, never closed. CLAUDE.md treats an undocumented seam as a defect in
    three separate places, and an interpolated point is a measurement nobody
    took.
    """
    lo, hi, _, _ = archive_bounds(conn)
    upper = args.upper or hi
    c = state.get("census") or {}
    first_live = c.get("first_live") or {}
    if not first_live:
        raise Fatal("no census in %s; the first self-hosted timestamp per key "
                    "cannot be recovered once the import has run. Restore it "
                    "from the ledger of the run that did the import."
                    % args.state)

    last_archive = {k: t for k, t in conn.execute(
        "SELECT key, MAX(ts) FROM telemetry WHERE ts <= ? GROUP BY key", (upper,))}

    print("%-22s %-19s %-19s %12s" % ("key", "last imported", "first self-hosted",
                                      "gap"))
    gaps = []
    for key in sorted(set(last_archive) | set(first_live)):
        a = last_archive.get(key)
        b = first_live.get(key)
        if a and b:
            gap = (b - a) / 1000.0
            gaps.append(gap)
            print("%-22s %-19s %-19s %12s"
                  % (key, fmt_ts(a), fmt_ts(b), fmt_gap(gap)))
        elif a:
            print("%-22s %-19s %-19s %12s"
                  % (key, fmt_ts(a), "-- retired --", "n/a"))
        else:
            print("%-22s %-19s %-19s %12s"
                  % (key, "-- new key --", fmt_ts(b), "n/a"))
    if gaps:
        print("\n%d keys span the seam: smallest gap %s, largest %s."
              % (len(gaps), fmt_gap(min(gaps)), fmt_gap(max(gaps))))
        print("Nothing has been written into it, and nothing should be.")
    return 0


def cmd_attributes(client, conn, args, dev) -> int:
    rows = list(conn.execute(
        "SELECT scope, key, ts, value FROM attributes ORDER BY scope, key"))
    live = {a["key"]: a.get("value") for a in client.attributes(dev)}
    carry = []
    print("%-22s %-13s %-9s %-24s %s" % ("key", "scope", "verdict", "archived",
                                         "on the target now"))
    for scope, key, ts, value in rows:
        verdict, why = CLASSIFIED_ATTRIBUTES.get(key, ("unknown", "not classified"))
        print("%-22s %-13s %-9s %-24s %s"
              % (key, scope, verdict, value[:24],
                 "-- absent --" if key not in live else repr(live[key])))
        if verdict == "operator" and key not in live:
            carry.append((scope, key, value))
    print()
    for key, (verdict, why) in sorted(CLASSIFIED_ATTRIBUTES.items()):
        print("   %-22s %-9s %s" % (key, verdict, why))
    if not args.write:
        print("\n%d attribute(s) would be carried over: %s"
              % (len(carry), ", ".join(k for _, k, _ in carry) or "none"))
        print("Nothing written. Add --write to carry them.")
        return 0
    for scope, key, value in carry:
        body = json.dumps({key: json.loads(json_literal(value))}).encode()
        client.call("POST", "/api/plugins/telemetry/DEVICE/%s/%s" % (dev, scope),
                    body)
        print("wrote %s.%s = %s" % (scope, key, value))
    back = {a["key"]: a.get("value") for a in client.attributes(dev)}
    missing = [k for _, k, _ in carry if k not in back]
    if missing:
        print("NOT READ BACK: %s" % ", ".join(missing))
        return 1
    print("all %d read back" % len(carry))
    return 0


# --------------------------------------------------------------------------
# Self-test -- offline, no credential, no network
# --------------------------------------------------------------------------

def self_test() -> int:
    checks = []

    def ok(name, got, want):
        checks.append((name, got == want, got, want))

    # The literal rule is the one thing here that can silently change a series'
    # TYPE, so it is checked against the real shapes the archive holds.
    ok("long stays a long", json_literal("0"), "0")
    ok("negative long", json_literal("-3"), "-3")
    ok("double stays a double", json_literal("66.62857055664062"),
       "66.62857055664062")
    ok("17 digits are not shortened", json_literal("0.47609522938728333"),
       "0.47609522938728333")
    ok("boolean true", json_literal("true"), "true")
    ok("boolean false", json_literal("false"), "false")
    ok("a word is quoted", json_literal("clear"), '"clear"')
    ok("a sentence is quoted", json_literal("software (ESP.restart)"),
       '"software (ESP.restart)"')
    ok("a quote is escaped", json_literal('Zona "1"'), '"Zona \\"1\\""')
    # Everything that LOOKS numeric and is not JSON: quoted, not guessed at.
    ok("NaN is not a number", json_literal("NaN"), '"NaN"')
    ok("Infinity is not a number", json_literal("Infinity"), '"Infinity"')
    ok("a leading plus is not JSON", json_literal("+1"), '"+1"')
    ok("a leading zero is not JSON", json_literal("01"), '"01"')
    ok("a bare fraction is not JSON", json_literal(".5"), '".5"')
    ok("an empty value is a string", json_literal(""), '""')
    ok("a version is a string", json_literal("2.17.0"), '"2.17.0"')
    ok("exponent form survives", json_literal("1.5E-7"), "1.5E-7")

    # float() would break the one that matters; state it as a test so nobody
    # "simplifies" the emitter back into json.dumps(float(text)).
    ok("float() would have shortened it",
       repr(float("0.47609522938728333")) != "0.47609522938728333", True)

    body = build_body([(7, [("a", "1"), ("b", "x")]), (9, [("c", "true")])])
    ok("body shape",
       body, b'[{"ts":7,"values":{"a":1,"b":"x"}},{"ts":9,"values":{"c":true}}]')
    ok("body parses", json.loads(body)[0]["values"]["a"], 1)
    ok("body keeps the string a string", json.loads(body)[0]["values"]["b"], "x")
    ok("body keeps the boolean a boolean",
       json.loads(body)[1]["values"]["c"], True)

    rows = [(1, "a", "1"), (1, "b", "2"), (2, "a", "3")]
    ok("grouping collapses a timestamp", group_by_ts(rows),
       [(1, [("a", "1"), ("b", "2")]), (2, [("a", "3")])])
    ok("grouping an empty archive", group_by_ts([]), [])

    pts = [(i, [("k", "1"), ("j", "2")]) for i in range(10)]
    got = list(batches(pts, limit=5))
    ok("a batch never exceeds its limit",
       max(sum(len(v) for _, v in b) for b in got) <= 5, True)
    ok("no timestamp is split",
       all(len({ts for ts, _ in b}) == len(b) for b in got), True)
    ok("every point is in exactly one batch",
       sum(len(v) for b in got for _, v in b), 20)
    ok("batch order is preserved",
       [ts for b in got for ts, _ in b], list(range(10)))
    ok("one oversized timestamp still goes out",
       len(list(batches([(1, [("k", "1")] * 9)], limit=5))), 1)

    ok("a first census is allowed", census_refusal(None, False), None)
    ok("a stored census is not retaken",
       census_refusal({"first_live": {}}, False) is not None, True)
    ok("--recensus overrides it", census_refusal({"first_live": {}}, True), None)
    ok("an empty stored census is not a census",
       census_refusal({}, False), None)

    # The guard that cannot fire against this migration's data, so every branch
    # of it is reached here instead.
    ok("an upper bound inside the archive is allowed",
       upper_bound_refusal(100, 100, 200), None)
    ok("no live point at all is allowed",
       upper_bound_refusal(100, 100, None), None)
    ok("a bound past the archive is refused",
       upper_bound_refusal(100, 101, None) is not None, True)
    ok("a bound that reaches the first live point is refused",
       upper_bound_refusal(100, 100, 100) is not None, True)
    ok("a bound past the first live point is refused",
       upper_bound_refusal(100, 100, 99) is not None, True)
    ok("a bound one millisecond clear of the first live point is allowed",
       upper_bound_refusal(100, 100, 101), None)
    ok("the refusal names both numbers",
       "100" in upper_bound_refusal(100, 100, 100)
       and "101" not in upper_bound_refusal(100, 100, 100), True)

    # The ledger, against a real file: a stale plan must drop the batch list and
    # KEEP the census, because the census cannot be taken twice.
    tmp = Path(tempfile.gettempdir()) / ("tb_import.selftest.%d.json" % os.getpid())
    try:
        save_state(tmp, {"fingerprint": "old", "done": [1, 2, 3],
                         "census": {"first_live": {"k": 7}}})
        same = load_state(tmp, "old")
        ok("a matching ledger resumes", same["done"], [1, 2, 3])
        stale = load_state(tmp, "new")
        ok("a stale ledger drops the batch list", stale["done"], [])
        ok("a stale ledger keeps the census",
           stale["census"], {"first_live": {"k": 7}})
        ok("a missing ledger is empty but valid",
           load_state(tmp.with_name("nope.json"), "new"),
           {"fingerprint": "new", "done": [], "census": None})
        tmp.write_text("{not json", encoding="utf-8")
        ok("an unreadable ledger is not fatal",
           load_state(tmp, "new")["done"], [])
    finally:
        tmp.unlink(missing_ok=True)
        tmp.with_suffix(".tmp").unlink(missing_ok=True)

    ok("fingerprints differ on the device",
       plan_fingerprint("a", "s", 1, 2) != plan_fingerprint("b", "s", 1, 2), True)
    ok("fingerprints differ on the bound",
       plan_fingerprint("a", "s", 1, 2) != plan_fingerprint("a", "s", 9, 2), True)
    ok("fingerprints differ on the row count",
       plan_fingerprint("a", "s", 1, 2) != plan_fingerprint("a", "s", 1, 3), True)
    ok("the same plan fingerprints the same",
       plan_fingerprint("a", "s", 1, 2), plan_fingerprint("a", "s", 1, 2))

    ok("a two-minute gap reads in minutes", fmt_gap(122.0), "2.0 min")
    ok("a short gap reads in seconds", fmt_gap(45.0), "45 s")
    ok("a long gap reads in hours", fmt_gap(7200.0), "2.00 h")
    ok("a very long gap reads in days", fmt_gap(432000.0), "5.00 days")

    ok("every archived attribute is classified",
       sorted(CLASSIFIED_ATTRIBUTES), sorted([
           "active", "ambient_sensor", "current_fw_title", "current_fw_version",
           "deviceId", "hostname", "inactivityAlarmTime", "lastActivityTime",
           "lastConnectTime", "lastDisconnectTime", "wateringMs",
           "wateringTime"]))
    ok("only operator attributes are carried",
       sorted(k for k, (v, _) in CLASSIFIED_ATTRIBUTES.items() if v == "operator"),
       ["wateringMs", "wateringTime"])

    bad = [c for c in checks if not c[1]]
    for name, good, got, want in checks:
        if not good:
            print("FAIL %-40s got %r, want %r" % (name, got, want))
    print("tb_import --self-test: %d checks, %d failed"
          % (len(checks), len(bad)))
    return 1 if bad else 0


# --------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--server", default=DEFAULT_SERVER)
    p.add_argument("--device", default=DEFAULT_DEVICE)
    p.add_argument("--db", default=str(DEFAULT_DB))
    p.add_argument("--key-file", default=str(DEFAULT_KEY_FILE))
    p.add_argument("--state", default=str(DEFAULT_STATE))
    p.add_argument("--upper", type=int, default=0,
                   help="newest millisecond to write; defaults to the archive's "
                        "own newest point, which is the cutover")
    p.add_argument("--plan", action="store_true")
    p.add_argument("--census", action="store_true")
    p.add_argument("--recensus", action="store_true",
                   help="retake the census, overwriting the stored "
                        "seam; refused without it")
    p.add_argument("--probe", action="store_true")
    p.add_argument("--import", dest="do_import", action="store_true")
    p.add_argument("--verify", action="store_true")
    p.add_argument("--seam", action="store_true")
    p.add_argument("--attributes", action="store_true")
    p.add_argument("--write", action="store_true",
                   help="with --attributes, actually write the ones classified "
                        "as operator settings")
    p.add_argument("--self-test", action="store_true")
    args = p.parse_args()

    if args.self_test:
        return self_test()

    wants_archive = (args.plan or args.census or args.do_import or args.verify
                     or args.seam or args.attributes)
    if not (wants_archive or args.probe):
        p.print_help()
        return 2

    conn = open_archive(Path(args.db)) if wants_archive else None
    if args.plan:
        return cmd_plan(conn, args)

    client = Client(args.server, Path(args.key_file))
    client.login()
    me = client.whoami()
    print("%s as %s (%s)" % (args.server, me.get("email"), me.get("authority")))

    if args.probe:
        return cmd_probe(client, args)

    dev = client.device_by_name(args.device)["id"]
    meta = archive_identity(conn)
    if meta.get("device_name") != args.device:
        print("NOTE: the archive was taken from %r and is being written to %r."
              % (meta.get("device_name"), args.device))
    lo, hi, rows, _ = archive_bounds(conn)
    fingerprint = plan_fingerprint(args.device, args.server, args.upper or hi, rows)
    state = load_state(Path(args.state), fingerprint)

    if args.census:
        return cmd_census(client, conn, args, state, dev)
    if args.do_import:
        return cmd_import(client, conn, args, state, dev)
    if args.verify:
        return cmd_verify(client, conn, args, state, dev)
    if args.seam:
        return cmd_seam(client, conn, args, state, dev)
    if args.attributes:
        return cmd_attributes(client, conn, args, dev)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fatal as e:
        print("tb_import: %s" % e, file=sys.stderr)
        sys.exit(1)
    except ApiError as e:
        print("tb_import: %s" % e, file=sys.stderr)
        sys.exit(1)
