#!/usr/bin/env python3
"""Does soil drying decay exponentially, and if so, where does it end up?

    python scripts/drying_fit.py                 # segment, fit, judge, report
    python scripts/drying_fit.py --stitch-seam   # rejoin the renamed probe
    python scripts/drying_fit.py --json out.json
    python scripts/drying_fit.py --self-test     # the pure logic only

THE QUESTION, AND WHY IT IS WORTH ASKING

CLAUDE.md's two-point calibration needs a `dry` anchor per probe, and this
garden has never been dry. The one anchor on record - 53.11 on Zona 1 - was
taken by carrying the probe into a different, dry pot, and it is an UPPER bound
because the probe was still falling when it came out. If drying is a decay
towards an asymptote then fitting the decay estimates the dry end without
waiting for a zone to dry out, which is the whole prize.

The firmware already fits the other half of the same physics.
`moistureTimeConstant()` in src/moisture_classifier.cpp estimates tau for the
RISE after a watering, m(t) = baseline + rise * (1 - exp(-(t-T)/tau)). Nothing
fits the fall.

WHAT THE ARCHIVE ACTUALLY CONTAINS - TWO DECAYS, DIFFERENT PHYSICS

  Soil drying          water leaving the soil. What is being asked about.
  Probe handling       water leaving the PROBE after somebody moved it between
                       a pot, the air and a glass of water. On the evening of
                       2026-09-03 both probes did exactly that.

A tau fitted across both describes neither, so they are separated before
anything is fitted. The separator is not a rate - a probe draining after a
watering falls as fast as a handled one, measured here - it is DISCONTINUITY.
Soil is continuous: it cannot step 17 points between two publishes, and it
cannot step up and then down inside two hours. So a cluster of steps is
handling, and a single rise is a watering. `find_steps` and
`find_drift_segments` come from moisture_stats.py rather than being written
again here, because a second copy of a detector is a second set of thresholds
to keep in step.

WHAT IT REFUSES, AND WHY REFUSING IS THE POINT

An asymptote extrapolated from a partial decay is ill-conditioned: the fit will
always produce a number, and the number is usually wherever the series happens
to have got to. So an asymptote is printed only when four checks pass, and the
refusal names the check - the same contract /moisture.json keeps with
`blockedBy` and moisture_fit.py keeps with its four refusals.

Standard library only. Reads backups/telemetry.sqlite READ-ONLY and touches
nothing else: no device, no config, no write anywhere.
"""

from __future__ import annotations

import argparse
import json
import math
import sqlite3
import sys
from pathlib import Path

# Plain sibling modules: this file is run as a script, so scripts/ is
# sys.path[0], the arrangement moisture_fit.py already has with moisture_stats.
from drying_evidence import (block_bootstrap, choose, criteria,
                             profile_interval, start_sensitivity)
from drying_evidence import self_test as evidence_self_test
from drying_models import MODELS, asymptote, fit, is_degenerate, resample
from drying_models import self_test as models_self_test
from moisture_stats import (MAX_GAP_SEC, STEP_MIN_POINTS, find_drift_segments,
                            find_steps, local)
from tb_export import SEAM_MS

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DB = ROOT / "backups" / "telemetry.sqlite"
DEFAULT_TZ_HOURS = -3

# The publish period changed from 60 s to 300 s on 2026-09-03 18:43, so a
# segment spanning that instant carries five times the weight per hour on one
# side. Everything is binned to the coarser period before fitting; the finer
# samples are averaged, not thrown away.
RESAMPLE_SEC = 300.0

# ---------------------------------------------------------------------------
# Segmentation thresholds. Each says what it was measured against.
# ---------------------------------------------------------------------------

# HANDLING: two or more steps inside two hours. Soil moisture is monotone
# between wettings, so a probe that jumps up and then down has been moved.
# Measured 2026-09-03: eight steps of 5-27 points inside 70 minutes while the
# probes were carried between pot, air and water. Against that, the largest
# single-publish jump in three days of undisturbed soil is 0.05 points.
HANDLING_CLUSTER_SEC = 2 * 3600
HANDLING_MIN_STEPS = 2

# How long a moved probe is given to re-equilibrate before its readings count
# again. A JUDGEMENT, and the archive does not settle it: the last handled
# probe was still falling 0.3 points per five minutes 105 minutes after the
# final step, and then the archive ends. Two hours is the observed minimum,
# not a measured settling time.
HANDLING_SETTLE_SEC = 2 * 3600

# ...and it is a --settle-hours flag rather than a constant, because it decides
# the answer: it changes which model wins the two longest segments, and with it
# whether a fast tau is reported at all. Both settings are honest and they
# disagree, so both are reachable from the command line and neither is buried
# in a constant.
#
# It is threaded through segment() and handling_windows() as an ARGUMENT. It
# used to be a module global that --settle-hours reassigned, so the number
# deciding the answer appeared in no signature, and self_test() - which runs
# before main() sets it - silently read whatever the global happened to hold.

# WETTING: a rise of two points inside half an hour opens a new segment at the
# peak. The garden's own diurnal wobble is 0.6 points over three hours,
# measured 2026-09-01; the smallest hand watering on record moved 13.8 points
# in 11 minutes. Two points sits between them with an order of magnitude
# either side. Relay events are NOT used for this - CLAUDE.md records that this
# garden is also watered by hand, so the relay log is not a complete label.
WETTING_RISE_POINTS = 2.0
WETTING_WINDOW_SEC = 30 * 60

# What counts as a segment worth fitting at all.
MIN_SEGMENT_SEC = 6 * 3600
MIN_SEGMENT_POINTS = 30
MIN_DECLINE_POINTS = 1.0

# A segment beginning within this of a handling episode or a data gap is
# reported as CONFOUNDED rather than dropped: its early hours could be the pot
# draining or the probe settling, and nothing in the series distinguishes them.
# Dropping it would throw away the only steep stretches the archive has.
CONFOUND_SEC = 6 * 3600

# ---------------------------------------------------------------------------
# What an asymptote has to clear before it is printed.
# ---------------------------------------------------------------------------

# The two-point calibration currently spans 20.4 points on Zona 1 (dry 53.1,
# wet 73.5) and moistureState() cuts it in thirds, so one class is 6.8 points
# wide. An anchor uncertain by +/-3 moves every boundary by 2 - nearly a third
# of a class - and that is the most a badge can absorb and still mean anything.
ASYMPTOTE_MAX_WIDTH = 6.0

# Trimming the start of the record must not move the answer by more than this.
# It is the sharpest of the four checks: an asymptote fixed by the shape of the
# curve does not care where the record begins, and one fixed by where the
# record happens to stop moves freely.
ASYMPTOTE_MAX_START_DRIFT = 3.0

# A later reading below the asymptote, with no wetting in between, falsifies
# it outright. The tolerance is measurement noise, not slack.
ASYMPTOTE_FALSIFY_TOLERANCE = 0.25

# AND THE ASYMPTOTE HAS TO SAY SOMETHING THE LAST SAMPLE DOES NOT.
#
# This gate was added because the tool passed one without it. On a segment
# where the decay had already finished, the fit returned tau = 0.39 h over an
# 8.2 h window and an asymptote of 42.10 against a final reading of 42.24 -
# bounded profile, stable under trimming, every statistic green. It is also
# worthless: it says the level the pot settled at, which is readable off the
# last sample, and calls it "fully dry" when the soil was merely no longer
# draining. A completed decay's asymptote is the baseline BETWEEN waterings,
# not the dry end.
#
# So an asymptote must sit at least this far below the last observation, or the
# extrapolation has extrapolated nothing. Two points, because the running
# minimum of the record is already free and an anchor that beats it by less
# than that is inside the day-to-day spread of the readings themselves.
#
# It is worth seeing what this gate does WITH the profile gate above it: a
# decay watched to completion is identified but uninformative, and one still
# bending is informative but unidentified. On a partial decay those two are the
# same dial turned opposite ways, which is the honest content of the whole
# exercise.
ASYMPTOTE_MIN_EXTRAPOLATION = 2.0

# A time constant shorter than two sample periods is not measured, it is
# guessed between two points. At the 300 s publish period the archive has since
# 2026-09-03 18:43 that is ten minutes, which matters: the observation that
# started this - a probe holding an apparent plateau for THREE MINUTES before
# resuming its fall - is entirely below the archive's resolution, and no fit
# to these series can confirm or deny it. A second exponential faster than this
# is reported as UNRESOLVED rather than as a second timescale.
TAU_UNRESOLVED_STEPS = 2.0


# ---------------------------------------------------------------------------
# the archive
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


def channels(cursor, since, until, stitch):
    """The moisture series, and the one place the archive lies about identity.

    tb_export.SEAM_MS records that at 2026-09-02 13:41:29 `moisture3` stopped
    and `moisture2` took over the SAME physical sensor, because the archive
    keys probes positionally and a slot was deleted. Off by default the seam is
    a hard boundary, exactly as moisture_fit.py treats it; --stitch-seam joins
    the two halves into one channel on the strength of that recorded identity.
    The join is checked rather than assumed - the report prints the gap across
    it, and here it is 0.08 points across 76 seconds.
    """
    found = {}
    for index in range(1, 5):
        key = "moisture%d" % index
        series = read_series(cursor, key, since, until)
        if series:
            found[key] = series
    if not stitch:
        for key, series in list(found.items()):
            if key in ("moisture2", "moisture3"):
                seam = SEAM_MS / 1000.0
                before = [p for p in series if p[0] < seam]
                after = [p for p in series if p[0] >= seam]
                keep = before if len(before) >= len(after) else after
                found[key] = keep
        return found, None

    seam = SEAM_MS / 1000.0
    before = [p for p in found.get("moisture3", []) if p[0] < seam]
    after = [p for p in found.get("moisture2", []) if p[0] >= seam]
    if not before or not after:
        return found, None
    found.pop("moisture3", None)
    found.pop("moisture2", None)
    found["zona3 (moisture3+moisture2)"] = before + after
    joint = {"lastBefore": before[-1], "firstAfter": after[0],
             "gapSec": after[0][0] - before[-1][0],
             "gapPoints": after[0][1] - before[-1][1]}
    return found, joint


# ---------------------------------------------------------------------------
# separating soil drying from a probe somebody picked up
# ---------------------------------------------------------------------------


def handling_windows(samples, settle_sec=HANDLING_SETTLE_SEC):
    """Clusters of discontinuities: a probe being moved, not soil drying."""
    steps = find_steps(samples, STEP_MIN_POINTS)
    windows = []
    index = 0
    while index < len(steps):
        end = index
        while (end + 1 < len(steps)
               and steps[end + 1]["at"] - steps[index]["at"] <= HANDLING_CLUSTER_SEC):
            end += 1
        if end - index + 1 >= HANDLING_MIN_STEPS:
            windows.append({
                "from": steps[index]["at"],
                "to": steps[end]["at"] + settle_sec,
                "steps": end - index + 1,
                "deltas": [round(s["delta"], 2) for s in steps[index:end + 1]],
            })
        index = end + 1
    merged = []
    for window in windows:
        if merged and window["from"] <= merged[-1]["to"]:
            merged[-1]["to"] = max(merged[-1]["to"], window["to"])
            merged[-1]["steps"] += window["steps"]
            merged[-1]["deltas"].extend(window["deltas"])
        else:
            merged.append(dict(window))
    return merged, steps


def wetting_events(samples):
    """Rises big enough to end one drying cycle and begin the next.

    Each is returned as (foot, peak) and BOTH are cut on, because they are
    different boundaries. The drying that preceded the water ends at the foot -
    keeping the rise inside it makes the block's net change positive and the
    whole stretch is then thrown away, which on this archive costs ten hours of
    perfectly good decay. The drying that follows begins at the peak, where the
    water stopped arriving; starting at the foot fits the tail of a watering as
    if it were the head of a drying curve.
    """
    events = []
    start = 0
    for index in range(1, len(samples)):
        while (start < index
               and samples[index][0] - samples[start][0] > WETTING_WINDOW_SEC):
            start += 1
        if start >= index:
            continue
        window = list(range(start, index + 1))
        foot = min(window, key=lambda i: samples[i][1])
        if samples[index][1] - samples[foot][1] >= WETTING_RISE_POINTS:
            peak = index
            while (peak + 1 < len(samples)
                   and samples[peak + 1][0] - samples[peak][0] <= MAX_GAP_SEC
                   and samples[peak + 1][1] >= samples[peak][1]):
                peak += 1
            if not events or peak > events[-1][1]:
                events.append((foot, peak))
            start = peak
    return events


def segment(samples, settle_sec=HANDLING_SETTLE_SEC):
    """Every stretch of this channel that is genuinely soil drying."""
    handled, steps = handling_windows(samples, settle_sec)
    step_times = [entry["at"] for entry in steps]

    def in_handling(stamp):
        return any(w["from"] <= stamp <= w["to"] for w in handled)

    clean = [point for point in samples if not in_handling(point[0])]
    if not clean:
        return [], handled, steps

    boundaries = set()
    for index in range(1, len(clean)):
        if clean[index][0] - clean[index - 1][0] > MAX_GAP_SEC:
            boundaries.add(index)
    for stamp in step_times:
        for index in range(1, len(clean)):
            if clean[index - 1][0] < stamp <= clean[index][0]:
                boundaries.add(index)
    for foot, peak in wetting_events(clean):
        boundaries.add(foot)
        boundaries.add(peak)

    cuts = sorted(boundaries | {0, len(clean)})
    blocks = [clean[cuts[i]:cuts[i + 1]] for i in range(len(cuts) - 1)]

    kept = []
    for block in blocks:
        if len(block) < MIN_SEGMENT_POINTS:
            continue
        if block[-1][0] - block[0][0] < MIN_SEGMENT_SEC:
            continue
        if block[0][1] - block[-1][1] < MIN_DECLINE_POINTS:
            continue
        confounds = []
        for window in handled:
            if 0 <= block[0][0] - window["to"] <= CONFOUND_SEC:
                confounds.append("starts %.1f h after a handling episode"
                                 % ((block[0][0] - window["to"]) / 3600.0))
        previous = [p for p in samples if p[0] < block[0][0]]
        if previous and block[0][0] - previous[-1][0] > MAX_GAP_SEC:
            confounds.append("starts after a %.0f min data gap"
                             % ((block[0][0] - previous[-1][0]) / 60.0))
        kept.append({"samples": block, "confounds": confounds})
    return kept, handled, steps


# ---------------------------------------------------------------------------
# the verdict
# ---------------------------------------------------------------------------


def falsified_by_later(level, channel_samples, segment_samples):
    """Did this channel later go BELOW its own fitted asymptote, undisturbed?

    The cheapest check of the four and the most decisive when it fires: an
    asymptote the archive has already walked through is not uncertain, it is
    wrong. Only readings before the next wetting count, because a watering
    resets the curve and a value after one says nothing about this decay.
    """
    end = segment_samples[-1][0]
    later = [point for point in channel_samples if point[0] > end]
    if not later:
        return None
    stop = len(later)
    for foot, _peak in wetting_events(later):
        stop = min(stop, foot)
        break
    for index in range(1, stop):
        if later[index][0] - later[index - 1][0] > MAX_GAP_SEC:
            stop = index
            break
    window = later[:stop]
    if not window:
        return None
    low = min(window, key=lambda point: point[1])
    if low[1] < level - ASYMPTOTE_FALSIFY_TOLERANCE:
        return {"value": low[1], "at": low[0], "below": level - low[1],
                "hoursAfter": (low[0] - end) / 3600.0}
    return None


def judge(analysis):
    """The four checks, in the order they are applied. First failure wins."""
    winner = analysis["winner"]
    if not winner["hasAsymptote"]:
        return ("model", "the held-out winner is '%s', which has no asymptote"
                         " - the record is a decaying transient on a straight"
                         " trend, and a straight trend does not end anywhere"
                         % winner["kind"])
    if winner["degenerate"]:
        return ("degenerate",
                "the winning fit's slowest tau is %.0f h against a %.0f h"
                " window, so that exponential IS a straight line and its"
                " asymptote is the line's intercept"
                % (max(winner["taus"]) / 3600.0, analysis["spanHours"]))
    reach = analysis["last"] - (winner["asymptote"] or 0.0)
    if reach < ASYMPTOTE_MIN_EXTRAPOLATION:
        return ("uninformative",
                "the fitted asymptote %.2f sits only %.2f points below the last"
                " reading %.2f, so the decay finished inside the window: this"
                " is the level the pot settles at between waterings, which the"
                " last sample already gave, and not the dry end"
                % (winner["asymptote"], reach, analysis["last"]))
    interval = analysis.get("profile")
    if interval is None:
        return ("profile", "the profile likelihood could not be evaluated")
    if interval.get("offScale"):
        return ("profile",
                "the fit puts the asymptote at %.1f, off the %.0f-%.0f scale a"
                " moisture reading lives on, and no level on that scale fits"
                " within %.0f %% of its SSE - so there is no interval to report"
                % (interval.get("point", float("nan")), interval["floorLevel"],
                   interval["topLevel"], (interval["threshold"] - 1.0) * 100.0))
    if not interval["boundedBelow"]:
        return ("profile",
                "the 95 %% profile interval reaches the bottom of the scale:"
                " every asymptote from %.1f down to 0 fits within %.0f %% of"
                " the best SSE, at n_eff = %.0f"
                % (interval["high"], (interval["threshold"] - 1.0) * 100.0,
                   interval["nEff"]))
    if not interval["boundedAbove"]:
        return ("profile",
                "the 95 %% profile interval runs up to the highest reading in"
                " the segment, %.1f: the fit is as content with an asymptote at"
                " the top of the record as at %.1f, so nothing above is"
                " excluded either, at n_eff = %.0f"
                % (interval["high"], interval["low"], interval["nEff"]))
    if interval["width"] > ASYMPTOTE_MAX_WIDTH:
        return ("profile",
                "the 95 %% profile interval is %.1f points wide"
                " [%.1f, %.1f], against %.1f allowed"
                % (interval["width"], interval["low"], interval["high"],
                   ASYMPTOTE_MAX_WIDTH))
    drift = analysis.get("startSpread")
    if drift is None or drift > ASYMPTOTE_MAX_START_DRIFT:
        return ("stability",
                "trimming the first fifth of the record moves the asymptote by"
                " %.1f points, against %.1f allowed"
                % (drift if drift is not None else float("nan"),
                   ASYMPTOTE_MAX_START_DRIFT))
    hit = analysis.get("falsified")
    if hit:
        return ("falsified",
                "this probe read %.2f %.1f h later with no watering in"
                " between - %.2f points BELOW the asymptote it was just"
                " assigned" % (hit["value"], hit["hoursAfter"], hit["below"]))
    return (None, None)


def analyse(name, block, channel_samples, step_sec, do_bootstrap=True):
    raw = block["samples"]
    binned = resample(raw, step_sec)
    if len(binned) < 20:
        return None
    span_hours = (binned[-1][0] - binned[0][0]) / 3600.0

    # moisture_fit.py's own transient detector, run over the segment this tool
    # has already decided is genuine drying. It is not used to reject anything
    # here - it flags a WATERING as readily as a handled probe, which its
    # docstring says outright - but the fraction it flags is the honest measure
    # of how much of a segment is transient rather than settled, and it is the
    # number to look at before believing any parameter fitted to it.
    drifts = find_drift_segments(binned)
    inside = sum(1 for stamp, _ in binned
                 if any(seg["from"] <= stamp <= seg["to"] for seg in drifts))
    drift_fraction = inside / float(len(binned))

    fits = {}
    for kind in MODELS:
        model = fit(kind, binned)
        if model is None:
            continue
        stats = criteria(model, binned)
        fits[kind] = {
            "kind": kind,
            "sse": model["sse"],
            "rmse": math.sqrt(model["sse"] / len(binned)),
            "taus": model["taus"],
            "coef": model["coef"],
            "asymptote": asymptote(model),
            "degenerate": is_degenerate(model),
            "unresolved": [t for t in model["taus"]
                           if t < TAU_UNRESOLVED_STEPS * step_sec],
            "hasAsymptote": asymptote(model) is not None,
            "params": model["params"],
            "model": model,
            **stats,
        }

    ranked = choose(binned)
    for entry in ranked:
        if entry["kind"] in fits:
            fits[entry["kind"]]["holdoutRmse"] = entry["rmse"]
            fits[entry["kind"]]["holdoutBias"] = entry["bias"]
            fits[entry["kind"]]["winner"] = entry["winner"]
    winner_kind = next((e["kind"] for e in ranked if e["winner"]), None)
    if winner_kind is None or winner_kind not in fits:
        return None
    winner = fits[winner_kind]

    analysis = {
        "channel": name,
        "from": raw[0][0], "to": raw[-1][0],
        "spanHours": span_hours,
        "rawPoints": len(raw), "points": len(binned),
        "first": binned[0][1], "last": binned[-1][1],
        "confounds": block["confounds"],
        "driftFraction": drift_fraction,
        "driftSegments": len(drifts),
        "fits": fits, "ranked": ranked,
        "winner": winner,
        "winnerKind": winner_kind,
    }

    # The exponential is profiled whether or not it won, because "the
    # exponential lost" and "the exponential won but says nothing" are
    # different answers and the reader is owed both.
    exp_model = fits.get("exp", {}).get("model")
    if exp_model is not None:
        analysis["profileExp"] = profile_interval(binned, exp_model)
        analysis["startExp"] = start_sensitivity("exp", binned)
        analysis["startSpreadExp"] = analysis["startExp"]["spread"]
        level = asymptote(exp_model)
        if level is not None:
            analysis["falsifiedExp"] = falsified_by_later(
                level, channel_samples, raw)
        if do_bootstrap:
            analysis["bootstrapExp"] = block_bootstrap("exp", binned)

    if winner["hasAsymptote"]:
        analysis["profile"] = profile_interval(binned, winner["model"])
        stability = start_sensitivity(winner_kind, binned)
        analysis["start"] = stability
        analysis["startSpread"] = stability["spread"]
        if winner["asymptote"] is not None:
            analysis["falsified"] = falsified_by_later(
                winner["asymptote"], channel_samples, raw)

    analysis["refusedBy"], analysis["refusal"] = judge(analysis)
    if analysis["refusedBy"] is None:
        analysis["dryAnchor"] = winner["asymptote"]
    return analysis


# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------


def hours(seconds):
    return "%.1f h" % (seconds / 3600.0)


def print_report(results, tz, joint, handled_by_channel):
    print("=" * 78)
    print("SOIL DRYING - is it exponential, and does the fit give a dry anchor?")
    print("=" * 78)
    if joint:
        print("\nSeam: moisture3 -> moisture2 rejoined at %s"
              % local(SEAM_MS / 1000.0, tz))
        print("  last before %.2f, first after %.2f: %+.2f points across %.0f s"
              % (joint["lastBefore"][1], joint["firstAfter"][1],
                 joint["gapPoints"], joint["gapSec"]))

    for name, windows in handled_by_channel.items():
        if not windows:
            continue
        print("\nHandling episodes excluded on %s (a cluster of steps soil"
              " cannot make):" % name)
        for window in windows:
            print("  %s -> %s  %d steps, deltas %s"
                  % (local(window["from"], tz), local(window["to"], tz),
                     window["steps"], window["deltas"][:8]))

    if not results:
        print("\nNo segment survived. Nothing to fit.")
        return

    print("\n%d genuine drying segment(s):" % len(results))
    for index, item in enumerate(results, 1):
        print("  [%d] %-26s %s -> %s  %s  %d pts (%d raw)  %.2f -> %.2f"
              "  drift %3.0f%%%s"
              % (index, item["channel"], local(item["from"], tz),
                 local(item["to"], tz), hours(item["to"] - item["from"]),
                 item["points"], item["rawPoints"], item["first"], item["last"],
                 100.0 * item["driftFraction"],
                 "  CONFOUNDED" if item["confounds"] else ""))
        for note in item["confounds"]:
            print("        %s" % note)

    for index, item in enumerate(results, 1):
        print("\n" + "-" * 78)
        print("[%d] %s   %s -> %s   %s"
              % (index, item["channel"], local(item["from"], tz),
                 local(item["to"], tz), hours(item["to"] - item["from"])))
        print("-" * 78)
        print("  %-7s %9s %9s %8s %-22s %9s %9s"
              % ("model", "held-out", "in-samp", "asympt", "tau (h)",
                 "BIC(n)", "BIC(nEff)"))
        for entry in item["ranked"]:
            row = item["fits"][entry["kind"]]
            mark = " <- winner" if row.get("winner") else ""
            print("  %-7s %9.3f %9.3f %8s %-22s %9.1f %9.2f%s"
                  % (entry["kind"], entry["rmse"], row["rmse"],
                     ("%.2f" % row["asymptote"]) if row["asymptote"] is not None
                     else "none",
                     ", ".join("%.2f" % (t / 3600.0) for t in row["taus"])
                     + ("  DEGENERATE" if row["degenerate"] else "")
                     + ("  UNRESOLVED" if row["unresolved"] else "") or "-",
                     row["bic"], row["bicEff"], mark))
        stats = item["fits"][item["winnerKind"]]
        print("  residual lag-1 rho = %.4f, so n = %d counts as n_eff = %.1f."
              " BIC on the raw n is reported but must not be used: it prefers"
              " the most complex model in every segment here."
              % (stats["rho1"], stats["n"], stats["nEff"]))

        if "profileExp" in item and item["profileExp"]:
            interval = item["profileExp"]
            print("\n  Single exponential, asked directly where it ends up:")
            print("    point estimate    %.2f  (tau %.2f h)"
                  % (item["fits"]["exp"]["asymptote"],
                     item["fits"]["exp"]["taus"][0] / 3600.0))
            if interval.get("offScale"):
                print("    95%% profile       none on the %.0f-%.0f scale"
                      " - the fit puts it at %.2f"
                      % (interval["floorLevel"], interval["topLevel"],
                         interval.get("point", float("nan"))))
            else:
                print("    95%% profile       [%s, %s]  width %.1f  %s"
                      % ("%.2f" % interval["low"] if interval["boundedBelow"]
                         else "0 (floor of the scale)",
                         "%.2f" % interval["high"] if interval["boundedAbove"]
                         else "%.2f (the highest reading)" % interval["high"],
                         interval["width"],
                         "BOUNDED" if interval["bounded"] else "UNBOUNDED"))
            boot = item.get("bootstrapExp")
            if boot:
                print("    block bootstrap   [%.2f, %.2f]  width %.2f"
                      " - conditional on the model, see below"
                      % (boot["low"], boot["high"], boot["high"] - boot["low"]))
            spread = item.get("startSpreadExp")
            if spread is not None:
                rows = item["startExp"]["rows"]
                print("    trim the start    %s"
                      % " -> ".join("%.1f" % r["asymptote"] for r in rows))
                print("                      moves it %.1f points over %.1f h"
                      " of trimming" % (spread, rows[-1]["trimHours"]))
            if boot and interval["width"] > 0:
                ratio = (boot["high"] - boot["low"]) / interval["width"]
                loose = (not interval["bounded"]
                         or interval["width"] > ASYMPTOTE_MAX_WIDTH)
                if ratio < 0.25 and loose:
                    # The guard on interval["width"] protects the division that
                    # makes `ratio`; this one protects the division that
                    # inverts it. Every refit collapsing onto one level is a
                    # zero-width bootstrap, and that is the MOST misleading
                    # case, not a case to crash on.
                    factor = ("%.0fx" % (1.0 / ratio) if ratio > 0.0
                              else "immeasurably")
                    print("    The bootstrap is %s tighter than the profile,"
                          " which is unbounded. That gap is the model being"
                          " wrong, not the noise being small." % factor)
            # Printed whichever model won, because "uncertain" and "already
            # refuted" are different verdicts and only one of them is fatal.
            hit = item.get("falsifiedExp")
            if hit:
                print("    FALSIFIED       this probe read %.2f at %s - %.1f h"
                      " later, no watering in between - which is %.2f points"
                      " BELOW that asymptote."
                      % (hit["value"], local(hit["at"], tz), hit["hoursAfter"],
                         hit["below"]))

        if item["refusedBy"]:
            print("\n  NO DRY ANCHOR - refused by %s:" % item["refusedBy"])
            print("    %s" % item["refusal"])
        else:
            print("\n  DRY ANCHOR %.2f  (95%% [%.2f, %.2f])"
                  % (item["dryAnchor"], item["profile"]["low"],
                     item["profile"]["high"]))

    print("\n" + "=" * 78)
    accepted = [r for r in results if not r["refusedBy"]]
    print("VERDICT")
    print("=" * 78)
    # Only non-degenerate exp+linear fits are summarised. A degenerate one has
    # let its exponential swallow the trend, so its "fast tau" is the window
    # length and its "slow slope" is whatever is left - reporting those beside
    # the real ones is how a range of -173 to +0.7 points/day gets printed as
    # if it meant something. It did, on the first run of this script.
    usable = [r for r in results
              if "explin" in r["fits"] and r["fits"]["explin"]["taus"]
              and not r["fits"]["explin"]["degenerate"]]
    degenerate = len(results) - len(usable)
    fast = [r["fits"]["explin"]["taus"][0] / 3600.0 for r in usable]
    slow = [r["fits"]["explin"]["coef"][2] * 24.0 for r in usable]
    print("  Segments: %d, of which %d are CONFOUNDED (they begin within %.0f h"
          " of a handling episode or a data gap)."
          % (len(results), sum(1 for r in results if r["confounds"]),
             CONFOUND_SEC / 3600.0))
    if fast:
        print("  Fast component, from the %d non-degenerate exp+linear fit(s):"
              " tau %.2f - %.2f h." % (len(fast), min(fast), max(fast)))
        print("  Slow limb over the same fits: %.2f to %.2f points/day."
              % (min(slow), max(slow)))
    else:
        print("  No segment produced a non-degenerate exp+linear fit.")
    if degenerate:
        print("  %d segment(s) had their exp+linear fit swallow the trend"
              " (tau past the window) and are excluded from those two ranges."
              % degenerate)
    print("  CLAUDE.md records -0.323 points/day over 29.7 days as a LINEAR"
          " trend explaining 86.4 %. That is a net rate across a period"
          " containing waterings, each of which puts the level back up, on an"
          " earlier probe generation; the rates above are drying-only, between"
          " waterings. Both can be true, and the archive cannot check the"
          " older one - it starts on 2026-08-24.")
    kinds = {}
    for item in results:
        kinds[item["winnerKind"]] = kinds.get(item["winnerKind"], 0) + 1
    unresolved = sum(1 for r in results
                     if r["fits"].get(r["winnerKind"], {}).get("unresolved"))
    if unresolved:
        print("  %d winning fit(s) rely on a time constant shorter than two"
              " sample periods, which the archive cannot resolve." % unresolved)
    print("  Held-out winner by segment: %s"
          % ", ".join("%s x%d" % (k, v) for k, v in sorted(kinds.items())))
    if accepted:
        print("  Usable dry anchor(s): %s"
              % ", ".join("%s = %.2f" % (r["channel"], r["dryAnchor"])
                          for r in accepted))
    else:
        print("  NO usable dry anchor. Every segment was refused, and the"
              " refusals are listed above.")
        reasons = {}
        for item in results:
            reasons[item["refusedBy"]] = reasons.get(item["refusedBy"], 0) + 1
        print("  Refusals: %s"
              % ", ".join("%s x%d" % (k, v) for k, v in sorted(reasons.items())))


# ---------------------------------------------------------------------------
# self-test of the segmentation, on series whose answer is known
# ---------------------------------------------------------------------------


def self_test():
    failures = list(models_self_test()) + list(evidence_self_test())

    def check(name, condition, detail=""):
        if not condition:
            failures.append("%s %s" % (name, detail))

    step = 60.0

    # A pure slow decline, twelve hours of it, must survive whole.
    slow = [(i * step, 80.0 - 0.004 * i) for i in range(720)]
    kept, handled, _ = segment(slow)
    check("segment/keeps-clean-decline", len(kept) == 1 and not handled,
          "kept %d, handled %d" % (len(kept), len(handled)))

    # A probe carried between pot, air and water: many steps in minutes. The
    # whole episode plus its settle window must be excluded, and with only an
    # hour of clean data before it nothing is left to fit.
    handled_series = ([(i * step, 75.0) for i in range(60)]
                      + [(3600.0 + i * step, v) for i, v in
                         enumerate([40.0, 99.0, 99.0, 60.0, 55.0, 95.0])]
                      + [(4000.0 + i * step, 90.0 - 0.01 * i) for i in range(400)])
    kept, handled, _ = segment(handled_series)
    check("segment/flags-handling", len(handled) == 1,
          "handled %d" % len(handled))
    check("segment/drops-handled-data", kept == [],
          "kept %d" % len(kept))

    # A watering in the middle: one long fall, a 14-point rise, another long
    # fall. Two segments, split at the PEAK and not at the foot of the rise.
    watered = ([(i * step, 80.0 - 0.002 * i) for i in range(600)]
               + [(600 * step + i * step, 78.8 + 3.0 * i) for i in range(5)]
               + [(605 * step + i * step, 93.8 - 0.004 * i) for i in range(600)])
    kept, _, _ = segment(watered)
    check("segment/splits-at-watering", len(kept) == 2,
          "kept %d" % len(kept))
    if len(kept) == 2:
        check("segment/first-ends-at-the-foot",
              abs(kept[0]["samples"][-1][1] - 78.8) < 0.2,
              "ends at %.2f" % kept[0]["samples"][-1][1])
        check("segment/second-starts-at-peak",
              abs(kept[1]["samples"][0][1] - 93.8) < 0.2,
              "starts at %.2f" % kept[1]["samples"][0][1])

    # The diurnal wobble must NOT split anything: 0.6 points over three hours.
    wobble = [(i * step, 76.5 + 0.3 * math.sin(i / 180.0) - 0.003 * i)
              for i in range(900)]
    kept, _, _ = segment(wobble)
    check("segment/ignores-diurnal-wobble", len(kept) == 1,
          "kept %d" % len(kept))

    # A constant series is not drying and must not be offered as a segment.
    flat = [(i * step, 0.0) for i in range(900)]
    kept, _, _ = segment(flat)
    check("segment/drops-flat", kept == [], "kept %d" % len(kept))

    # The falsification check has to fire on the case it exists for.
    channel = [(i * step, 80.0 - 0.001 * i) for i in range(2000)]
    hit = falsified_by_later(79.0, channel, channel[:900])
    check("falsify/fires", hit is not None and hit["value"] < 79.0,
          "%s" % (hit,))
    check("falsify/quiet-when-true",
          falsified_by_later(70.0, channel, channel[:900]) is None)

    return failures


# ---------------------------------------------------------------------------


def parse_when(text, tz_hours):
    import datetime as dt
    zone = dt.timezone(dt.timedelta(hours=tz_hours))
    for shape in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
        try:
            return int(dt.datetime.strptime(text, shape)
                       .replace(tzinfo=zone).timestamp() * 1000)
        except ValueError:
            continue
    raise SystemExit("cannot read a date from %r" % text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--db", default=str(DEFAULT_DB))
    parser.add_argument("--since")
    parser.add_argument("--until")
    parser.add_argument("--tz", type=int, default=DEFAULT_TZ_HOURS)
    parser.add_argument("--step", type=float, default=RESAMPLE_SEC,
                        help="resample period in seconds (default 300, the"
                             " device's current publish period)")
    parser.add_argument("--stitch-seam", action="store_true",
                        help="rejoin moisture3 and moisture2 across the"
                             " 2026-09-02 index seam into one channel")
    parser.add_argument("--settle-hours", type=float,
                        default=HANDLING_SETTLE_SEC / 3600.0,
                        help="hours to discard after a handling episode"
                             " (default 2; try 0 to see what those hours are"
                             " carrying)")
    parser.add_argument("--no-bootstrap", action="store_true")
    parser.add_argument("--json", help="write the whole analysis as JSON")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        problems = self_test()
        for line in problems:
            print("FAIL " + line)
        print("drying_fit self-test: %s" % ("FAILED" if problems else "ok"))
        return 1 if problems else 0

    settle_sec = args.settle_hours * 3600.0

    path = Path(args.db)
    if not path.is_file():
        raise SystemExit("no archive at %s - run scripts/tb_export.py first"
                         % path)
    since = parse_when(args.since, args.tz) if args.since else None
    until = parse_when(args.until, args.tz) if args.until else None

    connection = sqlite3.connect("file:%s?mode=ro" % path.as_posix(), uri=True)
    try:
        cursor = connection.cursor()
        found, joint = channels(cursor, since, until, args.stitch_seam)
    finally:
        connection.close()

    results = []
    handled_by_channel = {}
    for name, samples in sorted(found.items()):
        blocks, handled, _ = segment(samples, settle_sec)
        handled_by_channel[name] = handled
        for block in blocks:
            item = analyse(name, block, samples, args.step,
                           do_bootstrap=not args.no_bootstrap)
            if item is not None:
                results.append(item)
    results.sort(key=lambda item: item["from"])

    print_report(results, args.tz, joint, handled_by_channel)

    if args.json:
        payload = []
        for item in results:
            trimmed = {k: v for k, v in item.items()
                       if k not in ("fits", "ranked", "start", "startExp")}
            trimmed["fits"] = {
                kind: {k: v for k, v in row.items() if k != "model"}
                for kind, row in item["fits"].items()}
            for key in ("profile", "profileExp"):
                if trimmed.get(key):
                    trimmed[key] = {k: v for k, v in trimmed[key].items()
                                    if k != "curve"}
            payload.append(trimmed)
        Path(args.json).write_text(json.dumps(payload, indent=2),
                                   encoding="utf-8")
        print("\nwrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
