#!/usr/bin/env python3
"""Reads backups/telemetry.sqlite in a browser: charts, inventory, boots, gaps.

WHY THIS EXISTS

`tb_export.py --stats` prints 43 lines and answers one question -- how much is
stored. The questions that actually get asked of a field device's archive are
shaped differently: what did moisture2 do last week, which keys STOPPED
arriving, how often did the board reboot, and where are the holes. Every one of
those is a picture, and none of them is a column of numbers.

  backups/telemetry.sqlite  ->  http://127.0.0.1:8090/

Standard library only, same as dev_server.py and build_assets.py next door, and
a different default port from dev_server.py's 8080 so both can run at once.

WHAT IT REFUSES TO DO

  * It never forward-fills. A missing sample is missing, the line breaks, and
    the 18.9 h board-swap gap reads as 18.9 h of nothing rather than a straight
    line between its endpoints. `tb_export.export_csv` leaves the same cells
    empty for the same reason.
  * It never draws across the probe index seam. SEAM_MS and SEAM_KEYS are
    IMPORTED from tb_export rather than restated here -- one constant, one
    place, and no way for the two tools to disagree about when the hardware
    changed underneath the key names.
  * It opens the archive read-only (`mode=ro`, `query_only`), so a concurrent
    `tb_export.py` run cannot be corrupted by it. The sync button is the one
    exception, and it goes through tb_export's own writer.

Run:  python scripts/telemetry_ui.py
      python scripts/telemetry_ui.py --port 9100 --db /path/to/telemetry.sqlite
      python scripts/telemetry_ui.py --no-sync      # never touch the network
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import statistics
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

# Plain sibling module: this file is run as a script, so scripts/ is sys.path[0]
# -- the same import shape dev_server.py uses for sim_*.py. tb_export is
# imported for its SEAM constants and its sync, never re-implemented: a second
# copy of either would be a second thing to keep true.
import tb_export
from telemetry_page import PAGE

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DB = ROOT / "backups" / "telemetry.sqlite"
DEFAULT_PORT = 8090

# 30 days of `temperature` is ~13 000 points and a chart is ~900 px wide, so
# beyond this the extra points are drawn on top of each other and cost only
# transfer. Buckets carry min/max as well as the mean -- see bucket() for why
# that is not optional.
TARGET_POINTS = 1500

# A break is drawn when the interval between two stored points exceeds this
# multiple of the series' own LOCAL interval. Relative, not absolute, because
# the publish period is configuration -- `mqtt.publishSec` -- and a fixed
# threshold would call every ordinary tick a gap the moment it is raised.
GAP_FACTOR = 3.0

# ...and LOCAL rather than the median over the whole window, because the period
# has already changed inside this archive: firmware 2.8.0 took it from 60 s to
# 300 s at 18:43 on 2026-09-03, and against the window median every single
# publish after that instant reads as a five-minute hole. The baseline is the
# larger of the medians either side of a point, so a step in cadence is a step
# and not a hundred gaps -- and a real outage, which has ordinary intervals on
# both sides of it, still breaks the line.
GAP_LOCAL_K = 15
GAP_LOCAL_STRIDE = 8

# ...and at least this multiple of the bucket width, so a coarse window does not
# shred the line on holes it has no resolution to show. 1.5 rather than 3: it is
# the smallest value that cannot fire on ordinary jitter (one sample per bucket)
# while still breaking on a hole of two buckets.
GAP_BUCKETS = 1.5

# A key is DEAD when nothing has arrived for far longer than its own cadence.
# Relative again, and generously: `relay1Event` fires when a relay runs, so its
# mean interval is hours and three quiet days is not a fault, while `flowRate`
# publishes every ~74 s and three days of silence is the sensor being gone.
# The floor stops a key with two points ever from being called dead at 20 min.
DEAD_FACTOR = 10.0
DEAD_FLOOR_MS = 3600 * 1000

# Text keys have no y axis; they are drawn as a tick per event. A window holding
# more than this many is reported truncated rather than silently thinned.
MAX_EVENTS = 2000

# What the chart opens on. Two probes and the two DHT channels: the four keys
# that are live on this board today.
DEFAULT_KEYS = ("moisture1", "moisture2", "temperature", "airHumidity")

# `true`/`false` are stored verbatim by as_text(), and a relay state is exactly
# the thing worth seeing on a chart -- so they are numbers here, drawn as a step
# rather than a slope, because a contact does not ramp.
BOOLEANS = {"true": 1.0, "false": 0.0}


class Fatal(Exception):
    """Something the operator has to fix. Printed as a sentence, not a trace."""


# --------------------------------------------------------------------------
# Store
# --------------------------------------------------------------------------

def open_ro(path: Path) -> sqlite3.Connection:
    """A connection that CANNOT write, and waits rather than failing.

    `mode=ro` plus `query_only` is belt and braces: the archive is the record of
    everything the garden has measured and a browser refresh is not a reason to
    risk it. `busy_timeout` matters because tb_export.py may be committing a
    page while a request is being served -- in rollback-journal mode that blocks
    readers, and blocking for a moment is the right answer where "database is
    locked" is not.
    """
    conn = sqlite3.connect("file:%s?mode=ro" % path.as_posix(), uri=True,
                           timeout=10.0, check_same_thread=False)
    conn.execute("PRAGMA query_only = ON")
    conn.execute("PRAGMA busy_timeout = 10000")
    return conn


def as_number(text: str):
    """A float, or None when the stored value is not one.

    Everything in the archive is TEXT on purpose (see tb_export.as_text), so the
    cast happens here, where the caller knows which column it is casting.
    """
    if text in BOOLEANS:
        return BOOLEANS[text]
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def classify(conn: sqlite3.Connection, key: str) -> str:
    """`num`, `bool` or `text`, decided from the values rather than the name.

    Sampled from both ends: a key that changed shape part-way through has to
    come out `text`, and reading only the oldest rows would miss it.
    """
    seen = [r[0] for r in conn.execute(
        "SELECT value FROM telemetry WHERE key = ? ORDER BY ts LIMIT 40", (key,))]
    seen += [r[0] for r in conn.execute(
        "SELECT value FROM telemetry WHERE key = ? ORDER BY ts DESC LIMIT 40",
        (key,))]
    if not seen:
        return "text"
    if all(v in BOOLEANS for v in seen):
        return "bool"
    return "num" if all(as_number(v) is not None for v in seen) else "text"


def summary(conn: sqlite3.Connection, db: Path, sync_enabled: bool) -> dict:
    row = conn.execute("SELECT COUNT(*), MIN(ts), MAX(ts) FROM telemetry"
                       ).fetchone()
    rows, first, last = row[0], row[1], row[2]
    if not rows:
        raise Fatal("the archive at %s holds no telemetry; run "
                    "`python scripts/tb_export.py` first" % db)
    keys = conn.execute("SELECT COUNT(DISTINCT key) FROM telemetry").fetchone()[0]
    return {
        "rows": rows, "keys": keys, "first": first, "last": last,
        # Age of the newest datapoint, against the wall clock rather than
        # against the archive: "the newest row is the newest row" is true of a
        # stale archive too, and staleness is the thing this has to surface.
        "age_ms": max(0, int(time.time() * 1000) - last),
        "device": tb_export.meta_get(conn, "device_name") or "",
        "last_sync_ms": int(tb_export.meta_get(conn, "last_sync_ms") or 0),
        "db": str(db), "db_bytes": db.stat().st_size,
        "sync_enabled": sync_enabled,
        "seam_ms": tb_export.SEAM_MS, "seam_keys": list(tb_export.SEAM_KEYS),
    }


def inventory(conn: sqlite3.Connection) -> list:
    """Every key, its span, its cadence, its last value, and whether it is dead.

    One pass for the aggregates and one small query per key for the last value,
    which is what makes the Latest view free: the same rows answer both.
    """
    newest = conn.execute("SELECT MAX(ts) FROM telemetry").fetchone()[0]
    out = []
    for key, count, first, last in conn.execute(
            "SELECT key, COUNT(*), MIN(ts), MAX(ts) FROM telemetry "
            "GROUP BY key ORDER BY key"):
        span = max(last - first, 0)
        cadence = span / (count - 1) if count > 1 else 0
        age = newest - last
        value = conn.execute(
            "SELECT value FROM telemetry WHERE key = ? ORDER BY ts DESC LIMIT 1",
            (key,)).fetchone()[0]
        out.append({
            "key": key, "kind": classify(conn, key), "count": count,
            "first": first, "last": last, "age_ms": age,
            "cadence_ms": int(cadence),
            # A single stored point spans no time, and dividing by the epsilon
            # tb_export uses would report it as a billion a day.
            "per_day": (count / (span / 86400000.0)) if span else 0.0,
            "last_value": value,
            "dead": age > max(DEAD_FACTOR * cadence, DEAD_FLOOR_MS),
        })
    return out


# --------------------------------------------------------------------------
# Series
# --------------------------------------------------------------------------

def local_baselines(diffs: list) -> list:
    """Per-interval "what is normal HERE", as max(median left, median right).

    `max` rather than `min` is the whole point: at a step in cadence one side
    still holds the old period, and taking the smaller would flag every point of
    the new regime. A real outage has the ordinary period on both sides, so the
    max is ordinary too and the hole still breaks the line.

    Recomputed every GAP_LOCAL_STRIDE intervals and held in between -- a cadence
    that changes faster than eight samples is not a cadence, and computing it
    per point costs half a second on a 30-day window.
    """
    n = len(diffs)
    out = [0.0] * n
    held = 0.0
    for i in range(n):
        if i % GAP_LOCAL_STRIDE == 0:
            left = diffs[max(0, i - GAP_LOCAL_K):i] or [diffs[i]]
            right = diffs[i + 1:i + 1 + GAP_LOCAL_K] or [diffs[i]]
            held = max(sorted(left)[len(left) // 2],
                       sorted(right)[len(right) // 2])
        out[i] = held
    return out


def bucket(points: list, target: int, seam: bool) -> dict:
    """Downsamples to `target` buckets, carrying min and max as well as the mean.

    The mean alone is a lie at 30 days: 13 000 samples into 1500 buckets is nine
    to a bucket, and the 45.04 C spike -- one sample -- is averaged into
    invisibility at exactly the zoom level someone is hunting for it at. So each
    bucket reports its extremes and the chart draws them as a band.

    A bucket's timestamp is the MEAN of its samples, not the bucket boundary: a
    half-empty bucket at the edge of a gap then sits where its data actually is.

    The last element of each point is the break flag. Nothing is interpolated
    and nothing is carried forward -- a gap is emitted as a break and the chart
    lifts the pen.
    """
    n = len(points)
    if not n:
        return {"points": [], "bucket_ms": 0, "downsampled": False,
                "gap_typical_ms": 0, "median_dt_ms": 0}

    diffs = [b[0] - a[0] for a, b in zip(points, points[1:])]
    median_dt = statistics.median(diffs) if diffs else 0
    span = points[-1][0] - points[0][0]
    bucket_ms = 0 if (n <= target or span <= 0) else max(1, span // target + 1)
    baselines = local_baselines(diffs)
    # Reported for the reader, not applied: it is the threshold this window
    # WOULD use if the cadence were uniform, and it is the number to check the
    # median against. What each interval is actually judged by is local.
    gap_typical = max(GAP_FACTOR * median_dt, GAP_BUCKETS * bucket_ms)

    out = []
    prev_ts = None
    idx = None
    acc = None  # [sum_ts, sum_v, n, lo, hi, brk]
    for i, (ts, value) in enumerate(points):
        brk = prev_ts is not None and (ts - prev_ts) > max(
            GAP_FACTOR * baselines[i - 1], GAP_BUCKETS * bucket_ms)
        # The seam is a break whatever the interval says. `moisture2` publishes
        # straight through it -- the last old sample and the first new one are
        # 76 s apart -- so nothing in the timing would ever reveal that the two
        # sides are different physical sensors.
        if seam and prev_ts is not None and prev_ts < tb_export.SEAM_MS <= ts:
            brk = True
        prev_ts = ts
        # Not downsampling means one bucket per point, and `ts` is unique per
        # key -- PRIMARY KEY(key, ts) -- so it is already that identity.
        here = (ts // bucket_ms) if bucket_ms else ts
        if acc is not None and here != idx:
            out.append(flush(acc))
            acc = None
        if acc is None:
            idx, acc = here, [0, 0.0, 0, value, value, brk]
        acc[0] += ts
        acc[1] += value
        acc[2] += 1
        acc[3] = min(acc[3], value)
        acc[4] = max(acc[4], value)
        acc[5] = acc[5] or brk
    if acc is not None:
        out.append(flush(acc))
    return {"points": out, "bucket_ms": int(bucket_ms),
            "downsampled": bool(bucket_ms), "gap_typical_ms": int(gap_typical),
            "median_dt_ms": int(median_dt)}


def flush(acc: list) -> list:
    """One bucket as [ts, mean, min, max, samples, break]."""
    return [acc[0] // acc[2], round(acc[1] / acc[2], 4), round(acc[3], 4),
            round(acc[4], 4), acc[2], 1 if acc[5] else 0]


def series(conn, keys: list, lo: int, hi: int, kinds: dict, dead: dict) -> dict:
    scanned = 0
    started = time.time()
    out = []
    for key in keys:
        kind = kinds.get(key)
        if kind is None:
            continue
        rows = conn.execute(
            "SELECT ts, value FROM telemetry WHERE key = ? AND ts BETWEEN ? AND ? "
            "ORDER BY ts", (key, lo, hi)).fetchall()
        scanned += len(rows)
        entry = {"key": key, "kind": kind, "dead": dead.get(key, False),
                 "raw": len(rows), "seam": None}
        if key in tb_export.SEAM_KEYS and lo <= tb_export.SEAM_MS <= hi:
            entry["seam"] = {
                "ts": tb_export.SEAM_MS,
                # The wording is tb_export's, deliberately: the operator reads
                # the same sentence from the CSV warning and from the chart.
                "note": "probe index shift -- moisture3 and relay4 stop here, "
                        "and moisture2 changes meaning",
            }
        if kind == "text":
            events = [[ts, value] for ts, value in rows]
            entry["truncated"] = len(events) > MAX_EVENTS
            entry["events"] = events[-MAX_EVENTS:]
            entry["shown"] = len(entry["events"])
        else:
            numeric = [(ts, as_number(v)) for ts, v in rows]
            numeric = [(ts, v) for ts, v in numeric if v is not None]
            entry["skipped"] = len(rows) - len(numeric)
            entry.update(bucket(numeric, TARGET_POINTS,
                                entry["seam"] is not None))
            entry["shown"] = len(entry["points"])
        out.append(entry)
    return {"series": out, "rows_scanned": scanned,
            "query_ms": int((time.time() - started) * 1000),
            "since": lo, "until": hi}


# --------------------------------------------------------------------------
# Boots and gaps
# --------------------------------------------------------------------------

def boots(conn, lo: int, hi: int, gap_min: int) -> dict:
    """bootReason events, and every hole in the archive over `gap_min`.

    A gap is measured across ALL keys, not one: the question is whether the
    device was publishing, and any key arriving is proof that it was.
    """
    heap = dict(conn.execute(
        "SELECT ts, value FROM telemetry WHERE key = 'bootFreeHeapKB' "
        "AND ts BETWEEN ? AND ?", (lo, hi)))
    rows = conn.execute(
        "SELECT ts, value FROM telemetry WHERE key = 'bootReason' "
        "AND ts BETWEEN ? AND ? ORDER BY ts", (lo, hi)).fetchall()
    by_reason = {}
    events = []
    for ts, reason in rows:
        by_reason[reason] = by_reason.get(reason, 0) + 1
        kb = as_number(heap.get(ts, ""))
        events.append([ts, reason, int(kb) if kb is not None else None])

    stamps = [r[0] for r in conn.execute(
        "SELECT DISTINCT ts FROM telemetry WHERE ts BETWEEN ? AND ? ORDER BY ts",
        (lo, hi))]
    threshold = gap_min * 60000
    gaps = [[a, b] for a, b in zip(stamps, stamps[1:]) if b - a > threshold]

    # The publish period is configuration, so the threshold that separates an
    # outage from an ordinary tick moves with it. Say what the data's own
    # cadence is rather than letting a 5-minute default quietly list every
    # 300 s publish as a hole.
    diffs = [b - a for a, b in zip(stamps, stamps[1:])]
    typical = int(statistics.median(diffs)) if diffs else 0
    note = ("The archive's typical interval here is %d s, so a threshold at or "
            "below that lists ordinary publishes as gaps." % (typical // 1000))
    return {"boots": events, "by_reason": by_reason, "gaps": gaps,
            "gap_note": note, "typical_ms": typical, "gap_min": gap_min}


# --------------------------------------------------------------------------
# Sync
# --------------------------------------------------------------------------

# Serialised behind one lock, run off the request thread, and it never touches
# the credential: tb_export reads that file itself, inside its own process
# space, and the only thing that comes back here is a count of rows. Nothing in
# this module prints, logs or serialises a token -- the proof that auth worked
# is the row count, exactly as tb_export's proof is an HTTP status.
SYNC = {
    "lock": threading.Lock(), "running": False, "finished": 0,
    "message": "", "added": 0, "error": "",
}


def sync_worker(db: Path, device: str) -> None:
    conn = None
    try:
        conn = tb_export.open_db(db)
        tb_export.whoami()
        row = tb_export.resolve_device(device)
        keys = sorted(tb_export.timeseries_keys(row["id"]))
        before = tb_export.total_rows(conn)
        started = time.time()
        report = tb_export.sync(conn, row["id"], keys, None, None, False,
                                quiet=True)
        after = tb_export.total_rows(conn)
        SYNC["added"] = after - before
        top = sorted(((n, k) for k, n in report["per_key"].items() if n),
                     reverse=True)[:4]
        detail = ", ".join("%s +%d" % (k, n) for n, k in top)
        SYNC["message"] = ("synced %s: %d new rows in %.0f s (%d keys)%s"
                           % (device, after - before, time.time() - started,
                              report["keys"],
                              (" — " + detail) if detail else ""))
        SYNC["error"] = ""
    except Exception as e:            # reported to the page, never swallowed
        SYNC["error"] = str(e)
        SYNC["message"] = "sync failed: %s" % e
    finally:
        if conn is not None:
            conn.close()
        SYNC["finished"] = int(time.time() * 1000)
        SYNC["running"] = False
        SYNC["lock"].release()


def sync_start(db: Path, device: str) -> bool:
    """Non-blocking: a second click while one is running is refused, not queued.

    Two concurrent syncs would each page the same windows and fight over the
    same writer, which is a slow way to produce exactly the rows one of them
    already had.
    """
    if not SYNC["lock"].acquire(blocking=False):
        return False
    SYNC["running"] = True
    SYNC["message"] = "syncing…"
    threading.Thread(target=sync_worker, args=(db, device), daemon=True).start()
    return True


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "ESPGardenTelemetryUI/1.0"
    db_path: Path = DEFAULT_DB
    device: str = tb_export.DEFAULT_DEVICE
    sync_enabled: bool = True
    # Rebuilt after a sync rather than cached for the process lifetime: the
    # whole point of the button is that the answer changes.
    _cache_lock = threading.Lock()
    _cache: dict = {}

    def log_message(self, format, *args) -> None:  # noqa: A002 - stdlib signature
        pass

    def _send(self, status, body=b"", ctype="text/plain; charset=utf-8") -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json(self, payload, status=200) -> None:
        self._send(status, json.dumps(payload).encode("utf-8"),
                   "application/json; charset=utf-8")

    def _inventory(self, conn) -> list:
        """Memoised on (rows, newest), so a page refresh is one cheap query.

        classify() samples 80 values per key; over 43 keys that is a real cost
        on every request and none of it changes until something is written.
        """
        stamp = conn.execute(
            "SELECT COUNT(*), MAX(ts) FROM telemetry").fetchone()
        with self._cache_lock:
            if Handler._cache.get("stamp") == stamp:
                return Handler._cache["keys"]
        rows = inventory(conn)
        with self._cache_lock:
            Handler._cache = {"stamp": stamp, "keys": rows}
        return rows

    @staticmethod
    def _int(query, name, default):
        raw = query.get(name, [""])[0]
        try:
            return int(float(raw))
        except (TypeError, ValueError):
            return default

    def do_GET(self) -> None:  # noqa: N802 - stdlib signature
        url = urlparse(self.path)
        query = parse_qs(url.query)
        if url.path in ("/", "/index.html"):
            self._send(HTTPStatus.OK, PAGE.encode("utf-8"),
                       "text/html; charset=utf-8")
            return
        if url.path == "/api/sync":
            self._json({"running": SYNC["running"], "message": SYNC["message"],
                        "added": SYNC["added"], "error": SYNC["error"],
                        "finished": SYNC["finished"],
                        "enabled": self.sync_enabled})
            return

        conn = open_ro(self.db_path)
        try:
            if url.path == "/api/summary":
                self._json(summary(conn, self.db_path, self.sync_enabled))
            elif url.path == "/api/inventory":
                rows = self._inventory(conn)
                have = {r["key"] for r in rows}
                self._json({"keys": rows,
                            "default_keys": [k for k in DEFAULT_KEYS
                                             if k in have]})
            elif url.path == "/api/series":
                rows = self._inventory(conn)
                kinds = {r["key"]: r["kind"] for r in rows}
                dead = {r["key"]: r["dead"] for r in rows}
                keys = [k for k in query.get("keys", [""])[0].split(",")
                        if k in kinds]
                span = conn.execute(
                    "SELECT MIN(ts), MAX(ts) FROM telemetry").fetchone()
                lo = self._int(query, "since", span[0])
                hi = self._int(query, "until", span[1])
                self._json(series(conn, keys, lo, hi, kinds, dead))
            elif url.path == "/api/boots":
                span = conn.execute(
                    "SELECT MIN(ts), MAX(ts) FROM telemetry").fetchone()
                self._json(boots(conn, self._int(query, "since", span[0]),
                                 self._int(query, "until", span[1]),
                                 max(1, self._int(query, "gap_min", 5))))
            else:
                self._json({"error": "no such endpoint"}, HTTPStatus.NOT_FOUND)
        except Fatal as e:
            self._json({"error": str(e)}, HTTPStatus.SERVICE_UNAVAILABLE)
        except sqlite3.Error as e:
            self._json({"error": "sqlite: %s" % e},
                       HTTPStatus.SERVICE_UNAVAILABLE)
        finally:
            conn.close()

    def do_POST(self) -> None:  # noqa: N802
        if urlparse(self.path).path != "/api/sync":
            self._json({"error": "no such endpoint"}, HTTPStatus.NOT_FOUND)
            return
        if not self.sync_enabled:
            self._json({"started": False,
                        "message": "sync disabled (--no-sync)"},
                       HTTPStatus.FORBIDDEN)
            return
        if sync_start(self.db_path, self.device):
            self._json({"started": True, "message": SYNC["message"]})
        else:
            # 409, not 202: a click that silently joined a running sync would
            # report the previous run's numbers as its own.
            self._json({"started": False, "message": SYNC["message"]},
                       HTTPStatus.CONFLICT)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Browse backups/telemetry.sqlite in a browser.")
    ap.add_argument("--db", default=str(DEFAULT_DB),
                    help="SQLite archive (default backups/telemetry.sqlite)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help="default %d, deliberately not dev_server.py's 8080"
                         % DEFAULT_PORT)
    ap.add_argument("--device", default=tb_export.DEFAULT_DEVICE,
                    help="ThingsBoard device name used by the sync button")
    ap.add_argument("--no-sync", action="store_true",
                    help="disable the sync button; touch no network at all")
    args = ap.parse_args()

    db = Path(args.db).resolve()
    if not db.is_file():
        raise Fatal("no archive at %s.\nRun `python scripts/tb_export.py` to "
                    "create it, or point --db at an existing one." % db)

    conn = open_ro(db)
    try:
        info = summary(conn, db, not args.no_sync)
    finally:
        conn.close()

    Handler.db_path = db
    Handler.device = args.device
    Handler.sync_enabled = not args.no_sync

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print("telemetry_ui: %s, %d rows over %d keys, %s .. %s"
          % (db, info["rows"], info["keys"], tb_export.iso(info["first"]),
             tb_export.iso(info["last"])))
    print("telemetry_ui: newest datapoint is %.1f h old"
          % (info["age_ms"] / 3600000.0))
    print("telemetry_ui: serving http://%s:%d/  (read-only; sync %s)"
          % (args.host, args.port, "off" if args.no_sync else "on"))
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\ntelemetry_ui: shutting down")
        server.server_close()
    return 0


if __name__ == "__main__":
    import sys
    try:
        sys.exit(main())
    except Fatal as e:
        print("telemetry_ui: %s" % e, file=sys.stderr)
        sys.exit(1)
