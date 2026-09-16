#!/usr/bin/env python3
"""What `GET /history.json` means, separated from the code that fetches it.

The arithmetic half of scripts/history_export.py, which is the entry point and
imports this - the same split moisture_fit.py has with moisture_stats.py, and
for the same reason CLAUDE.md gives for segment_index.h: a wrong answer here
does not fail, it silently reorders or drops history, and that is the one part
of a subsystem that must have tests. Everything below is a free function or a
plain class over plain dicts, so `moisture_fit.py --self-test` can reach all of
it.

WHY AN ARCHIVER EXISTS AT ALL

The device holds a bounded, self-recycling buffer: 3000 records at a 60 s period
on 6224, about two days. Records that nobody collects are gone, and this is the
ONLY place the device's own 60 s record is ever visible - ThingsBoard gets the
300 s publish, and scripts/tb_export.py archives that. Two questions in
CLAUDE.md are blocked on this record and on nothing else: seeding
/moisture_model.bin (see MODEL_FILE_DECLINED in moisture_fit.py) and resolving
a three-minute plateau that sits below the 300 s sampling.

THE THREE THINGS THAT CAN GO WRONG, AND WHAT STOPS EACH

  DOUBLE COUNTING   Every run overlaps the last. The record identity is the
                    record's own timestamp - see `record_digest` for why that
                    cannot collide - so an overlap is a no-op, not a duplicate.
                    The table that enforces it is in history_store.py beside
                    this, split off when the pair crossed the 1000-line gate.

  SILENT DROPPING   `offset` is a LOGICAL index and a segment rotation shifts
                    every one of them down by a whole segment at once. A walk
                    that trusts an offset it computed before a rotation reads
                    the same newest records twice, concludes it has reached
                    known data, and never sees the 375 in between. `collect()`
                    refuses to believe an offset: it checks the device's own
                    `stored` and, independently, that consecutive pages abut
                    exactly where their own offsets say they do.

  UNNOTICED GAPS    A collector that was away longer than the buffer's retention
                    loses records, and so does a device whose clock never synced
                    - historyTaskHandler() returns without appending while
                    `time(NULL) < g_safeTimestamp`, which cost 6224 17.6 hours of
                    history on 2026-09-16. Neither is recoverable and both are
                    REPORTED rather than stitched over.
"""

from __future__ import annotations

import hashlib
import json
import math

# ---------------------------------------------------------------------------
# The firmware's own constants. Changing one here without changing it there
# makes this tool answer a question the device is not asking.
# ---------------------------------------------------------------------------

# segment::kSegments in include/core/segment_index.h.
SEGMENTS = 8

# g_historyMaxResponse in src/web.cpp. The device clamps `limit` to it; asking
# for more is not an error, it is silently reduced.
MAX_RESPONSE = 200

# g_safeTimestamp in include/core/tasks.h, 2021-01-01. historyTaskHandler()
# refuses to write a record while the clock is below it, so no stored record can
# carry a pre-NTP stamp. That guarantee is what makes the timestamp usable as an
# identity, and a record below it means something this tool does not understand.
SANE_EPOCH = 1609459200

# The float columns of IoRecord, in the order handleHistoryJson serialises them.
# `moisture` is an array of IO_HISTORY_MAX_MOISTURE and is handled separately.
FLOAT_KEYS = ("lum", "temp", "hum", "water", "flow", "flowTotal")
MOISTURE_SLOTS = 4  # IO_HISTORY_MAX_MOISTURE


# ---------------------------------------------------------------------------
# This tool's own thresholds. Every one is a judgement, so every one says what
# it was set against.
# ---------------------------------------------------------------------------

# How many records two consecutive pages must share. It is not a safety margin
# against a rotation - a rotation shifts by a whole segment, which is 375 on
# 6224 and larger than any page - it is what makes the ordinary case
# SELF-VERIFYING: every page proves it abuts the last by containing records the
# last one already carried, rather than by arithmetic on an index the device
# is free to renumber. Five records costs one extra request per 195 collected.
PAGE_OVERLAP = 5

# A walk that keeps meeting rotations is a walk against a device recycling
# faster than the network can read it. Restarting for ever would hold a session
# slot indefinitely on a board with one HTTP task.
MAX_RESTARTS = 3

# A gap longer than this many record periods breaks the series. Below it the
# device simply missed a tick - historyTaskHandler() reschedules from the end of
# its callback, so a slow background task pushes the next append out. Three
# periods is the smallest number that cannot be reached by one late tick.
GAP_PERIODS = 3.0


# ---------------------------------------------------------------------------
# The device's buffer, as a rule rather than as a file
# ---------------------------------------------------------------------------


def records_per_segment(capacity, segments=SEGMENTS):
    """segment::recordsPerSegment(). Rounded UP, so the history is never short."""
    if segments <= 0 or capacity <= 0:
        return 0
    return min((capacity + segments - 1) // segments, 0xFFFF)


def effective_capacity(requested, segments=SEGMENTS):
    """IoHistory::capacity() - segmentRecords * kSegments, so 1001 becomes 1008."""
    return records_per_segment(requested, segments) * segments


def retention_seconds(capacity, period_sec, segments=SEGMENTS):
    """The shortest time a record is GUARANTEED to still be readable.

    Retention is granular: a whole segment is dropped at once, so the buffer
    holds between (segments-1)/segments of capacity and all of it. The
    guaranteed figure is the LOW end - a record written just before a rotation
    is evicted when the buffer is at its shortest, not at its longest - and
    planning a collection cadence against the high end loses a record roughly
    one run in eight, which is exactly the kind of loss nothing would report.
    """
    per = records_per_segment(capacity, segments)
    return per * (segments - 1) * period_sec


def safe_interval_seconds(capacity, period_sec, tolerated_failures,
                          segments=SEGMENTS):
    """The longest gap between two runs that still cannot lose a record.

    `tolerated_failures` consecutive runs may fail - the host asleep, the device
    rebooting, Wi-Fi down - and the next successful one must still find
    everything. So the interval is the guaranteed retention divided by one more
    than the number of failures tolerated.
    """
    if tolerated_failures < 0:
        raise ValueError("tolerated_failures must not be negative")
    return retention_seconds(capacity, period_sec, segments) / (
        tolerated_failures + 1
    )


class SegmentHistory:
    """The device's append-only segment buffer, as observable behaviour.

    Mirrors IoHistory::append() and rotateLocked() in src/io_history.cpp: append
    to the newest segment; when it is full, recycle the oldest (or claim an
    unused one) and start there. Nothing is ever rewritten, and a rotation
    discards a whole segment's worth of records in one step.

    IT IS A BELIEF ABOUT THE DEVICE, NOT A MEASUREMENT OF IT. `test_segment_index`
    is what pins the firmware's arithmetic; this is a second implementation in
    another language, written from that header and from io_history.cpp. It is
    shared between scripts/sim_state.py and this module's own checks() rather
    than written twice, so the SIMULATOR and the ARCHIVER'S TESTS agree by
    construction - which means a wrong belief about the device's rule would be
    invisible in both at once. That is the cost, and it is taken deliberately:
    two Python copies of the same misreading would not have caught it either.
    """

    def __init__(self, requested, segments=SEGMENTS):
        self.segments = segments
        self.per_segment = records_per_segment(requested, segments)
        self.requested = requested
        # Oldest first, exactly as `order` is. Each entry is a list of records.
        self._files: list[list[dict]] = []
        self.evicted = 0
        self.rotations = 0

    @property
    def capacity(self):
        return self.per_segment * self.segments

    @property
    def stored(self):
        return sum(len(f) for f in self._files)

    def append(self, record):
        if self.per_segment == 0:
            return False
        if not self._files or len(self._files[-1]) >= self.per_segment:
            if len(self._files) >= self.segments:
                dropped = self._files.pop(0)
                self.evicted += len(dropped)
            self._files.append([])
            self.rotations += 1
        self._files[-1].append(record)
        return True

    def all_records(self):
        return [record for f in self._files for record in f]

    def read(self, limit, offset=None):
        """IoHistory::read() plus handleHistoryJson's header, as a dict.

        Deliberately the HTTP shape and not a list: the thing under test is what
        a caller can see through the endpoint, and the endpoint is where `offset`
        stops being a memory address and starts being a promise the device can
        break.
        """
        everything = self.all_records()
        limit = max(1, min(int(limit), MAX_RESPONSE))
        if offset is None:
            skip = max(0, len(everything) - limit)
        else:
            skip = max(0, int(offset))
        records = everything[skip:skip + limit]
        return {
            "capacity": self.capacity,
            "stored": len(everything),
            "returned": len(records),
            "offset": skip,
            "stride": 1,
            "window": 0,
            "records": records,
        }


# ---------------------------------------------------------------------------
# Record identity
# ---------------------------------------------------------------------------


def record_digest(record):
    """A stable fingerprint of one record's content, excluding its timestamp.

    THE IDENTITY IS THE TIMESTAMP; this is the check on it. The argument that
    `t` cannot collide is read out of the firmware and is three steps:

      1. historyTaskHandler() returns above the append while
         `time(NULL) < g_safeTimestamp`, so every stored record carries a real
         epoch and the 1970 collision class does not exist.
      2. The history task's period is `history.periodSec` (60 s) and background
         tasks reschedule from the END of their callback, so two appends are at
         least one period apart on the monotonic clock.
      3. So two records share a `t` only if the WALL clock moved backwards by a
         whole period between two appends - an NTP correction larger than 60 s
         on a board that had already synced.

    Step 3 is not impossible, only unobserved, so it is CHECKED rather than
    assumed: a second record arriving at a timestamp the archive already holds
    is compared field by field, and a disagreement is stored as a conflict and
    counted. Nothing is overwritten, and nothing is dropped without saying so.
    """
    payload = {
        "relays": record.get("relays"),
        "moisture": [_number(v) for v in (record.get("moisture") or [])],
        "float": record.get("float"),
    }
    for key in FLOAT_KEYS:
        payload[key] = _number(record.get(key))
    text = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(text.encode()).hexdigest()[:16]


def _number(value):
    """None stays None; everything else is rounded to the two decimals the
    device serialises, so a float that survived a JSON round trip still matches.
    """
    if value is None:
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(number):
        return None
    return round(number, 3)


def record_is_sane(record):
    """Whether a record may be merged at all, with the reason when it may not."""
    stamp = record.get("t")
    if not isinstance(stamp, (int, float)) or stamp <= 0:
        return False, "no timestamp"
    if stamp < SANE_EPOCH:
        return False, (
            f"timestamp {int(stamp)} is before {SANE_EPOCH} and "
            "historyTaskHandler() refuses to write one: this record was not "
            "produced by a firmware this tool understands"
        )
    return True, None


# ---------------------------------------------------------------------------
# The walk
# ---------------------------------------------------------------------------


class Collected:
    """One run's result: what was read, and every way it fell short."""

    def __init__(self):
        self.records: list[dict] = []
        self.pages = 0
        self.restarts = 0
        self.capacity = 0
        self.stored = 0
        self.reached_oldest = False
        self.gap_before = None   # (archive newest, device oldest) when a gap
        self.notes: list[str] = []

    @property
    def newest(self):
        return self.records[-1]["t"] if self.records else None

    @property
    def oldest(self):
        return self.records[0]["t"] if self.records else None


def collect(fetch, known_newest=0, limit=MAX_RESPONSE, overlap=PAGE_OVERLAP,
            max_restarts=MAX_RESTARTS):
    """Every record newer than `known_newest`, oldest first.

    `fetch(limit, offset)` returns one parsed /history.json response; `offset`
    is None for the tail. The walk goes BACKWARDS from the newest record,
    because that is the end the archive needs and the only end whose position is
    not an index at all - a tail read needs no offset to be correct.

    Two independent guards, because one of them can be fooled:

      `stored` must not DECREASE. It is the device's own count, and the only
      thing that reduces it is a rotation. An INCREASE is an ordinary append and
      shifts no index, so it is accepted and the anchor moves with it.

      Consecutive pages must SHARE records. Pages step by `limit - overlap`, so
      in the absence of a shift each one carries `overlap` timestamps the last
      one already had. An empty intersection means an index moved under the
      walk whatever the header said.

    Either guard tripping restarts the walk from the tail rather than trying to
    repair an offset. A restart is cheap - the archive keeps what it collected
    and the identity makes the re-read free - and repairing an index against a
    device that is still recycling is how a drop becomes silent.
    """
    if overlap < 1 or overlap >= limit:
        raise ValueError("overlap must be at least 1 and smaller than limit")

    result = Collected()
    for attempt in range(max_restarts + 1):
        pages = _walk(fetch, known_newest, limit, overlap, result)
        if pages is not None:
            result.pages += _finish(result, pages, known_newest)
            return result
        result.restarts += 1
        if attempt == max_restarts:
            result.notes.append(
                f"gave up after {max_restarts} restarts: the buffer rotated "
                "under every walk. The device is recycling faster than this "
                "link can read it, so collect more often or raise --limit"
            )
            return result
    return result


def _walk(fetch, known_newest, limit, overlap, result):
    """One attempt. Returns the pages, or None when an index moved under it."""
    pages = []
    page = fetch(limit, None)
    result.pages += 1
    anchor = page.get("stored", 0)
    result.capacity = page.get("capacity", 0)
    result.stored = anchor
    if not page.get("records"):
        return pages

    while True:
        pages.append(page)
        first_index = int(page.get("offset", 0))
        oldest_here = page["records"][0]["t"]
        if first_index <= 0:
            result.reached_oldest = True
            break
        if known_newest and oldest_here <= known_newest:
            break

        page = fetch(limit, max(0, first_index - (limit - overlap)))
        result.pages += 1
        stored_now = page.get("stored", 0)
        if stored_now < anchor:
            result.notes.append(
                f"a segment rotated mid-walk (stored {anchor} -> {stored_now}); "
                "every logical index moved, so the walk restarted"
            )
            return None
        anchor = max(anchor, stored_now)
        result.stored = anchor
        if not page.get("records"):
            # An offset inside `stored` that answers nothing is not a shape any
            # reading of handleHistoryJson produces. Restart rather than guess.
            result.notes.append(
                "a page inside the buffer came back empty; the walk restarted"
            )
            return None
        if not _pages_abut(page, pages[-1]):
            result.notes.append(
                "two consecutive pages did not abut where their own offsets "
                "said they would, so an index moved under the walk whatever "
                "`stored` said; the walk restarted"
            )
            return None
    return pages


def _pages_abut(older, newer):
    """Whether `older` really is the page immediately before `newer`.

    THE EXACT CHECK, and it replaced a weaker one that a fault injector walked
    straight through. "Do the two pages share any record at all" sounds
    sufficient and is not: a rotation that is immediately followed by enough
    appends leaves `stored` where it was AND shifts every index, and the page
    that comes back then overlaps its predecessor by 185 records instead of 5.
    Non-empty, wrong, and the walk sailed on collecting a shorter range than the
    buffer held while reporting no gap - the precise silent loss this module
    exists to prevent, reproduced against scripts/dev_server.py with
    --history-rotate-every.

    So the shape is checked rather than the fact of an overlap. The two
    responses state their own start indices, so how many records they must share
    is arithmetic: `older.offset + len(older) - newer.offset`. Those records must
    be the LAST of `older` and the FIRST of `newer`, in order, by timestamp.
    Under a shift the count comes out right and the timestamps do not.
    """
    expected = (int(older.get("offset", 0)) + len(older["records"])
                - int(newer.get("offset", 0)))
    if expected < 1 or expected > len(older["records"]):
        return False
    if expected > len(newer["records"]):
        return False
    tail = [record["t"] for record in older["records"][-expected:]]
    head = [record["t"] for record in newer["records"][:expected]]
    return tail == head


def _finish(result, pages, known_newest):
    """Flattens the pages, oldest first.

    Records the archive already holds are NOT filtered out here, deliberately.
    Dropping them would make the merge's duplicate count zero on every ordinary
    run, and the primary key - the whole de-duplication argument - would then be
    exercised only in checks(). Handing the overlap to the merge means the
    identity is re-proved against the real table every time the tool runs, and
    the cost is a few hundred indexed SELECTs.
    """
    seen = {}
    for page in pages:
        for record in page["records"]:
            seen.setdefault(record["t"], record)
    result.records = [seen[key] for key in sorted(seen)]

    if result.reached_oldest and known_newest and result.records:
        # The walk ran out of buffer before it reached what the archive already
        # had. Everything between is gone from the device and was never
        # collected; it cannot be recovered and must not be papered over.
        if result.records[0]["t"] > known_newest:
            result.gap_before = (known_newest, result.records[0]["t"])
    return 0


# ---------------------------------------------------------------------------
# What the archive can say about itself
# ---------------------------------------------------------------------------


def coverage_gaps(times, period_sec, gap_periods=GAP_PERIODS):
    """Every break in a sorted timestamp series longer than a few periods.

    It CANNOT say which side of the break is at fault. A gap is either records
    the collector never fetched or records the device never wrote - and the
    device not writing is real: historyTaskHandler() skips the append entirely
    while the clock is unsynced, which on 6224 was 17.6 hours on 2026-09-16.
    Both are reported the same way, and neither is guessed at.
    """
    threshold = period_sec * gap_periods
    gaps = []
    for index in range(1, len(times)):
        span = times[index] - times[index - 1]
        if span > threshold:
            gaps.append(
                {
                    "from": times[index - 1],
                    "to": times[index],
                    "seconds": span,
                    "missing": max(0, int(round(span / period_sec)) - 1),
                }
            )
    return gaps


def observed_period(times):
    """The median spacing, which is what the archive actually sampled at.

    The configured `history.periodSec` is what the device was ASKED for; a
    background task reschedules from the end of its callback, so the delivered
    period is that or a little more. Everything that divides by a period here
    uses the observed one.
    """
    if len(times) < 2:
        return 0.0
    spans = sorted(times[i] - times[i - 1] for i in range(1, len(times)))
    middle = len(spans) // 2
    if len(spans) % 2:
        return float(spans[middle])
    return (spans[middle - 1] + spans[middle]) / 2.0


# ---------------------------------------------------------------------------
# The adapter the fit consumes
# ---------------------------------------------------------------------------


def samples_for_probe(records, index):
    """[(epoch, value)] for one probe slot, skipping the slots it does not have.

    A board with two probes still writes four, filled with NaN, which
    handleHistoryJson serialises as `null`. Those are not readings and are
    dropped rather than read as zero - the whole reason the record layout is
    fixed regardless of the fitted count.
    """
    samples = []
    for record in records:
        values = record.get("moisture") or []
        if index >= len(values):
            continue
        value = _number(values[index])
        if value is None:
            continue
        samples.append((float(record["t"]), value))
    return samples


def series_for_key(records, key):
    """[(epoch, value)] for one of the scalar channels."""
    samples = []
    for record in records:
        value = _number(record.get(key))
        if value is not None:
            samples.append((float(record["t"]), value))
    return samples


def events_for_relay(records, relay, period_sec, gap_periods=GAP_PERIODS):
    """Rising edges of one relay's bit - the watering events, as the device counts them.

    Mirrors collectEvents() in src/moisture_model.cpp: the mask is STICKY across
    a whole record period, so one long watering spans several records and must
    count once; and the FIRST record is never an edge, because a watering that
    was already running at the oldest surviving record would otherwise register
    as an event that never started.

    ONE DELIBERATE DIVERGENCE FROM THE DEVICE, in the conservative direction.
    The device's buffer is contiguous, so it needs no gap rule; an archive can
    have holes, and across a hole the previous state is unknown. A bit that is
    set on the far side of a gap may have risen inside it, so the edge detector
    is reset at every gap and that record cannot be an event. The alternative
    fabricates waterings at exactly the moments the record is weakest, and the
    six-event gate is the one thing standing between this garden and a
    confident wrong badge.
    """
    if relay is None or relay < 0:
        return []
    bit = 1 << int(relay)
    events = []
    previous_on = False
    seen = False
    previous_t = None
    threshold = period_sec * gap_periods
    for record in records:
        stamp = float(record["t"])
        if previous_t is not None and stamp - previous_t > threshold:
            seen = False
            previous_on = False
        on = bool(int(record.get("relays") or 0) & bit)
        if on and seen and not previous_on:
            events.append(stamp)
        previous_on = on
        seen = True
        previous_t = stamp
    return events


def consumed_until(records, relays):
    """MoistureModelState::consumedUntil for a model fitted from these records.

    The newest watering event across ALL probes, which is exactly what
    moistureModelTrain() stores: a cycle is not finished until the NEXT watering
    bounds it, so everything from that event onward waits for a later run.

    This is the value MODEL_FILE_DECLINED reason 3 says cannot be had from
    another data source, and it is right: the ThingsBoard archive is a different
    series at a different rate, so the newest event in it is not the newest
    event in the buffer the device will train from. From the DEVICE'S OWN
    records it is simply the last rising edge, and 0 - meaning "nothing
    consumed" - is only correct when there genuinely was none.
    """
    newest = 0.0
    for relay in relays:
        for event in events_for_relay(records, relay, 60.0):
            newest = max(newest, event)
    return int(newest)


# ---------------------------------------------------------------------------
# checks() - run by `python scripts/moisture_fit.py --self-test`
# ---------------------------------------------------------------------------


def _fake_records(count, start=SANE_EPOCH + 86400, period=60, relays=0):
    return [
        {
            "t": start + i * period,
            "relays": relays,
            "moisture": [40.0 + i * 0.01, None, None, None],
            "lum": 50.0, "temp": 25.0, "hum": 60.0,
            "water": None, "flow": None, "flowTotal": None, "float": None,
        }
        for i in range(count)
    ]


class _Tap:
    """A fetch() that can be told to disturb the buffer between two pages."""

    def __init__(self, history, rotate_after=None, append_after=None,
                 refill_after=None):
        self.history = history
        self.rotate_after = rotate_after
        self.append_after = append_after
        self.refill_after = refill_after
        self.calls = 0

    def _fill_segment(self):
        """Fills the newest segment and appends once, forcing one rotation."""
        newest = self.history.all_records()[-1]
        room = self.history.per_segment - len(self.history._files[-1])
        for step in range(room + 1):
            self.history.append({**newest, "t": newest["t"] + 60 * (step + 1)})

    def __call__(self, limit, offset):
        if self.calls == self.rotate_after:
            self._fill_segment()
        if self.calls == self.refill_after:
            # The NASTY one: rotate and then append enough that `stored` lands
            # back where it was. Every index has moved by a whole segment and
            # the device's own count says nothing happened.
            before = self.history.stored
            self._fill_segment()
            newest = self.history.all_records()[-1]
            step = 1
            while self.history.stored < before:
                self.history.append({**newest, "t": newest["t"] + 60 * step})
                step += 1
        if self.calls == self.append_after:
            newest = self.history.all_records()[-1]
            self.history.append({**newest, "t": newest["t"] + 60})
        self.calls += 1
        return self.history.read(limit, offset)


def checks():
    """Returns (name, ok, detail) triples for moisture_fit.py --self-test."""
    out = []

    def check(name, condition, detail=""):
        out.append((name, bool(condition), detail))

    # --- the buffer rule ---------------------------------------------------
    check("1001 records round up to 1008 across eight segments",
          effective_capacity(1001) == 1008, str(effective_capacity(1001)))
    check("3000 records is 375 per segment, exactly",
          records_per_segment(3000) == 375 and effective_capacity(3000) == 3000)

    history = SegmentHistory(800)  # 100 per segment
    for record in _fake_records(800):
        history.append(record)
    check("a full buffer holds its capacity", history.stored == 800)
    history.append(_fake_records(1, start=SANE_EPOCH + 200000)[0])
    check("...and the next append drops a whole SEGMENT, not one record",
          history.stored == 701 and history.evicted == 100,
          f"stored {history.stored}, evicted {history.evicted}")
    check("...so retention swings between 7/8 and 8/8 of capacity",
          history.per_segment * (SEGMENTS - 1) <= history.stored <= 800)

    # --- the cadence arithmetic -------------------------------------------
    check("6224's 3000 records at 60 s guarantee 43.75 h",
          abs(retention_seconds(3000, 60) - 157500) < 1e-9,
          str(retention_seconds(3000, 60)))
    check("...and tolerating six failed runs puts the interval at 6.25 h",
          abs(safe_interval_seconds(3000, 60, 6) - 22500) < 1e-9,
          str(safe_interval_seconds(3000, 60, 6)))
    check("retention is the LOW end of the swing, never the capacity",
          retention_seconds(3000, 60) < 3000 * 60)

    # --- the walk ----------------------------------------------------------
    history = SegmentHistory(800)
    for record in _fake_records(800):
        history.append(record)
    tap = _Tap(history)
    result = collect(tap, limit=200, overlap=5)
    check("a first run reads the whole buffer",
          len(result.records) == 800 and result.reached_oldest,
          f"{len(result.records)} records in {result.pages} pages")
    check("...oldest first, with no duplicates",
          [r["t"] for r in result.records] == sorted(
              {r["t"] for r in result.records}))

    known = result.records[-1]["t"]
    tap = _Tap(SegmentHistory(800))
    for record in history.all_records():
        tap.history.append(record)
    for record in _fake_records(30, start=known + 60):
        tap.history.append(record)
    result = collect(tap, known_newest=known, limit=200, overlap=5)
    check("a second run stops as soon as it meets known data",
          result.pages == 1, str(result.pages))
    check("...carrying the 30 new records",
          len([r for r in result.records if r["t"] > known]) == 30,
          str(len([r for r in result.records if r["t"] > known])))
    check("...and the overlap with what is held, for the merge to prove the key",
          len([r for r in result.records if r["t"] <= known]) > 0)
    check("...and reports no gap",
          result.gap_before is None, str(result.gap_before))

    # A rotation between two pages is the silent-drop case. Without the guards
    # the walk re-reads the newest records, believes it has met known data and
    # never sees the segment in between.
    history = SegmentHistory(800)
    for record in _fake_records(800):
        history.append(record)
    tap = _Tap(history, rotate_after=1)
    result = collect(tap, limit=200, overlap=5)
    check("a rotation mid-walk is caught and the walk restarts",
          result.restarts == 1, f"restarts {result.restarts}")
    check("...and the restarted walk is still complete and contiguous",
          result.records and coverage_gaps(
              [r["t"] for r in result.records], 60) == [],
          str(coverage_gaps([r["t"] for r in result.records], 60)))
    check("...and it said why",
          any("rotated" in note for note in result.notes), str(result.notes))

    # The nasty variant: rotate AND refill, so `stored` comes back to where it
    # was while every index has moved by a segment. The device's own count says
    # nothing happened, and an overlap test that only asks "do these two pages
    # share ANY record" passes - the shifted page overlaps its predecessor by
    # far MORE than the five records it was supposed to. Only the exact
    # arithmetic in _pages_abut() catches it.
    history = SegmentHistory(800)
    for record in _fake_records(800):
        history.append(record)
    tap = _Tap(history, refill_after=1)
    before_stored = history.stored
    result = collect(tap, limit=200, overlap=5)
    check("a rotate-and-refill leaves `stored` where it was",
          tap.history.stored == before_stored,
          f"{before_stored} -> {tap.history.stored}")
    check("...and is caught anyway, by the pages not abutting",
          result.restarts >= 1 and any("abut" in note
                                       for note in result.notes),
          f"restarts {result.restarts}: {result.notes}")
    check("...leaving a contiguous archive rather than a short one",
          result.records and coverage_gaps(
              [r["t"] for r in result.records], 60) == [],
          str(coverage_gaps([r["t"] for r in result.records], 60)))

    # And the guard must not fire on the ordinary case, or every run restarts.
    history = SegmentHistory(800)
    for record in _fake_records(800):
        history.append(record)
    result = collect(_Tap(history), limit=200, overlap=5)
    check("an undisturbed walk never trips the abutment check",
          result.restarts == 0 and len(result.records) == 800,
          f"restarts {result.restarts}, {len(result.records)} records")

    # An ordinary append mid-walk shifts no index and must NOT cost a restart:
    # a guard that fires once a minute is a guard nobody keeps. The buffer is
    # deliberately 750 of 800, so the newest segment has room and the append is
    # an append - on a buffer sitting exactly at a segment boundary the very
    # next append IS a rotation, which is the case above.
    history = SegmentHistory(800)
    for record in _fake_records(750):
        history.append(record)
    tap = _Tap(history, append_after=1)
    result = collect(tap, limit=200, overlap=5)
    check("an append mid-walk is not mistaken for a rotation",
          result.restarts == 0 and len(result.records) >= 750,
          f"restarts {result.restarts}, {len(result.records)} records")

    # The collector was away longer than the buffer holds.
    history = SegmentHistory(800)
    for record in _fake_records(2000):
        history.append(record)
    result = collect(_Tap(history), known_newest=SANE_EPOCH + 86400,
                     limit=200, overlap=5)
    check("a collector that was away too long reports the gap",
          result.gap_before is not None, str(result.gap_before))

    # --- identity ----------------------------------------------------------
    one = _fake_records(1)[0]
    check("the same record digests the same twice",
          record_digest(one) == record_digest(dict(one)))
    check("...and a changed reading digests differently",
          record_digest(one) != record_digest({**one, "lum": 51.0}))
    check("...while the timestamp is NOT part of the digest",
          record_digest(one) == record_digest({**one, "t": one["t"] + 60}))
    check("a 1970 record is refused rather than merged",
          not record_is_sane({"t": 90000})[0])
    check("...naming g_safeTimestamp as the reason",
          str(SANE_EPOCH) in (record_is_sane({"t": 90000})[1] or ""))

    # --- the adapter the fit consumes --------------------------------------
    records = _fake_records(10)
    records[3]["relays"] = 1
    records[4]["relays"] = 1   # a sticky mask spanning two records
    records[7]["relays"] = 1
    events = events_for_relay(records, 0, 60)
    check("a sticky mask over two records is ONE watering",
          len(events) == 2, str(events))
    check("...and a relay of -1 has no events",
          events_for_relay(records, -1, 60) == [])
    first_on = _fake_records(4)
    for record in first_on:
        record["relays"] = 1
    check("a watering already running at the oldest record is not an event",
          events_for_relay(first_on, 0, 60) == [])
    across = _fake_records(3) + _fake_records(3, start=SANE_EPOCH + 200000)
    for record in across[3:]:
        record["relays"] = 1
    check("a set bit on the far side of a GAP is not an event either",
          events_for_relay(across, 0, 60) == [],
          str(events_for_relay(across, 0, 60)))
    check("consumedUntil is the newest edge across every probe's pump",
          consumed_until(records, [0, -1]) == int(records[7]["t"]),
          str(consumed_until(records, [0, -1])))
    check("...and 0 when no pump has ever run",
          consumed_until(_fake_records(10), [0, 1]) == 0)

    check("a null moisture slot is dropped, not read as zero",
          len(samples_for_probe(records, 1)) == 0
          and len(samples_for_probe(records, 0)) == 10)

    check("a gap in the archive is reported with the records it lost",
          coverage_gaps([0, 60, 120, 600, 660], 60)[0]["missing"] == 7,
          str(coverage_gaps([0, 60, 120, 600, 660], 60)))
    check("...and an ordinary late tick is not a gap",
          coverage_gaps([0, 60, 121, 181], 60) == [])
    check("the observed period is the median spacing, not the configured one",
          observed_period([0, 60, 120, 181, 241]) == 60)

    return out
