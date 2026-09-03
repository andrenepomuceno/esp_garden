#!/usr/bin/env python3
"""Fits the cloud-cover model from the device's own luminosity archive.

    python scripts/cloud_fit.py                    # fit, report, write the header
    python scripts/cloud_fit.py --dry-run          # fit and report only
    python scripts/cloud_fit.py --since 2026-09-10 # after moving the sensor

WHAT IT PRODUCES

  backups/telemetry.sqlite  ->  include/core/clear_sky_table.h

A clear-sky reference per time-of-day bin, a daylight window, and the handful
of thresholds src/cloud_cover.cpp turns into a state and a transient episode.
Standard library only, same as tb_export.py and dev_server.py next door.

WHY THE REFERENCE IS EMPIRICAL

This sensor peaks around 15:00 local, about three hours after solar noon, and
its mornings read three to five times lower than its afternoons. A solar
geometry model would read all of that as cloud, every morning, for ever. The
reference is therefore the upper envelope of what THIS sensor has actually
measured at each time of day, which absorbs the mounting, the shading and the
LDR's own transfer curve at the price of describing one sensor in one place.

WHY IT MUST BE RE-RUN, AND WHAT INVALIDATES THE FIT

  - Moving the sensor, or anything new shading it. The archive already contains
    one such break: everything up to 2026-08-27 was measured at a different
    mounting and is excluded by the default --since. THE REGIME TABLE PRINTED
    BELOW IS HOW YOU SEE THE NEXT ONE — a row whose ratios are low in a
    time-of-day-locked way on days whose weather had nothing in common is a
    geometry change, not a week of bad luck.
  - The season. The fit here rests on ten days at the end of the southern
    winter. Day length and solar elevation move underneath it, so a reference
    fitted in September overstates the June sky and understates December's.
  - Replacing the LDR or its divider, which rescales everything at once.

RE-FITTING THE TRANSIENT THRESHOLDS NEEDS 60 s DATA, AND THE DEVICE NO LONGER
PUBLISHES AT 60 s. The archive's `luminosity` is the mean over one publish
period, so while mqtt.publishSec was 60 the stored series was exactly the
minute means the firmware computes internally. At 300 s it is not, and the
|dk| statistics below fall back to whatever pairs are one minute apart. The
envelope itself is unaffected — a 300 s mean is still an observation of the
sky — but the transient numbers can only be re-derived from a stretch of
60 s archive.
"""

from __future__ import annotations

import argparse
import datetime as dt
import math
import sqlite3
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DB = ROOT / "backups" / "telemetry.sqlite"
DEFAULT_OUT = ROOT / "include" / "core" / "clear_sky_table.h"

# The device runs on `<-03>3` (config.timezone), and every schedule, log line
# and history record is stamped in that local time. The model keys off the local
# minute of the day, so the fit has to agree.
DEFAULT_TZ_HOURS = -3

# The sensor's mounting changed between 2026-08-27 and 2026-08-28: first light
# moved 20-35 minutes later and every hour before 14:00 lost 20-80 % of its
# maximum, identically across six days of unrelated weather. Fitting across the
# break would build a reference no geometry ever produced.
DEFAULT_SINCE = "2026-08-28"

# Ten minutes: 144 bins is 288 bytes of flash, each bin holds ~60 samples per
# six days of 60 s data, and the reference moves little enough inside one bin
# that linear interpolation between centres covers the rest. Measured against
# 5/15/20/30 minute bins, this had the flattest per-bin median clearness index.
DEFAULT_BIN_MINUTES = 10

# The 98th percentile of everything measured in a bin. The maximum is one
# sample of one day; the 95th leaves 6.4 % of readings above the reference,
# which reads as a ceiling nobody reaches from below.
DEFAULT_QUANTILE = 0.98

# The daylight window is the run of bins around the peak whose reference is at
# least this fraction of it. Below a quarter of peak the ratio stops meaning
# anything: the reference is a handful of points, and the artificial light this
# garden sees at night reaches 13-14 points on its own.
DEFAULT_FLOOR_FRACTION = 0.25

# Thresholds on the clearness index. NOT the solar literature's K_t bands: an
# LDR read as a fraction of full scale compresses irradiance, so the scale is
# the sensor's own. See the report these came from in CLAUDE.md.
CLEAR_ABOVE = 0.85
OVERCAST_BELOW = 0.50
STATE_BAND = 0.05
STATE_ALPHA = 0.30
STATE_DWELL = 5

VAR_ALPHA = 0.30
TRANSIENT_ENTER = 0.070
TRANSIENT_EXIT = 0.035
TRANSIENT_EXIT_RUN = 3


# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------


def quantile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    k = (len(s) - 1) * p
    lo = int(math.floor(k))
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def load(db: Path, key: str, tz_hours: int) -> list[tuple[dt.datetime, float]]:
    if not db.exists():
        sys.exit(f"cloud_fit: no archive at {db}. Run scripts/tb_export.py first.")
    tz = dt.timezone(dt.timedelta(hours=tz_hours))
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    try:
        rows = con.execute(
            "SELECT ts, value FROM telemetry WHERE key = ? ORDER BY ts", (key,)
        ).fetchall()
    finally:
        con.close()
    out = []
    for ts, value in rows:
        try:
            out.append((dt.datetime.fromtimestamp(ts / 1000, tz), float(value)))
        except (TypeError, ValueError):
            continue  # a non-numeric point is a cloud-side artefact, not a reading
    return out


def minute_of_day(when: dt.datetime) -> int:
    return when.hour * 60 + when.minute


# ---------------------------------------------------------------------------
# the fit
# ---------------------------------------------------------------------------


class Reference:
    """The clear-sky envelope and the daylight window fitted with it."""

    def __init__(self, values, bin_minutes, quantile_p, floor_fraction):
        self.bin_minutes = bin_minutes
        self.bins = 1440 // bin_minutes
        pooled = defaultdict(list)
        for when, value in values:
            pooled[minute_of_day(when) // bin_minutes].append(value)
        self.counts = [len(pooled.get(b, [])) for b in range(self.bins)]
        raw = [quantile(pooled.get(b, []), quantile_p) for b in range(self.bins)]

        # One (1, 2, 1) pass. A moving MAXIMUM was tried first and is wrong on a
        # ramp: it drags the brightest sample in a bin up to fifteen minutes
        # earlier, which on the morning climb shifts the whole reference.
        n = self.bins
        self.table = [
            (raw[(b - 1) % n] + 2 * raw[b] + raw[(b + 1) % n]) / 4.0 for b in range(n)
        ]

        self.peak = max(self.table)
        peak_bin = self.table.index(self.peak)
        floor = floor_fraction * self.peak
        lo = hi = peak_bin
        while lo - 1 >= 0 and self.table[lo - 1] >= floor:
            lo -= 1
        while hi + 1 < n and self.table[hi + 1] >= floor:
            hi += 1
        self.first_minute = lo * bin_minutes
        self.last_minute = hi * bin_minutes + bin_minutes - 1
        self.floor = floor

    def at(self, minute: int) -> float:
        """Interpolated between bin centres, 0 outside the window.

        Mirrors cloudClearSky() in src/cloud_cover.cpp exactly; if the two ever
        disagree the reported numbers describe a model the device is not
        running.
        """
        if not (self.first_minute <= minute <= self.last_minute):
            return 0.0
        half = self.bin_minutes / 2.0
        x = (minute - half) / self.bin_minutes
        lo = int(math.floor(x))
        frac = x - lo
        lo %= self.bins
        hi = (lo + 1) % self.bins
        return self.table[lo] * (1 - frac) + self.table[hi] * frac

    def clearness(self, when: dt.datetime, value: float):
        ref = self.at(minute_of_day(when))
        return None if ref <= 0.0 else value / ref


def hhmm(minute: int) -> str:
    return f"{minute // 60:02d}:{minute % 60:02d}"


# ---------------------------------------------------------------------------
# the same state machine the firmware runs, so the report describes the device
# ---------------------------------------------------------------------------


def classify(k: float) -> int:
    return 3 if k >= CLEAR_ABOVE else (1 if k < OVERCAST_BELOW else 2)


def with_hysteresis(current: int, k: float) -> int:
    if current == 0:
        return classify(k)
    lower, upper = OVERCAST_BELOW, CLEAR_ABOVE
    if current == 1:
        lower += STATE_BAND
    elif current == 3:
        upper -= STATE_BAND
    else:
        lower -= STATE_BAND
        upper += STATE_BAND
    return 3 if k >= upper else (1 if k < lower else 2)


NAMES = {0: "unknown", 1: "overcast", 2: "partly", 3: "clear"}


def simulate(day_series: list[tuple[dt.datetime, float]]):
    """Runs one day through the firmware's logic. Returns (changes, occupancy,
    episodes, total episode minutes)."""
    smooth = None
    prev = None
    var = 0.0
    state = 0
    candidate = 0
    run = 0
    changes = 0
    occupancy = Counter()
    in_ep = False
    below = 0
    started = None
    episodes = []

    for when, k in day_series:
        if prev is not None:
            dk = abs(k - prev)
            var += VAR_ALPHA * (dk - var)
            if not in_ep:
                if var > TRANSIENT_ENTER:
                    in_ep, below, started = True, 0, when
            else:
                if var < TRANSIENT_EXIT:
                    below += 1
                    if below >= TRANSIENT_EXIT_RUN:
                        in_ep = False
                        episodes.append((when - started).total_seconds() / 60.0)
                else:
                    below = 0
        prev = k

        smooth = k if smooth is None else smooth + STATE_ALPHA * (k - smooth)
        nxt = with_hysteresis(state, smooth)
        if nxt == state:
            candidate, run = 0, 0
        elif state == 0:
            state, changes, candidate, run = nxt, changes + 1, 0, 0
        else:
            if nxt == candidate:
                run += 1
            else:
                candidate, run = nxt, 1
            if run >= STATE_DWELL:
                state, changes, candidate, run = nxt, changes + 1, 0, 0
        occupancy[state] += 1

    if in_ep and started is not None:
        episodes.append((day_series[-1][0] - started).total_seconds() / 60.0)
    return changes, occupancy, episodes


# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------


def report_regime(points, reference, fitted_days):
    """Per-day, per-hour maximum as a fraction of the fitted envelope's.

    This is the table that found the 2026-08-28 break, and it is printed for
    EVERY day in the archive — the excluded ones included — because a reference
    fitted on one geometry is only trustworthy while the sensor still has it.
    """
    print("\n-- regime check: hourly max / fitted envelope's hourly max --------")
    print("   A day whose ratios are low in the same hours as its neighbours,")
    print("   across unrelated weather, is a MOUNTING change. Re-fit --since it.")
    # Daylight hours only. The night bins carry this garden's artificial light —
    # a 13-point plateau between 19:30 and 21:30 on some evenings — and a ratio
    # against a porch lamp says nothing about where the sun is.
    env_hour = defaultdict(float)
    for b, value in enumerate(reference.table):
        minute = b * reference.bin_minutes
        if reference.first_minute <= minute <= reference.last_minute:
            env_hour[minute // 60] = max(env_hour[minute // 60], value)
    day_hour = defaultdict(float)
    for when, value in points:
        day_hour[(when.date(), when.hour)] = max(
            day_hour[(when.date(), when.hour)], value
        )
    days = sorted({d for d, _ in day_hour})
    hours = sorted(env_hour)
    print("   hour  " + " ".join(f"{d.strftime('%m-%d')}" for d in days))
    print("         " + " ".join(("  fit" if d in fitted_days else "  ---") for d in days))
    for h in hours:
        cells = " ".join(f"{day_hour.get((d, h), 0.0) / env_hour[h]:5.2f}" for d in days)
        print(f"   {h:4d}  {cells}")


def report(points, fitted, reference, args):
    fitted_days = {when.date() for when, _ in fitted}
    print("=" * 74)
    print(f"cloud_fit: {len(points)} points of '{args.key}', "
          f"{points[0][0].date()} .. {points[-1][0].date()} (UTC{args.tz:+d})")
    gaps = Counter(
        round((points[i + 1][0] - points[i][0]).total_seconds())
        for i in range(len(points) - 1)
    )
    print("  cadence (s): " + ", ".join(f"{s}x{n}" for s, n in gaps.most_common(4)))
    print(f"  fitted on {len(fitted_days)} day(s) from --since {args.since}: "
          f"{len(fitted)} points")
    if len(fitted_days) < 10:
        print(f"  ! {len(fitted_days)} days is THIN. Every threshold below is a "
              "choice this much data cannot settle on its own.")

    print("\n-- clear-sky envelope ---------------------------------------------")
    print(f"  {reference.bins} bins of {reference.bin_minutes} min, "
          f"q={args.quantile}, one (1,2,1) smoothing pass")
    print(f"  peak {reference.peak:.2f} at "
          f"{hhmm(reference.table.index(reference.peak) * reference.bin_minutes)}, "
          f"floor {reference.floor:.2f} ({args.floor:.2f} x peak)")
    print(f"  daylight window {hhmm(reference.first_minute)} .. "
          f"{hhmm(reference.last_minute)} local")

    per_bin = defaultdict(list)
    ks = []
    for when, value in fitted:
        k = reference.clearness(when, value)
        if k is not None:
            per_bin[minute_of_day(when) // reference.bin_minutes].append(k)
            ks.append(k)
    if not ks:
        sys.exit("cloud_fit: no readings fall inside the fitted window.")

    print("\n   time    ref     n   median k   (a reference that fits gives a")
    print("                              flat median k across the window)")
    for b in sorted(per_bin):
        print(f"   {hhmm(b * reference.bin_minutes)}  {reference.table[b]:6.2f} "
              f"{reference.counts[b]:5d}   {statistics.median(per_bin[b]):.2f}")
    medians = [statistics.median(v) for v in per_bin.values()]
    print(f"   per-bin median k spans {min(medians):.2f}..{max(medians):.2f}, "
          f"sd {statistics.pstdev(medians):.3f}")

    over = 100.0 * sum(1 for k in ks if k > 1.0) / len(ks)
    print(f"\n  clearness index over {len(ks)} readings: "
          + "  ".join(f"p{p}={quantile(ks, p / 100):.3f}" for p in (5, 25, 50, 75, 95)))
    print(f"  above the reference: {over:.2f} % (an envelope, not a ceiling)")

    print("\n-- states and transients, per day, as the firmware would run them --")
    print(f"  clear k>={CLEAR_ABOVE}, overcast k<{OVERCAST_BELOW}, band "
          f"+-{STATE_BAND}, EWMA {STATE_ALPHA}, dwell {STATE_DWELL} min")
    print(f"  transient EWMA {VAR_ALPHA} on |dk|, enter {TRANSIENT_ENTER}, "
          f"exit {TRANSIENT_EXIT} for {TRANSIENT_EXIT_RUN} min")
    by_day = defaultdict(list)
    for when, value in fitted:
        k = reference.clearness(when, value)
        if k is not None:
            by_day[when.date()].append((when, k))
    print("\n   day         n   med k   clear  partly  overcast | changes  "
          "episodes  minutes")
    for day in sorted(by_day):
        series = sorted(by_day[day])
        changes, occ, eps = simulate(series)
        total = sum(occ.values()) or 1
        print(f"   {day} {len(series):4d}   {statistics.median(k for _, k in series):.3f}"
              f"  {100 * occ[3] / total:5.1f}%  {100 * occ[2] / total:5.1f}%  "
              f"{100 * occ[1] / total:7.1f}% | {changes:7d}  {len(eps):8d}  "
              f"{sum(eps):7.0f}")

    deltas = []
    for day in sorted(by_day):
        series = sorted(by_day[day])
        for i in range(len(series) - 1):
            gap = (series[i + 1][0] - series[i][0]).total_seconds()
            if 30 <= gap <= 100:
                deltas.append(abs(series[i + 1][1] - series[i][1]))
    if deltas:
        print(f"\n  |dk| over {len(deltas)} one-minute pairs: "
              + "  ".join(f"p{p}={quantile(deltas, p / 100):.4f}"
                          for p in (50, 90, 95, 99)))
    else:
        print("\n  ! no one-minute pairs in the fitted range: the transient "
              "thresholds cannot be re-derived from this data (see the module "
              "docstring).")

    report_regime(points, reference, fitted_days)


# ---------------------------------------------------------------------------
# the generated header
# ---------------------------------------------------------------------------


def emit(reference, fitted, args) -> str:
    days = sorted({when.date() for when, _ in fitted})
    table = [max(0, min(65535, int(round(v * 100)))) for v in reference.table]
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include \"core/cloud_cover.h\"")
    lines.append("")
    lines.append("// GENERATED by scripts/cloud_fit.py -- do not edit by hand.")
    lines.append("//")
    lines.append(f"// Fitted from backups/telemetry.sqlite, key '{args.key}', on")
    lines.append(f"// {len(days)} day(s): {days[0]} .. {days[-1]} "
                 f"({len(fitted)} readings, UTC{args.tz:+d}).")
    lines.append("//")
    lines.append("// THIS TABLE DESCRIBES ONE SENSOR AT ONE MOUNTING IN ONE SEASON.")
    lines.append("// Moving the sensor, shading it, replacing the LDR or letting the")
    lines.append("// season move under it all invalidate it, and nothing on the device")
    lines.append("// can detect that it has happened. Re-run the fit; the regime table")
    lines.append("// it prints is what makes a mounting change visible.")
    lines.append("//")
    lines.append(f"// Envelope: {reference.bins} bins x {reference.bin_minutes} min, "
                 f"{args.quantile:g} quantile, one (1,2,1) pass.")
    lines.append(f"// Peak {reference.peak:.2f} %, daylight window "
                 f"{hhmm(reference.first_minute)}..{hhmm(reference.last_minute)} local.")
    lines.append("//")
    lines.append("// Hundredths of a percent of full scale, indexed by "
                 f"minute_of_day / {reference.bin_minutes}.")
    lines.append(f"static const uint16_t g_clearSkyTable[{reference.bins}] = {{")
    for start in range(0, reference.bins, 12):
        row = table[start:start + 12]
        stamp = hhmm(start * reference.bin_minutes)
        lines.append("    " + ", ".join(f"{v:5d}" for v in row) + f",  // {stamp}")
    lines.append("};")
    lines.append("")
    lines.append("static const CloudModelParams g_cloudParams = {")
    lines.append("    g_clearSkyTable,")
    lines.append(f"    {reference.bins}, {reference.bin_minutes},")
    lines.append(f"    {reference.first_minute}, {reference.last_minute},")
    lines.append(f"    {OVERCAST_BELOW:.2f}f, {CLEAR_ABOVE:.2f}f, "
                 f"{STATE_BAND:.2f}f, {STATE_ALPHA:.2f}f, {STATE_DWELL},")
    lines.append(f"    {VAR_ALPHA:.2f}f, {TRANSIENT_ENTER:.3f}f, "
                 f"{TRANSIENT_EXIT:.3f}f, {TRANSIENT_EXIT_RUN},")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", type=Path, default=DEFAULT_DB)
    parser.add_argument("--key", default="luminosity")
    parser.add_argument("--tz", type=int, default=DEFAULT_TZ_HOURS,
                        help="hours from UTC; must match config.timezone")
    parser.add_argument("--since", default=DEFAULT_SINCE,
                        help="first local date to fit on (YYYY-MM-DD)")
    parser.add_argument("--bin", type=int, default=DEFAULT_BIN_MINUTES)
    parser.add_argument("--quantile", type=float, default=DEFAULT_QUANTILE)
    parser.add_argument("--floor", type=float, default=DEFAULT_FLOOR_FRACTION)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--dry-run", action="store_true",
                        help="report the fit without writing the header")
    args = parser.parse_args()

    if 1440 % args.bin:
        sys.exit(f"cloud_fit: --bin {args.bin} does not divide 1440 minutes.")

    points = load(args.db, args.key, args.tz)
    if len(points) < 500:
        sys.exit(f"cloud_fit: only {len(points)} points of '{args.key}'. "
                 "A day of daylight is ~600; fitting on less is guessing.")

    since = dt.date.fromisoformat(args.since)
    fitted = [(w, v) for w, v in points if w.date() >= since]
    if len(fitted) < 500:
        sys.exit(f"cloud_fit: only {len(fitted)} points since {since}.")

    reference = Reference(fitted, args.bin, args.quantile, args.floor)
    report(points, fitted, reference, args)

    header = emit(reference, fitted, args)
    if args.dry_run:
        print("\ncloud_fit: --dry-run, nothing written.")
        return 0
    args.out.write_text(header, encoding="utf-8")
    print(f"\ncloud_fit: wrote {args.out.relative_to(ROOT).as_posix()} "
          f"({reference.bins * 2} bytes of table).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
