#!/usr/bin/env python3
"""Archives the device's ThingsBoard telemetry into SQLite, and exports CSV.

WHY THIS EXISTS

ThingsBoard Cloud's free tier keeps telemetry for a limited retention window
and the device publishes ~1400 points per key per day. Everything the garden
has ever measured lives in one tenant that can silently age it out, so the
answer to "what did the soil do last month" has to be held somewhere that is
ours. This pulls it down into a file.

  SQLite is the store of record, `--csv` is a view of it.

Standard library only, same as dev_server.py and build_assets.py next door.
Only GET requests are issued: the account is a tenant admin, so the credential
could write and delete, and nothing here should ever be the reason it does.

THE CREDENTIAL

Read from `.chave_tb_claude` at the repo root (override with TB_API_KEY_FILE),
INSIDE this process, and never printed, never logged, never put on a command
line. A command line lands in the process list and in shell history; a log line
lands in a transcript. Neither is a place for a token that opens a live tenant.
Proof that auth worked is an HTTP status and the account's email, never the
token.

The header is `X-Authorization: ApiKey <token>`. A `tb_...` user API key is not
a JWT: sent as `Bearer` it returns `401 {"message":"Invalid username or
password","errorCode":10}`, which reads like a bad credential rather than a
wrong scheme and is exactly how this gets misdiagnosed.

WHAT IT DOES

  ThingsBoard  ->  backups/telemetry.sqlite  ->  --csv out.csv

Incremental by default: per key it resumes from the newest timestamp already
stored and fetches forward. A first run backfills from the oldest datapoint the
cloud still holds. Re-running is a no-op, because every insert is
`INSERT OR IGNORE` against `PRIMARY KEY(key, ts)`.

Run:  python scripts/tb_export.py                    # sync
      python scripts/tb_export.py --stats            # what is in the archive
      python scripts/tb_export.py --csv out.csv --since 2026-09-01
      python scripts/tb_export.py --csv out.csv --keys moisture1,moisture2
      python scripts/tb_export.py --no-sync --csv out.csv    # offline export
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

BASE = "https://thingsboard.cloud"
DEFAULT_DEVICE = "espgarden1"
DEFAULT_DB = ROOT / "backups" / "telemetry.sqlite"
DEFAULT_KEY_FILE = ROOT / ".chave_tb_claude"

# Measured against the live endpoint, not read off a doc page. Asking for more
# is a hard refusal -- `400 Query limit 100000 exceeds 50000 threshold` -- and
# asking for less silently TRUNCATES: a window holding 13173 points answered
# with exactly 10000 at limit=10000, oldest first, with no flag saying so. That
# is the whole reason this file pages by time instead of trusting one call: a
# truncation nobody notices produces an archive with holes that looks complete.
PAGE_LIMIT = 50000

# Also measured: the range is [startTs, endTs). startTs == endTs == a stored
# point returns nothing, while startTs == that point returns it. So a resume
# uses MAX(ts) + 1 and a window end uses `until + 1` to include its own edge.
#
# And `orderBy=ASC` truncation keeps the OLDEST points, which is what makes
# paging forward from the last point of a full page correct.
ORDER = "ASC"

# THE INDEX SEAM -- 2026-09-02 13:41:29 local (epoch ms, so it does not move
# with the reader's timezone).
#
# The probe indices shifted on the device. `moisture3` and `relay4` stop dead
# there; `moisture2` changes MEANING, from a pin tied to 3V3 that reported a
# constant 0.00 to the sensor that had been publishing as `moisture3`.
#
# Observed in the archive, which is why the constant is trusted: the last
# old-regime sample is 13:41:10 (moisture2 = 0, moisture3 = 77.23) and the
# first new-regime one is 13:42:26 (moisture2 = 77.31, moisture3 absent).
#
# Nothing here splits, renames or stitches those series. An export is faithful
# to what the cloud holds; correcting it in flight would bake one reading of
# the hardware history into every file anyone ever exports, and the operator is
# the one who knows which. So a CSV spanning this instant gets a warning on
# stderr naming the timestamp and the keys, and identical bytes either way.
SEAM_MS = 1788367289000
SEAM_KEYS = ("moisture2", "moisture3", "relay4", "relay4Ran", "relay4Event")

ATTRIBUTE_SCOPES = ("CLIENT_SCOPE", "SERVER_SCOPE", "SHARED_SCOPE")

# The tenant is on the free plan, which rate-limits the REST API. A backfill is
# ~250 000 datapoints and is by far the heaviest thing this will ever do, so it
# is the run that meets a 429 first.
#
# The one thing a 429 must NEVER do is look like an empty page. `raw_series`
# returning [] ends the walk for that key, so a throttled request swallowed
# here would close the window early and write an archive with a hole in it that
# reports success. It retries, and if it runs out of retries it raises.
RETRY_STATUSES = (429, 502, 503, 504)
MAX_RETRIES = 6
RETRY_BASE_SEC = 2.0
RETRY_CAP_SEC = 60.0

SCHEMA = """
CREATE TABLE IF NOT EXISTS telemetry (
    key   TEXT    NOT NULL,
    ts    INTEGER NOT NULL,
    value TEXT    NOT NULL,
    PRIMARY KEY (key, ts)
) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS telemetry_ts ON telemetry (ts);

CREATE TABLE IF NOT EXISTS attributes (
    scope TEXT    NOT NULL,
    key   TEXT    NOT NULL,
    ts    INTEGER NOT NULL,
    value TEXT    NOT NULL,
    PRIMARY KEY (scope, key)
);

CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
"""


class ApiError(Exception):
    """An HTTP status and the body the API sent, verbatim.

    Reported rather than worked around: a 403 on one endpoint and a 400 on a
    malformed range are different problems, and a retry loop that flattens both
    into "it did not work" costs the only sentence that says which.
    """

    def __init__(self, path: str, status: int, body: bytes):
        self.status = status
        self.body = body.decode("utf-8", "replace")[:600]
        super().__init__("GET %s -> %d: %s" % (path, status, self.body))


class Fatal(Exception):
    """Something the operator has to fix. Printed as a sentence, not a trace."""


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

_token: str | None = None


def token() -> str:
    """Reads the credential once, from disk, and hands it only to a header."""
    global _token
    if _token is None:
        path = Path(os.environ.get("TB_API_KEY_FILE") or DEFAULT_KEY_FILE)
        try:
            _token = path.read_text(encoding="utf-8").strip()
        except OSError as e:
            raise Fatal(
                "cannot read the ThingsBoard credential from %s (%s).\n"
                "Put the API key in that file, or point TB_API_KEY_FILE at it. "
                "It is gitignored on purpose -- do not pass it on a command "
                "line." % (path, e.strerror or e))
        if not _token:
            raise Fatal("%s is empty; it should hold the API key and nothing "
                        "else" % path)
    return _token


RETRIES = {"count": 0, "slept": 0.0}


def _retry_delay(attempt: int, headers) -> float:
    """Retry-After when the server names one, exponential backoff otherwise.

    ThingsBoard's rate limiter does not always send the header, and guessing
    low is what turns one 429 into a stampede of them.
    """
    if headers is not None:
        raw = headers.get("Retry-After")
        if raw:
            try:
                return min(max(float(raw.strip()), 1.0), RETRY_CAP_SEC)
            except ValueError:
                pass
    return min(RETRY_BASE_SEC * (2 ** attempt), RETRY_CAP_SEC)


def api_get(path: str, timeout: int = 180):
    """One GET, retried on a throttle or a gateway blip.

    Non-200 that is not retryable raises ApiError carrying the body the server
    sent, verbatim. A retryable status that outlives MAX_RETRIES raises the
    same way -- it is reported, never converted into an empty result.
    """
    for attempt in range(MAX_RETRIES + 1):
        req = urllib.request.Request(BASE + path, method="GET")
        req.add_header("X-Authorization", "ApiKey " + token())
        req.add_header("Accept", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                raw = resp.read()
            return json.loads(raw) if raw else None
        except urllib.error.HTTPError as e:
            body = e.read()
            if e.code not in RETRY_STATUSES or attempt == MAX_RETRIES:
                raise ApiError(path, e.code, body)
            delay = _retry_delay(attempt, getattr(e, "headers", None))
            RETRIES["count"] += 1
            RETRIES["slept"] += delay
            # Logged every time. A backfill that quietly took ten minutes
            # because it was throttled is a different fact from one that took
            # ten minutes because there is a lot of data.
            print("tb_export: %d from the API, retry %d/%d in %.0fs"
                  % (e.code, attempt + 1, MAX_RETRIES, delay), file=sys.stderr)
            time.sleep(delay)
        except urllib.error.URLError as e:
            raise Fatal("cannot reach %s (%s)" % (BASE, e.reason))


def whoami() -> dict:
    return api_get("/api/auth/user")


def resolve_device(name: str) -> dict:
    """The device row, matched on name.

    A UUID is an opaque row id and not a secret, so it is cached in the archive
    itself -- but it is never the thing looked up first, because a cache that
    cannot be rebuilt from something a human knows is a cache that outlives its
    truth.
    """
    page = api_get("/api/tenant/devices?pageSize=200&page=0")
    for d in page.get("data", []):
        if d.get("name") == name:
            return {"id": d["id"]["id"], "name": d.get("name"),
                    "label": d.get("label"), "type": d.get("type")}
    seen = ", ".join(sorted(d.get("name", "?") for d in page.get("data", [])))
    raise Fatal("no device named %r in this tenant; it has: %s" % (name, seen))


def _tel(dev_id: str, suffix: str) -> str:
    return "/api/plugins/telemetry/DEVICE/%s/%s" % (dev_id, suffix)


def timeseries_keys(dev_id: str) -> list:
    """Keys ThingsBoard STORES, which is not the same question as what the
    firmware currently sends: a retired key keeps its row here, and a key the
    broker never accepted is absent however hard the device publishes it."""
    return api_get(_tel(dev_id, "keys/timeseries")) or []


def raw_series(dev_id: str, key: str, start_ms: int, end_ms: int) -> list:
    """One page of stored datapoints.

    agg=NONE returns what is on disk. Any other aggregation answers with
    buckets, and buckets hide gaps by interpolating over them -- which for an
    archive is the one failure that cannot be detected after the fact.
    """
    query = urllib.parse.urlencode({
        "keys": key, "startTs": int(start_ms), "endTs": int(end_ms),
        "limit": PAGE_LIMIT, "agg": "NONE", "orderBy": ORDER})
    out = api_get(_tel(dev_id, "values/timeseries?" + query))
    return (out or {}).get(key, [])


def attribute_rows(dev_id: str) -> list:
    rows = []
    for scope in ATTRIBUTE_SCOPES:
        for a in api_get(_tel(dev_id, "values/attributes/" + scope)) or []:
            rows.append((scope, a.get("key"), int(a.get("lastUpdateTs") or 0),
                         as_text(a.get("value"))))
    return rows


# --------------------------------------------------------------------------
# Store
# --------------------------------------------------------------------------

def as_text(value) -> str:
    """Everything is stored as TEXT.

    ThingsBoard hands telemetry back as strings already, and several of these
    keys are genuinely not numbers -- bootReason, relayName, firmware,
    moisture1State, relayNEvent. Coercing at write time would either fail on
    those or silently turn "0.00" into 0.0 and lose how the device said it.
    Casting is the caller's business, at export time, where the caller knows
    which column they are casting.
    """
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    return json.dumps(value, separators=(",", ":"), sort_keys=True)


def open_db(path: Path) -> sqlite3.Connection:
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path))
    conn.executescript(SCHEMA)
    return conn


def meta_set(conn: sqlite3.Connection, key: str, value: str) -> None:
    conn.execute("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)",
                 (key, value))


def meta_get(conn: sqlite3.Connection, key: str) -> str | None:
    row = conn.execute("SELECT value FROM meta WHERE key = ?", (key,)).fetchone()
    return row[0] if row else None


def stored_max_ts(conn: sqlite3.Connection, key: str):
    row = conn.execute("SELECT MAX(ts) FROM telemetry WHERE key = ?",
                       (key,)).fetchone()
    return row[0] if row and row[0] is not None else None


def total_rows(conn: sqlite3.Connection) -> int:
    return conn.execute("SELECT COUNT(*) FROM telemetry").fetchone()[0]


# --------------------------------------------------------------------------
# Sync
# --------------------------------------------------------------------------

def sync_key(conn, dev_id: str, key: str, start_ms: int, end_ms: int):
    """Pages one key's window into the archive. Returns (inserted, seen, pages).

    Continuity is asserted rather than hoped for. A page shorter than
    PAGE_LIMIT means the window is exhausted; a page exactly that long means
    the server truncated and there is more, so the next window opens one
    millisecond past its last point. Anything that would make the walk stand
    still or go backwards -- an out-of-order page, a page whose last timestamp
    is not past the cursor -- stops the run instead of quietly looping or
    skipping a stretch of history.
    """
    cursor, pages, seen, inserted = start_ms, 0, 0, 0
    previous_last = None
    while cursor < end_ms:
        points = raw_series(dev_id, key, cursor, end_ms)
        pages += 1
        if not points:
            break

        stamps = [int(p["ts"]) for p in points]
        if any(b < a for a, b in zip(stamps, stamps[1:])):
            raise Fatal("%s: page %d came back out of order despite "
                        "orderBy=%s; refusing to guess the real sequence"
                        % (key, pages, ORDER))
        if stamps[0] < cursor:
            raise Fatal("%s: page %d starts at %d, before the window opened at "
                        "%d" % (key, pages, stamps[0], cursor))
        if previous_last is not None and stamps[0] <= previous_last:
            raise Fatal("%s: page %d overlaps the previous one (%d <= %d)"
                        % (key, pages, stamps[0], previous_last))

        before = conn.total_changes
        conn.executemany(
            "INSERT OR IGNORE INTO telemetry (key, ts, value) VALUES (?, ?, ?)",
            [(key, int(p["ts"]), as_text(p.get("value"))) for p in points])
        inserted += conn.total_changes - before
        seen += len(points)
        previous_last = stamps[-1]

        if len(points) < PAGE_LIMIT:
            break
        if stamps[-1] + 1 <= cursor:
            raise Fatal("%s: a full page ended at %d without passing the "
                        "cursor at %d; the walk cannot advance"
                        % (key, stamps[-1], cursor))
        cursor = stamps[-1] + 1
    return inserted, seen, pages


def sync(conn, dev_id: str, keys: list, since_ms, until_ms, full: bool,
         quiet: bool = False) -> dict:
    """Brings the archive up to date. Returns a small report."""
    end_ms = (until_ms if until_ms is not None
              else int(time.time() * 1000)) + 1  # endTs is exclusive
    report = {"keys": 0, "inserted": 0, "seen": 0, "pages": 0, "per_key": {}}

    for key in keys:
        if full or since_ms is not None:
            start_ms = since_ms if since_ms is not None else 0
        else:
            last = stored_max_ts(conn, key)
            # MAX(ts) + 1 because startTs is inclusive. Re-fetching the
            # boundary point would be harmless -- INSERT OR IGNORE absorbs it
            # -- but skipping it would not be, so the +1 is the deliberate half
            # of that pair, not an off-by-one.
            start_ms = 0 if last is None else last + 1

        inserted, seen, pages = sync_key(conn, dev_id, key, start_ms, end_ms)
        conn.commit()
        report["keys"] += 1
        report["inserted"] += inserted
        report["seen"] += seen
        report["pages"] += pages
        report["per_key"][key] = inserted
        if not quiet:
            print("  %-18s %7d new  (%d fetched, %d page%s)"
                  % (key, inserted, seen, pages, "" if pages == 1 else "s"))

    rows = attribute_rows(dev_id)
    # Attributes are a SNAPSHOT, not a history: ThingsBoard keeps only the
    # current value per scope and key, so the table is replaced wholesale
    # rather than accumulated. Merging would leave rows for attributes the
    # tenant has since deleted, with nothing to say they are gone.
    conn.execute("DELETE FROM attributes")
    conn.executemany(
        "INSERT OR REPLACE INTO attributes (scope, key, ts, value) "
        "VALUES (?, ?, ?, ?)", rows)
    report["attributes"] = len(rows)
    meta_set(conn, "last_sync_ms", str(int(time.time() * 1000)))
    conn.commit()
    return report


# --------------------------------------------------------------------------
# CSV
# --------------------------------------------------------------------------

def export_csv(conn, path: Path, keys: list, since_ms, until_ms) -> tuple:
    """Wide CSV: one row per timestamp, one column per key.

    A cell with no sample at that timestamp is left EMPTY. Forward-filling
    would make the file easier to plot and would invent readings the garden
    never took -- and a gap is the single most interesting thing an archive of
    a field device has to say.

    Written streaming, grouped by an ORDER BY, so a 300k-row window costs one
    row of memory rather than a pivot table.
    """
    lo = since_ms if since_ms is not None else -(1 << 62)
    hi = until_ms if until_ms is not None else (1 << 62)

    if keys:
        columns = list(keys)
        placeholders = ",".join("?" * len(columns))
        where = ("ts BETWEEN ? AND ? AND key IN (%s)" % placeholders)
        params = [lo, hi] + columns
    else:
        columns = [r[0] for r in conn.execute(
            "SELECT DISTINCT key FROM telemetry WHERE ts BETWEEN ? AND ? "
            "ORDER BY key", (lo, hi))]
        where = "ts BETWEEN ? AND ?"
        params = [lo, hi]

    if not columns:
        raise Fatal("nothing to export: no rows in that window"
                    + (" for %s" % ",".join(keys) if keys else ""))

    index = {k: i for i, k in enumerate(columns)}
    rows_written = 0
    path.parent.mkdir(parents=True, exist_ok=True)

    with open(path, "w", encoding="utf-8", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["timestamp", "epoch_ms"] + columns)
        current_ts = None
        cells = [""] * len(columns)
        cursor = conn.execute(
            "SELECT ts, key, value FROM telemetry WHERE %s ORDER BY ts, key"
            % where, params)
        for ts, key, value in cursor:
            if ts != current_ts:
                if current_ts is not None:
                    writer.writerow([iso(current_ts), current_ts] + cells)
                    rows_written += 1
                current_ts = ts
                cells = [""] * len(columns)
            cells[index[key]] = value
        if current_ts is not None:
            writer.writerow([iso(current_ts), current_ts] + cells)
            rows_written += 1

    warn_on_seam(columns, lo, hi)
    return rows_written, len(columns) + 2


def warn_on_seam(columns: list, lo: int, hi: int) -> None:
    """Says so on stderr when the export straddles the index shift.

    The file itself is untouched. What the operator gets is the one fact that
    makes it readable: two stretches of `moisture2` in the same column are two
    different sensors.
    """
    if not (lo <= SEAM_MS <= hi):
        return
    affected = [k for k in columns if k in SEAM_KEYS]
    if not affected:
        return
    print("tb_export: WARNING -- this export spans the probe index shift at "
          "%s (epoch_ms %d)." % (iso(SEAM_MS), SEAM_MS), file=sys.stderr)
    print("tb_export: affected columns: %s" % ", ".join(affected),
          file=sys.stderr)
    print("tb_export: after that instant moisture3 and relay4 stop, and "
          "moisture2 changes meaning -- it was a pin tied to 3V3 reading a "
          "constant 0.00, and becomes the sensor that had been publishing as "
          "moisture3. The export is faithful to the cloud and corrects "
          "nothing.", file=sys.stderr)


# --------------------------------------------------------------------------
# Reporting and CLI
# --------------------------------------------------------------------------

def iso(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000.0).isoformat(sep=" ",
                                                         timespec="seconds")


def parse_when(text: str, end_of_day: bool) -> int:
    """ISO date or datetime, in LOCAL time, to epoch ms.

    Local because every other timestamp a person reads about this device --
    the serial log, /history.json, CLAUDE.md -- is local, and a UTC-only flag
    would be a three-hour trap on exactly the window someone is trying to look
    at closely.
    """
    try:
        when = datetime.fromisoformat(text)
    except ValueError:
        raise Fatal("cannot read %r as a date; use 2026-09-02 or "
                    "2026-09-02T13:41:29" % text)
    if end_of_day and len(text.strip()) == 10:
        when = when.replace(hour=23, minute=59, second=59, microsecond=999000)
    return int(when.timestamp() * 1000)


def print_stats(conn) -> None:
    rows = conn.execute(
        "SELECT key, COUNT(*), MIN(ts), MAX(ts) FROM telemetry "
        "GROUP BY key ORDER BY key").fetchall()
    if not rows:
        print("tb_export: the archive is empty")
        return
    for key, n, lo, hi in rows:
        span_days = max((hi - lo) / 86400000.0, 1e-9)
        print("  %-18s %7d  %s .. %s  (%.0f/day)"
              % (key, n, iso(lo), iso(hi), n / span_days))
    total = sum(r[1] for r in rows)
    lo = min(r[2] for r in rows)
    hi = max(r[3] for r in rows)
    attrs = conn.execute("SELECT COUNT(*) FROM attributes").fetchone()[0]
    print("\ntb_export: %d rows, %d keys, %s .. %s, %d attribute(s)"
          % (total, len(rows), iso(lo), iso(hi), attrs))


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Archive ThingsBoard telemetry into SQLite; export CSV.")
    ap.add_argument("--db", default=str(DEFAULT_DB),
                    help="SQLite archive (default backups/telemetry.sqlite)")
    ap.add_argument("--device", default=DEFAULT_DEVICE,
                    help="ThingsBoard device name (default %s)" % DEFAULT_DEVICE)
    ap.add_argument("--keys", help="comma-separated subset of telemetry keys")
    ap.add_argument("--since", help="ISO date/datetime, local (sync and CSV)")
    ap.add_argument("--until", help="ISO date/datetime, local (sync and CSV)")
    ap.add_argument("--full", action="store_true",
                    help="refetch every key from the beginning instead of "
                         "resuming from what is stored")
    ap.add_argument("--no-sync", action="store_true",
                    help="touch no network; export from the archive as it is")
    ap.add_argument("--csv", help="write a wide CSV to this path")
    ap.add_argument("--stats", action="store_true",
                    help="report what the archive holds, per key")
    args = ap.parse_args()

    keys = [k.strip() for k in args.keys.split(",")] if args.keys else []
    since_ms = parse_when(args.since, False) if args.since else None
    until_ms = parse_when(args.until, True) if args.until else None
    if since_ms is not None and until_ms is not None and since_ms > until_ms:
        raise Fatal("--since is after --until")

    conn = open_db(Path(args.db).resolve())
    try:
        if not args.no_sync:
            started = time.time()
            me = whoami()
            # The status is the proof, not the token: a 200 here is the only
            # thing that needs saying about a credential.
            print("tb_export: auth 200 as %s (%s)"
                  % (me.get("email"), me.get("authority")))

            device = resolve_device(args.device)
            meta_set(conn, "device_id", device["id"])
            meta_set(conn, "device_name", device["name"] or "")
            print("tb_export: device %s (%s)" % (device["name"], device["id"]))

            available = sorted(timeseries_keys(device["id"]))
            wanted = keys or available
            missing = [k for k in wanted if k not in available]
            if missing:
                print("tb_export: not stored by the tenant, skipped: %s"
                      % ", ".join(missing), file=sys.stderr)
            wanted = [k for k in wanted if k in available]

            before = total_rows(conn)
            report = sync(conn, device["id"], wanted, since_ms, until_ms,
                          args.full)
            after = total_rows(conn)
            print("tb_export: %d keys, %d fetched, %d new rows, %d pages, "
                  "%d attributes, %.1fs"
                  % (report["keys"], report["seen"], report["inserted"],
                     report["pages"], report["attributes"],
                     time.time() - started))
            print("tb_export: archive %d -> %d rows in %s"
                  % (before, after, args.db))
            if RETRIES["count"]:
                print("tb_export: %d request(s) were throttled or failed and "
                      "retried, %.0fs spent waiting"
                      % (RETRIES["count"], RETRIES["slept"]))

        if args.csv:
            out = Path(args.csv).resolve()
            rows, cols = export_csv(conn, out, keys, since_ms, until_ms)
            print("tb_export: wrote %d rows x %d columns to %s"
                  % (rows, cols, out))

        if args.stats:
            print_stats(conn)
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fatal as e:
        print("tb_export: %s" % e, file=sys.stderr)
        sys.exit(1)
    except ApiError as e:
        print("tb_export: %s" % e, file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("tb_export: interrupted", file=sys.stderr)
        sys.exit(130)
