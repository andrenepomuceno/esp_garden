#!/usr/bin/env python3
"""Archives the device's own 60 s I/O history into SQLite, incrementally.

    python scripts/history_export.py --plan          # cost, cadence: no device
    python scripts/history_export.py                 # collect
    python scripts/history_export.py --stats         # what is in the archive
    python scripts/history_export.py --identity      # every seam, named
    python scripts/history_export.py --csv out.csv

WHY THIS EXISTS

scripts/tb_export.py archives ThingsBoard, which is the 300 s PUBLISH. The
device's own record is `GET /history.json` at `history.periodSec` - 60 s here -
and NOTHING has ever collected it. That one missing record blocks two questions
CLAUDE.md states outright:

  - seeding /moisture_model.bin, where MODEL_FILE_DECLINED says "the fix for
    both is to fit from GET /history.json ... the first thing to change when
    there is finally something to seed";
  - the three-minute plateau on the evening of 2026-09-03, which is below the
    300 s sampling and which the drying section says needs "GET /history.json at
    60 s, or a deliberate 1 Hz capture".

  device /history.json  ->  backups/history.sqlite  ->  moisture_fit --history-db

THE BUFFER IS FINITE AND IT RECYCLES

3000 records at 60 s on 6224 is 43.75 h of GUARANTEED retention (see
history_archive.retention_seconds - a whole segment is dropped at once, so the
low end of the swing is the number that matters). Anything not collected inside
that window is gone. `--plan` prints the cadence arithmetic for the device's own
configured numbers and is the one subcommand that touches no network.

WHAT IT COSTS THE DEVICE

The board serves HTTP from a single `async_tcp` task and CLAUDE.md records one
self-inflicted incident from a script polling it during other work. So: ONE
login, requests strictly SEQUENTIAL, never concurrent, one logout - one session
slot held for the length of the run and no more. Every run prints its own
request count and byte total, because a budget the tool does not measure is a
budget nobody checks. `window=` is never sent: it DECIMATES to fit the
200-record cap, and an archiver built on it would store one record in eight and
report a full collection.

Standard library only, same as tb_export.py and dev_server.py next door. GETs
only - it never writes a byte to the device.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
import time
import urllib.error
from pathlib import Path

import history_archive as ha
import history_store as store
from device_http import Device

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DB = ROOT / "backups" / "history.sqlite"
# The mDNS NAME, not an address, and the reason is a measurement. This constant
# and every mention of the garden in CLAUDE.md said 192.168.1.55 until
# 2026-09-17, when the FIRST live run of this tool timed out at TCP connect
# against it: the DHCP lease had moved the board to 192.168.1.242 and nothing
# anywhere would have said so - the address was written down once and
# re-derived by nobody. The name is published by the device itself, since
# webSetup() registers mDNS as <hostname>.local, so it tracks the lease.
# Verified through Python's own resolver rather than assumed:
# socket.getaddrinfo("espgarden1.local", 80) answered 192.168.1.242 in 0.148 s.
# Pass --host with a literal address on a host with no mDNS responder.
DEFAULT_DEVICE = "espgarden1.local"
DEFAULT_CREDENTIALS = ROOT / "data" / "config.json"

# How many consecutive failed runs the recommended cadence tolerates. Six is a
# judgement and not a measurement: at 6224's numbers it lands the interval on
# 6.25 h, which is four unattended runs a day, and a host that misses six of
# those in a row has a problem the archive cannot be the first to report.
TOLERATED_FAILURES = 6


class Fatal(Exception):
    """Something the operator has to fix. Printed as a sentence, not a trace."""


def open_archive(path, create=True):
    """history_archive.open_archive, with this tool's own error sentence."""
    try:
        return store.open_archive(path, create=create)
    except FileNotFoundError:
        raise Fatal(
            f"no archive at {path}; run scripts/history_export.py first")


# ---------------------------------------------------------------------------
# Identity - what makes a positional key self-describing
# ---------------------------------------------------------------------------


def identity_of(document):
    """The part of /config.json that says what a positional key MEANS.

    /history.json carries no names at all: `moisture` is an array of four and
    `relays` is a bitmask. Which pot slot 1 is in, and which pump bit 0 drives,
    lives only in config.json - and CLAUDE.md records that changing it
    renumbers everything after the deletion, with nothing in the series to say
    so. moisture_fit.py has to DERIVE that seam from the ThingsBoard archive's
    (relay, relayName) pairs and calls the result "deliberately blunt".

    Stamping the binding into every collection session makes the seam a stated
    fact instead of an inference: two consecutive sessions that disagree are the
    edit, to the minute of the run that noticed it.

    ONLY the identity fields are stored. The rest of the document is not this
    archive's business, and `backups/` is a directory CLAUDE.md forbids
    committing precisely because of what can end up in it - a masked secret is
    still a secret-shaped thing to keep out of a file nobody thinks about.
    """
    io = document.get("io", {}) or {}
    relays = []
    for index, entry in enumerate(io.get("relays", []) or []):
        entry = entry if isinstance(entry, dict) else {"pin": entry}
        relays.append({"index": index, "pin": entry.get("pin"),
                       "name": entry.get("name")})
    probes = []
    tuning = document.get("moisture", []) or []
    for index, entry in enumerate(io.get("soilMoisture", []) or []):
        entry = entry if isinstance(entry, dict) else {"pin": entry}
        cal = tuning[index] if index < len(tuning) else {}
        cal = cal if isinstance(cal, dict) else {}
        probes.append({
            "index": index, "pin": entry.get("pin"), "name": entry.get("name"),
            "relay": cal.get("relay", -1), "invert": cal.get("invert", True),
            "kind": cal.get("kind", ""),
            "dry": cal.get("dry", 0), "wet": cal.get("wet", 0),
        })
    history = document.get("history", {}) or {}
    return {
        "id": document.get("id"),
        "relays": relays,
        "probes": probes,
        "historyRecords": history.get("records"),
        "historyPeriodSec": history.get("periodSec"),
    }


def identity_seams(conn):
    """Every moment the stored binding changed, oldest first."""
    rows = conn.execute(
        "SELECT id, at, identity FROM sessions WHERE identity IS NOT NULL "
        "ORDER BY id"
    ).fetchall()
    seams = []
    previous = None
    for session_id, at, text in rows:
        current = json.loads(text)
        if previous is not None and current != previous:
            seams.append({"session": session_id, "at": at,
                          "was": previous, "now": current,
                          "changed": _what_changed(previous, current)})
        previous = current
    return seams


def _what_changed(before, after):
    changed = []
    for key in ("id", "historyRecords", "historyPeriodSec"):
        if before.get(key) != after.get(key):
            changed.append(f"{key} {before.get(key)!r} -> {after.get(key)!r}")
    for field in ("relays", "probes"):
        old = {e["index"]: e for e in before.get(field, [])}
        new = {e["index"]: e for e in after.get(field, [])}
        for index in sorted(set(old) | set(new)):
            if old.get(index) != new.get(index):
                changed.append(
                    f"{field}[{index}] {old.get(index)} -> {new.get(index)}")
    return changed


def latest_identity(conn):
    row = conn.execute(
        "SELECT identity FROM sessions WHERE identity IS NOT NULL "
        "ORDER BY id DESC LIMIT 1"
    ).fetchone()
    return json.loads(row[0]) if row else None


# ---------------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------------


def sync(args, out=sys.stderr):
    conn = open_archive(args.db)
    credentials = json.loads(
        Path(args.credentials).read_text(encoding="utf-8")
    ).get("ota", {})

    device = Device(args.host, timeout=args.timeout)
    device.login(credentials.get("username", ""), credentials.get("password", ""))
    try:
        document = device.config()
        identity = identity_of(document)
        stored_id = conn.execute(
            "SELECT value FROM meta WHERE key = 'device'").fetchone()
        if stored_id and identity.get("id") and stored_id[0] != identity["id"]:
            # The record identity is the timestamp, which says nothing about
            # WHICH device produced it. Two gardens in one file would interleave
            # into a series that looks continuous and is not.
            raise Fatal(
                f"this archive holds device {stored_id[0]} and the device at "
                f"{args.host} reports {identity['id']}. Use a separate --db: "
                "a record's identity is its timestamp, which cannot tell two "
                "devices apart"
            )

        period = int(identity.get("historyPeriodSec") or 60)
        known = store.newest_stored(conn)
        cursor = conn.execute(
            "INSERT INTO sessions (at, host, device, period_sec, identity) "
            "VALUES (?,?,?,?,?)",
            (int(time.time()), args.host, identity.get("id"), period,
             json.dumps(identity, sort_keys=True, separators=(",", ":"))),
        )
        session_id = cursor.lastrowid

        result = ha.collect(device.history, known_newest=known,
                            limit=args.limit, overlap=args.overlap)
        counts = store.merge(conn, result.records, session_id,
                          report=sys.stderr)
        conn.execute(
            "UPDATE sessions SET capacity = ?, stored = ?, requests = ?, "
            "added = ? WHERE id = ?",
            (result.capacity, result.stored, device.requests,
             counts["new"], session_id),
        )
        if identity.get("id"):
            conn.execute(
                "INSERT OR REPLACE INTO meta (key, value) VALUES ('device', ?)",
                (identity["id"],))
        conn.commit()
    finally:
        device.logout()

    _report_run(result, counts, device, known, period, out)
    _report_seam(conn, out)
    conn.close()
    return 1 if result.notes and not result.records else 0


def _report_run(result, counts, device, known, period, out):
    # The OBSERVED spacing, not the configured one. history.periodSec is what
    # the device was ASKED for; historyTaskHandler() reschedules from the end of
    # its callback, so a stalling background task delivers something longer, and
    # everything below that divides by a period should divide by what arrived.
    # They are printed together when they disagree, because that disagreement is
    # itself worth seeing.
    observed = ha.observed_period([r["t"] for r in result.records]) or period
    print(f"device buffer: {result.stored} / {result.capacity} records "
          f"at {observed:.0f} s"
          + (f" (config asks for {period} s)" if abs(observed - period) >= 1
             else ""), file=out)
    period = observed
    print(f"collected: {counts['new']} new, {counts['duplicate']} already held"
          + (f", {counts['conflict']} CONFLICTING" if counts["conflict"] else "")
          + (f", {counts['refused']} refused" if counts["refused"] else ""),
          file=out)
    print(f"cost: {device.requests} requests, "
          f"{device.bytes_read / 1024:.1f} KB, "
          f"{result.pages} history pages, {result.restarts} restarts",
          file=out)
    for note in result.notes:
        print(f"  note: {note}", file=out)
    if result.gap_before:
        lost, found = result.gap_before
        missing = max(0, int((found - lost) / max(period, 1)) - 1)
        print(
            f"GAP: the archive ends at {_stamp(lost)} and the oldest record the "
            f"device still holds is {_stamp(found)} - about {missing} records "
            "were recycled before anything collected them. That is not "
            "recoverable; collect more often.",
            file=out)
    if not result.records and not result.notes:
        print("nothing new: the archive is already level with the device",
              file=out)


def _report_seam(conn, out):
    seams = identity_seams(conn)
    if not seams:
        return
    print("\nIDENTITY SEAMS - a positional key changed meaning here", file=out)
    for seam in seams:
        print(f"  {_stamp(seam['at'])}  session {seam['session']}", file=out)
        for line in seam["changed"]:
            print(f"      {line}", file=out)


def _stamp(epoch):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(int(epoch)))


# ---------------------------------------------------------------------------
# --plan: the cost and the cadence, against no device at all
# ---------------------------------------------------------------------------


# Measured, not estimated: one record serialised by handleHistoryJson with two
# fitted probes, luminosity and a DHT, every other channel null. Recomputed by
# --plan from the shape rather than carried as a constant, so a record that
# grows a field does not leave this number behind.
def record_bytes(probes=2, scalars=("lum", "temp", "hum")):
    record = {
        "t": 1789000000, "relays": 0,
        "moisture": [75.52, 73.48, None, None][:4],
        "lum": 55.0, "temp": 25.5, "hum": 70.0,
        "water": None, "flow": None, "flowTotal": None, "float": None,
    }
    for index in range(4):
        if index >= probes:
            record["moisture"][index] = None
    for key in ("lum", "temp", "hum"):
        if key not in scalars:
            record[key] = None
    text = json.dumps(record, separators=(",", ":"))
    # handleHistoryJson writes floats with two decimals and null for NaN, which
    # is what json.dumps produces here; the comma between records is the one
    # byte it adds that a single dict does not carry.
    return len(text) + 1


def plan(args, capacity, period, out=sys.stdout):
    per_segment = ha.records_per_segment(capacity)
    low = per_segment * (ha.SEGMENTS - 1)
    retention = ha.retention_seconds(capacity, period)
    interval = ha.safe_interval_seconds(capacity, period, args.tolerate)
    step = args.limit - args.overlap
    full_pages = -(-capacity // step)
    per_run = -(-int(interval / max(period, 1)) // step) + 1
    size = record_bytes()

    print(f"buffer        {capacity} records at {period} s, "
          f"{ha.SEGMENTS} segments of {per_segment}", file=out)
    print(f"retention     {low}-{capacity} records held "
          f"({retention / 3600:.2f} h GUARANTEED, "
          f"{capacity * period / 3600:.2f} h at best)", file=out)
    print(f"cadence       every {interval / 3600:.2f} h tolerates "
          f"{args.tolerate} consecutive failed runs", file=out)
    print(f"first run     {full_pages} history pages + login + config + "
          f"logout = {full_pages + 3} requests, "
          f"~{capacity * size / 1024:.0f} KB", file=out)
    print(f"later runs    {per_run} history pages + 3 = {per_run + 3} requests, "
          f"~{interval / period * size / 1024:.0f} KB", file=out)
    print(f"session slots 1, held for the length of the run", file=out)
    print(file=out)
    print("The guaranteed figure is the LOW end of the swing. A whole segment "
          "is dropped\nat once, so planning against the capacity loses a record "
          "roughly one run in\neight - and nothing would report it.", file=out)


# ---------------------------------------------------------------------------
# --stats and --csv
# ---------------------------------------------------------------------------


def stats(args, out=sys.stdout):
    conn = open_archive(args.db, create=False)
    total, first, last = conn.execute(
        "SELECT COUNT(*), MIN(t), MAX(t) FROM records").fetchone()
    print(f"archive       {args.db}", file=out)
    print(f"records       {total}", file=out)
    if not total:
        return 0
    print(f"span          {_stamp(first)} .. {_stamp(last)} "
          f"({(last - first) / 3600:.1f} h)", file=out)
    times = [row[0] for row in conn.execute(
        "SELECT t FROM records ORDER BY t")]
    period = ha.observed_period(times)
    print(f"period        {period:.0f} s observed (median spacing)", file=out)
    gaps = ha.coverage_gaps(times, period or 60)
    missing = sum(gap["missing"] for gap in gaps)
    print(f"coverage      {len(gaps)} gaps, ~{missing} records missing "
          f"({100.0 * total / max(1, total + missing):.1f} % of the span)",
          file=out)
    for gap in gaps[-5:]:
        print(f"                {_stamp(gap['from'])} .. {_stamp(gap['to'])}"
              f"  {gap['seconds'] / 60:.0f} min, ~{gap['missing']} records",
              file=out)
    if gaps:
        print("              a gap is EITHER records nobody collected OR "
              "records the device\n              never wrote - "
              "historyTaskHandler() skips the append entirely while\n"
              "              the clock is unsynced. This cannot tell them "
              "apart.", file=out)
    conflicts = conn.execute("SELECT COUNT(*) FROM conflicts").fetchone()[0]
    if conflicts:
        print(f"CONFLICTS     {conflicts} timestamps arrived twice with "
              "different content", file=out)
    identity = latest_identity(conn)
    if identity:
        for probe in identity["probes"]:
            events = len(ha.events_for_relay(
                store.read_records(conn), probe["relay"], period or 60))
            print(f"probe {probe['index']}       {probe['name']!r} "
                  f"pin {probe['pin']} relay {probe['relay']}: "
                  f"{events} watering events in the archive", file=out)
    sessions = conn.execute("SELECT COUNT(*), SUM(requests) FROM sessions") \
        .fetchone()
    print(f"sessions      {sessions[0]} runs, {sessions[1] or 0} requests "
          "issued in total", file=out)
    conn.close()
    return 0


def export_csv(args, out=sys.stderr):
    conn = open_archive(args.db, create=False)
    records = store.read_records(conn)
    path = Path(args.csv)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["t", "iso", "relays", "m0", "m1", "m2", "m3", "lum",
                         "temp", "hum", "water", "flow", "flowTotal", "float"])
        for record in records:
            moisture = list(record["moisture"]) + [None] * 4
            writer.writerow([
                record["t"], _stamp(record["t"]), record["relays"],
                *moisture[:4], record["lum"], record["temp"], record["hum"],
                record["water"], record["flow"], record["flowTotal"],
                record["float"]])
    print(f"wrote {len(records)} records to {path}", file=out)
    conn.close()
    return 0


# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default=str(DEFAULT_DB))
    parser.add_argument("--host", "--device", dest="host",
                        default=DEFAULT_DEVICE)
    parser.add_argument("--credentials", default=str(DEFAULT_CREDENTIALS))
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--limit", type=int, default=ha.MAX_RESPONSE,
                        help="records per request; the device caps it at 200")
    parser.add_argument("--overlap", type=int, default=ha.PAGE_OVERLAP,
                        help="records two consecutive pages must share")
    parser.add_argument("--tolerate", type=int, default=TOLERATED_FAILURES,
                        help="--plan: consecutive failed runs to survive")
    parser.add_argument("--plan", action="store_true",
                        help="print the cost and cadence; touches no network")
    parser.add_argument("--capacity", type=int, default=3000,
                        help="--plan: the device's history.records")
    parser.add_argument("--period", type=int, default=60,
                        help="--plan: the device's history.periodSec")
    parser.add_argument("--stats", action="store_true")
    parser.add_argument("--identity", action="store_true")
    parser.add_argument("--csv")
    args = parser.parse_args()

    args.limit = max(2, min(args.limit, ha.MAX_RESPONSE))
    args.overlap = max(1, min(args.overlap, args.limit - 1))

    try:
        if args.plan:
            return plan(args, args.capacity, args.period)
        if args.stats:
            return stats(args)
        if args.identity:
            conn = open_archive(args.db, create=False)
            seams = identity_seams(conn)
            if not seams:
                print("no seam: every session saw the same binding")
            _report_seam(conn, sys.stdout)
            conn.close()
            return 0
        if args.csv:
            return export_csv(args)
        return sync(args)
    except Fatal as error:
        print(f"history_export: {error}", file=sys.stderr)
        return 2
    except urllib.error.HTTPError as error:
        print(f"history_export: {error.code} from the device: "
              f"{error.read().decode('utf-8', 'replace')[:200]}",
              file=sys.stderr)
        return 2
    except OSError as error:
        print(f"history_export: cannot reach the device ({error})",
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
