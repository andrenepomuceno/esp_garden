#!/usr/bin/env python3
"""Where the collected history LIVES: one SQLite file, and how a record enters it.

The storage half of scripts/history_export.py; scripts/history_archive.py is the
arithmetic half and this imports it. Split when the pair crossed the 1000-line
gate scripts/check_lines.py enforces, along the seam the repo already uses for
moisture_fit.py + moisture_stats.py and telemetry_ui.py + telemetry_page.py: the
half that reasons about data, and the half that talks to a database.

SQLite for the same reason tb_export.py chose it: the store of record is a file
and every other view is derived from it. The merge lives here rather than in the
entry point so that checks() can exercise a REAL one against `:memory:` - the
conflict branch in particular, which exists because the identity argument in
history_archive.record_digest() is reasoning about the firmware rather than a
measurement of it.

Standard library only. It never touches a network.
"""

from __future__ import annotations

import json
import sqlite3
from pathlib import Path

# `_number` and `_fake_records` are underscored because they are internal to the
# PAIR, not to one file: the two modules were one until the 1000-line gate split
# them, and a rounding helper that disagrees between the walk and the table is
# exactly the drift the split must not introduce.
from history_archive import (MOISTURE_SLOTS, SANE_EPOCH, _fake_records,
                             _number, record_digest, record_is_sane)


SCHEMA = """
CREATE TABLE IF NOT EXISTS records (
    t          INTEGER NOT NULL PRIMARY KEY,
    digest     TEXT    NOT NULL,
    session    INTEGER NOT NULL,
    relays     INTEGER NOT NULL,
    m0 REAL, m1 REAL, m2 REAL, m3 REAL,
    lum REAL, temp REAL, hum REAL, water REAL,
    flow REAL, flow_total REAL,
    float_sw   INTEGER
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS conflicts (
    t       INTEGER NOT NULL,
    digest  TEXT    NOT NULL,
    session INTEGER NOT NULL,
    payload TEXT    NOT NULL,
    PRIMARY KEY (t, digest)
);

CREATE TABLE IF NOT EXISTS sessions (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    at         INTEGER NOT NULL,
    host       TEXT    NOT NULL,
    device     TEXT,
    capacity   INTEGER,
    stored     INTEGER,
    period_sec REAL,
    requests   INTEGER,
    added      INTEGER,
    identity   TEXT
);

CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
"""


def open_archive(path, create=True):
    if path != ":memory:":
        path = Path(path)
        if not path.exists() and not create:
            raise FileNotFoundError(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path = str(path)
    conn = sqlite3.connect(path)
    # Rows come back by NAME. Positional unpacking is what turns "a column was
    # added in the middle" into a silently rearranged archive, which is the same
    # class of defect as the positional keys this file exists to stamp.
    conn.row_factory = sqlite3.Row
    conn.executescript(SCHEMA)
    return conn


def newest_stored(conn):
    row = conn.execute("SELECT MAX(t) FROM records").fetchone()
    return int(row[0]) if row and row[0] is not None else 0


def _row_to_record(row):
    return {
        "t": int(row["t"]),
        "relays": int(row["relays"]),
        "moisture": [row["m0"], row["m1"], row["m2"], row["m3"]],
        "lum": row["lum"], "temp": row["temp"], "hum": row["hum"],
        "water": row["water"], "flow": row["flow"],
        "flowTotal": row["flow_total"],
        "float": row["float_sw"],
    }


def read_records(conn, since=0, until=None):
    """Every archived record in the dict shape /history.json serialises.

    ONE shape everywhere: the walk, the merge, the fit adapter and the CSV all
    speak it, so nothing has to remember which side of the database it is on.
    """
    query = "SELECT * FROM records WHERE t > ?"
    args = [int(since)]
    if until is not None:
        query += " AND t <= ?"
        args.append(int(until))
    query += " ORDER BY t"
    return [_row_to_record(row) for row in conn.execute(query, args)]


def merge(conn, records, session_id, report=None):
    """Writes what is new, and REPORTS everything else rather than swallowing it.

    Three outcomes per record and all three are counted:

      new        a timestamp the archive did not hold.
      duplicate  the same timestamp AND the same content. This is the ordinary
                 case - every run deliberately overlaps the last - and it is a
                 no-op.
      conflict   the same timestamp, DIFFERENT content. record_digest() says why
                 that should be impossible; it is stored in its own table with
                 the losing payload intact and printed, because an identity this
                 tool believes in and cannot prove is worth more as a loud row
                 than as a silent overwrite.
      refused    a timestamp below g_safeTimestamp, which the firmware will not
                 write. Not merged at all: it would take a primary key that a
                 later, real record may need.
    """
    counts = {"new": 0, "duplicate": 0, "conflict": 0, "refused": 0}
    for record in records:
        ok, why = record_is_sane(record)
        if not ok:
            counts["refused"] += 1
            if report is not None:
                print(f"  refused a record: {why}", file=report)
            continue
        stamp = int(record["t"])
        digest = record_digest(record)
        row = conn.execute(
            "SELECT digest FROM records WHERE t = ?", (stamp,)
        ).fetchone()
        if row is not None:
            if row["digest"] == digest:
                counts["duplicate"] += 1
            else:
                counts["conflict"] += 1
                conn.execute(
                    "INSERT OR IGNORE INTO conflicts (t, digest, session, "
                    "payload) VALUES (?, ?, ?, ?)",
                    (stamp, digest, session_id,
                     json.dumps(record, separators=(",", ":"))),
                )
            continue
        moisture = list(record.get("moisture") or []) + [None] * MOISTURE_SLOTS
        conn.execute(
            "INSERT INTO records (t, digest, session, relays, m0, m1, m2, m3, "
            "lum, temp, hum, water, flow, flow_total, float_sw) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (stamp, digest, session_id, int(record.get("relays") or 0),
             _number(moisture[0]), _number(moisture[1]), _number(moisture[2]),
             _number(moisture[3]), _number(record.get("lum")),
             _number(record.get("temp")), _number(record.get("hum")),
             _number(record.get("water")), _number(record.get("flow")),
             _number(record.get("flowTotal")), record.get("float")),
        )
        counts["new"] += 1
    return counts


# ---------------------------------------------------------------------------
# checks() - run by `python scripts/moisture_fit.py --self-test`
# ---------------------------------------------------------------------------


def checks():
    """Against a real sqlite3, because the merge IS the de-duplication.

    A dictionary standing in for a table would not exercise the primary key,
    which is the whole mechanism.
    """
    out = []

    def check(name, condition, detail=""):
        out.append((name, bool(condition), detail))

    conn = open_archive(":memory:")
    batch = _fake_records(50)
    first = merge(conn, batch, 1)
    check("a first merge writes everything once",
          first["new"] == 50 and first["duplicate"] == 0, str(first))
    again = merge(conn, batch, 2)
    check("...and merging the SAME records again writes nothing",
          again["new"] == 0 and again["duplicate"] == 50, str(again))
    check("...leaving the archive at 50 rows",
          newest_stored(conn) == batch[-1]["t"]
          and len(read_records(conn)) == 50)
    overlapped = merge(conn, batch[-5:] + _fake_records(
        5, start=batch[-1]["t"] + 60), 3)
    check("an overlapping run adds only the new tail",
          overlapped["new"] == 5 and overlapped["duplicate"] == 5,
          str(overlapped))

    # The identity argument says this cannot happen. It is checked anyway,
    # because it is reasoning about the firmware and not a measurement of it.
    collided = merge(conn, [{**batch[0], "lum": 99.0}], 4)
    check("a timestamp arriving twice with DIFFERENT content is a conflict",
          collided["conflict"] == 1 and collided["new"] == 0, str(collided))
    check("...stored with its payload rather than overwriting the original",
          conn.execute("SELECT COUNT(*) FROM conflicts").fetchone()[0] == 1
          and read_records(conn)[0]["lum"] == 50.0)
    refused = merge(conn, [{"t": 90000, "relays": 0, "moisture": []}], 5)
    check("a pre-2021 record is refused and never takes a primary key",
          refused["refused"] == 1 and len(read_records(conn)) == 55,
          str(refused))
    check("...and the refusal names g_safeTimestamp",
          str(SANE_EPOCH) in (record_is_sane({"t": 90000})[1] or ""))
    check("a record survives the round trip through SQLite unchanged",
          record_digest(read_records(conn)[10]) == record_digest(batch[10]),
          str(read_records(conn)[10]))
    conn.close()
    return out
