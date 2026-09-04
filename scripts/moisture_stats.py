#!/usr/bin/env python3
"""What the moisture archive says, separated from the device that produced it.

The statistics half of scripts/moisture_fit.py, which is the entry point and
imports this. Split when the pair crossed the 1000-line gate CLAUDE.md keeps,
along the seam the repo already uses for telemetry_ui.py + telemetry_page.py
and for dev_server.py + its sim_* siblings: the half that reasons about data,
and the half that talks to a network and a database.

Everything here is a free function over plain lists of `(epoch, value)` pairs.
That is deliberate and is the same rule CLAUDE.md states for the firmware: put
the arithmetic somewhere a test can reach it. `pio test -e native` is a C++
Unity environment and cannot reach Python, so the tests live in `self_test()`
and run as `python scripts/moisture_fit.py --self-test`.
"""

from __future__ import annotations

import datetime as dt
import math
import statistics

# ---------------------------------------------------------------------------
# The firmware's own constants. Changing one here without changing it in
# src/moisture_model.cpp makes this tool answer a question the device is not
# asking.
# ---------------------------------------------------------------------------

WET_WINDOW_SEC = 30 * 60          # g_wetWindowSec
DRY_WINDOW_SEC = 60 * 60          # g_dryWindowSec
TAPER_SEC = 10 * 60               # g_taperSec
ABSORPTION_LAG_SEC = 5 * 60       # g_absorptionLagSec
OUTLIER_Z = 3.0                   # g_outlierZ
VARIANCE_FLOOR = 0.01             # g_gaussianVarianceFloor
MIN_EVENTS = 6                    # g_moistureMinEvents
MIN_WEIGHT_PER_CLASS = 20.0       # g_moistureMinWeightPerClass
MIN_SEPARATION = 4.0              # g_moistureMinSeparation
CLASSES = ("dry", "humid", "wet")

# ---------------------------------------------------------------------------
# This tool's own thresholds. Every one is a judgement, so every one says what
# it was set against.
# ---------------------------------------------------------------------------

# Thirty minutes is long enough that the garden's own drying - measured at
# -0.323 points/day, i.e. 0.007 points per half hour - cannot reach the change
# threshold, and short enough to bracket a probe that was handled: measured on
# 2026-09-03, a re-seated probe fell ~0.45 points per five minutes, which is
# 2.7 across this window.
DRIFT_WINDOW_SEC = 30 * 60

# One point of change across that window. Two orders of magnitude above the
# garden's own drying rate, an order below a handled probe's.
DRIFT_MIN_CHANGE = 1.0

# ...and the change has to dominate the noise, which is what separates
# "drifting" from "noisy around a level". A settled probe measured 2026-09-03
# held sd 0.14-0.32 while moving 0.45 per 5 min: precise, and going somewhere.
DRIFT_MIN_RATIO = 3.0

# Fewer points than this in the window and the line is fitted to nothing.
DRIFT_MIN_POINTS = 6

# A gap longer than this breaks the series: a boot, an outage or a publish-period
# change is not a slope. Six times the 300 s publish period.
MAX_GAP_SEC = 1800

# A jump this large between consecutive published points is a physical event, not
# soil. Soil moved 0.02-0.05 points between consecutive publishes here all night;
# re-seating a probe moved it 17.5.
STEP_MIN_POINTS = 5.0

# A settled plateau has to last this long to anchor anything. Shorter than one
# absorption window would let a probe still taking up water pose as settled.
PLATEAU_MIN_SEC = 30 * 60

# How long after a pump a plateau may start and still be that watering's. The
# firmware calls 30 min wet; the drift detector then smears the edge by up to
# its own 30-minute window, because a window straddling the rise is flagged
# whole. Two hours covers both and still refuses a plateau that only settles
# once the pot has begun to drain.
WET_ANCHOR_MAX_LAG_SEC = 2 * 3600

# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------


def local(epoch: float, tz_hours: int) -> str:
    zone = dt.timezone(dt.timedelta(hours=tz_hours))
    return dt.datetime.fromtimestamp(epoch, zone).strftime("%Y-%m-%d %H:%M:%S")


def linear_fit(points: list[tuple[float, float]]) -> tuple[float, float]:
    """Least-squares slope (per second) and the residual standard deviation."""
    count = len(points)
    if count < 2:
        return 0.0, 0.0
    mean_t = sum(t for t, _ in points) / count
    mean_v = sum(v for _, v in points) / count
    sxx = sum((t - mean_t) ** 2 for t, _ in points)
    if sxx <= 0.0:
        return 0.0, 0.0
    sxy = sum((t - mean_t) * (v - mean_v) for t, v in points)
    slope = sxy / sxx
    intercept = mean_v - slope * mean_t
    if count < 3:
        return slope, 0.0
    residuals = [v - (slope * t + intercept) for t, v in points]
    return slope, statistics.pstdev(residuals)


# ---------------------------------------------------------------------------
# The transient detector - the pure logic, and what --self-test covers
# ---------------------------------------------------------------------------


def find_drift_segments(
    samples,
    window_sec=DRIFT_WINDOW_SEC,
    min_change=DRIFT_MIN_CHANGE,
    min_ratio=DRIFT_MIN_RATIO,
    min_points=DRIFT_MIN_POINTS,
):
    """Runs of samples that are going somewhere rather than sitting still.

    The signature is a SUSTAINED drift with a small residual spread - precise,
    and moving. That is what a probe equilibrating after somebody handled it
    looks like, and it is a case this tool will meet again every time a probe is
    re-seated, so it is detected rather than configured.

    A window qualifies when the change across it clears `min_change` in absolute
    terms AND clears `min_ratio` times the residual spread around the fitted
    line. Both halves are needed: the first alone flags a noisy probe going
    nowhere, the second alone flags the garden's own drying, which is tiny but
    extremely clean.

    A WATERING IS ALSO A TRANSIENT, and is deliberately not exempted. CLAUDE.md
    records the probe-health statistic being changed once for having made a
    watering look like a fault; the answer there was to measure something a
    watering does not do. Here the opposite holds - a probe taking up water is
    not sitting at an anchor value either - so the exclusion applies to ANCHOR
    estimation only, never to the classifier fit, where wet samples are the
    entire point.
    """
    flagged = [False] * len(samples)
    for start in range(len(samples)):
        end = start
        while (
            end + 1 < len(samples)
            and samples[end + 1][0] - samples[end][0] <= MAX_GAP_SEC
            and samples[end + 1][0] - samples[start][0] <= window_sec
        ):
            end += 1
        span = samples[end][0] - samples[start][0]
        if end - start + 1 < min_points or span < window_sec / 2.0:
            continue
        slope, resid_sd = linear_fit(samples[start : end + 1])
        change = abs(slope) * span
        if change >= min_change and change >= min_ratio * max(resid_sd, 1e-6):
            for index in range(start, end + 1):
                flagged[index] = True

    segments = []
    index = 0
    while index < len(samples):
        if not flagged[index]:
            index += 1
            continue
        end = index
        while end + 1 < len(samples) and flagged[end + 1]:
            end += 1
        block = samples[index : end + 1]
        slope, resid_sd = linear_fit(block)
        segments.append(
            {
                "from": block[0][0],
                "to": block[-1][0],
                "samples": len(block),
                "change": block[-1][1] - block[0][1],
                "slopePer5min": slope * 300.0,
                "residualSd": resid_sd,
            }
        )
        index = end + 1
    return segments


def find_steps(samples, min_step=STEP_MIN_POINTS):
    """Jumps between consecutive published points, which soil cannot make.

    Bounded by MAX_GAP_SEC: across a boot or an outage the two samples are not
    consecutive observations of anything, and calling that a step would report
    every reboot as a probe being moved.
    """
    steps = []
    for index in range(1, len(samples)):
        gap = samples[index][0] - samples[index - 1][0]
        if gap > MAX_GAP_SEC:
            continue
        delta = samples[index][1] - samples[index - 1][1]
        if abs(delta) >= min_step:
            steps.append({"at": samples[index][0], "delta": delta, "gapSec": gap})
    return steps


def settled_plateaus(samples, drifts, min_sec=PLATEAU_MIN_SEC):
    """The complement of the drift segments: stretches that sat still long enough."""
    inside = [
        any(seg["from"] <= sample[0] <= seg["to"] for seg in drifts)
        for sample in samples
    ]

    plateaus = []
    index = 0
    while index < len(samples):
        if inside[index]:
            index += 1
            continue
        end = index
        while (
            end + 1 < len(samples)
            and not inside[end + 1]
            and samples[end + 1][0] - samples[end][0] <= MAX_GAP_SEC
        ):
            end += 1
        block = samples[index : end + 1]
        if block[-1][0] - block[0][0] >= min_sec and len(block) >= 3:
            values = [value for _, value in block]
            plateaus.append(
                {
                    "from": block[0][0],
                    "to": block[-1][0],
                    "samples": len(block),
                    "mean": sum(values) / len(values),
                    "sd": statistics.pstdev(values),
                    "min": min(values),
                    "max": max(values),
                }
            )
        index = end + 1
    return plateaus


# ---------------------------------------------------------------------------
# The Gaussian naive Bayes fit - the firmware's, on the workstation's data
# ---------------------------------------------------------------------------


def gaussian_mean(stats):
    return stats["sum"] / stats["weight"] if stats["weight"] > 0.0 else 0.0


def gaussian_variance(stats):
    if stats["weight"] <= 0.0:
        return VARIANCE_FLOOR
    mean = gaussian_mean(stats)
    variance = stats["sumSq"] / stats["weight"] - mean * mean
    return variance if variance > VARIANCE_FLOOR else VARIANCE_FLOOR


def label_for(timestamp, previous, following):
    """src/moisture_model.cpp's labelFor(), with tau unmeasured.

    tau is 0 for every probe on this device - CLAUDE.md's unverified list says
    so and the archive agrees, since no complete watering cycle has ever been in
    the history buffer at training time - so the linear stand-in ramp is what
    the firmware would use here too.
    """
    if previous is not None and timestamp <= previous + WET_WINDOW_SEC:
        since = timestamp - previous
        confidence = (
            1.0 if since >= ABSORPTION_LAG_SEC else since / ABSORPTION_LAG_SEC
        )
        return "wet", confidence
    if following is not None and timestamp + DRY_WINDOW_SEC >= following:
        until = following - timestamp
        return "dry", max(0.0, 1.0 - until / DRY_WINDOW_SEC)
    if previous is not None and following is not None:
        since_wet = timestamp - (previous + WET_WINDOW_SEC)
        until_dry = max(0.0, following - DRY_WINDOW_SEC - timestamp)
        edge = min(since_wet, until_dry)
        return "humid", 1.0 if edge >= TAPER_SEC else edge / TAPER_SEC
    return None, 0.0


def label_all(samples, events):
    labelled = []
    for timestamp, value in samples:
        previous = None
        following = None
        for event in events:
            if event <= timestamp:
                previous = event
            elif following is None:
                following = event
        name, confidence = label_for(timestamp, previous, following)
        if name is None or confidence <= 0.0:
            continue
        labelled.append((timestamp, value, name, confidence))
    return labelled


def accumulate(labelled, reference=None):
    """One pass. With a `reference` fit it rejects samples beyond OUTLIER_Z of it.

    Two passes, because the rejection threshold is a function of the fit it
    protects - the same reason src/moisture_model.cpp cannot do it in one.
    """
    stats = {
        name: {"weight": 0.0, "sum": 0.0, "sumSq": 0.0, "n": 0} for name in CLASSES
    }
    dropped = 0
    for _, value, name, confidence in labelled:
        if reference is not None and reference[name]["weight"] > 0.0:
            spread = math.sqrt(gaussian_variance(reference[name]))
            if abs(value - gaussian_mean(reference[name])) > OUTLIER_Z * spread:
                dropped += 1
                continue
        entry = stats[name]
        entry["weight"] += confidence
        entry["sum"] += value * confidence
        entry["sumSq"] += value * value * confidence
        entry["n"] += 1
    return stats, dropped


def separation(stats):
    dry, wet = stats["dry"], stats["wet"]
    if dry["weight"] <= 0.0 or wet["weight"] <= 0.0:
        return 0.0
    delta = gaussian_mean(wet) - gaussian_mean(dry)
    return delta * delta / (gaussian_variance(wet) + gaussian_variance(dry))


def gate_refusal(stats, events):
    """The first gate that refuses, in the order /moisture.json reports them."""
    if events < MIN_EVENTS:
        return f"only {events} watering events, {MIN_EVENTS} needed"
    for name in CLASSES:
        if stats[name]["weight"] < MIN_WEIGHT_PER_CLASS:
            return (
                f"class {name} carries {stats[name]['weight']:.1f} weight, "
                f"{MIN_WEIGHT_PER_CLASS:.0f} needed"
            )
    # Separation before ordering, because moistureModelIsUsable() tests them in
    # that order and this tool has to name the gate the DEVICE would name. A
    # probe that never dries collapses all three means onto one number and fails
    # both at once; the firmware calls that a separation failure, which is also
    # the more useful of the two answers.
    value = separation(stats)
    if value < MIN_SEPARATION:
        return f"separation J={value:.2f}, {MIN_SEPARATION:.0f} needed"
    means = [gaussian_mean(stats[name]) for name in CLASSES]
    ordered = means[0] < means[1] < means[2] or means[0] > means[1] > means[2]
    if not ordered:
        return (
            "humid does not lie between dry and wet "
            f"({means[0]:.1f} / {means[1]:.1f} / {means[2]:.1f})"
        )
    return None


# ---------------------------------------------------------------------------
# Per-probe analysis, and the admission checks a parameter has to clear
# ---------------------------------------------------------------------------


def analyse_probe(identity, samples, events, tz_hours):
    """Everything the report and the proposal need for one probe."""
    result = {
        "identity": identity,
        "samples": len(samples),
        "twoPoint": {"proposed": None, "blockedBy": None},
        "model": {"blockedBy": None},
    }
    if len(samples) < DRIFT_MIN_POINTS:
        result["twoPoint"]["blockedBy"] = "no usable samples in this probe's window"
        result["model"]["blockedBy"] = "no usable samples in this probe's window"
        return result

    result["from"] = samples[0][0]
    result["to"] = samples[-1][0]

    drifts = find_drift_segments(samples)
    plateaus = settled_plateaus(samples, drifts)
    steps = find_steps(samples)
    result["drifts"] = drifts
    result["plateaus"] = plateaus

    # A step is explained when this probe's own pump ran inside the wet window
    # before it. Anything else means the probe or the pot changed - including a
    # watering can, which this garden does get and the relay record never sees.
    unexplained = []
    for step in steps:
        explained = any(
            0 <= step["at"] - event <= WET_WINDOW_SEC for event in events
        )
        if not explained:
            unexplained.append(step)
    result["steps"] = steps
    result["unexplainedSteps"] = unexplained

    # --- the classifier fit -------------------------------------------------
    labelled = label_all(samples, events)
    first_pass, _ = accumulate(labelled)
    stats, dropped = accumulate(labelled, first_pass)
    result["model"].update(
        {
            "events": len(events),
            "outliersDropped": dropped,
            "separation": separation(stats),
            "classes": {
                name: {
                    "n": stats[name]["n"],
                    "weight": stats[name]["weight"],
                    "mean": gaussian_mean(stats[name]),
                    "sd": math.sqrt(gaussian_variance(stats[name])),
                }
                for name in CLASSES
            },
        }
    )
    result["model"]["blockedBy"] = gate_refusal(stats, len(events))

    # --- the two-point anchors ---------------------------------------------
    result["twoPoint"]["blockedBy"] = anchor_refusal(
        result, samples, plateaus, unexplained, events, tz_hours
    )
    return result


def anchor_refusal(result, samples, plateaus, unexplained, events, tz_hours):
    """Why no dry/wet pair can be proposed, or None with the pair filled in.

    The admission checks, in order:

      - anchors come from SETTLED plateaus, never from a reading taken while the
        probe was still equilibrating;
      - they come from after the last unexplained step, because a step means the
        probe or its pot changed and evidence from before it describes a
        different configuration;
      - the dry anchor is refused when the probe has been drier than any settled
        plateau, since the driest reading then sits inside a transient and is an
        UPPER BOUND on dry rather than dry;
      - the wet anchor has to follow a watering of this probe's own pump, or it
        is a wet day rather than a saturated pot.
    """
    boundary = unexplained[-1]["at"] if unexplained else 0.0
    fresh = [entry for entry in plateaus if entry["from"] >= boundary]
    if not fresh:
        if unexplained:
            return (
                "no settled plateau since the last unexplained step at "
                f"{local(boundary, tz_hours)} "
                f"({unexplained[-1]['delta']:+.1f} points): the probe is still "
                "equilibrating, and a reading taken now is precise and wrong"
            )
        return "no settled plateau of at least 30 min anywhere in the window"

    driest = min(fresh, key=lambda entry: entry["mean"])

    lowest = min(value for timestamp, value in samples if timestamp >= boundary)
    if lowest < driest["mean"] - max(3.0 * driest["sd"], 1.0):
        return (
            f"no settled dry anchor: the probe reached {lowest:.1f} but only "
            f"inside a transient, and the driest SETTLED plateau is "
            f"{driest['mean']:.1f}. {lowest:.1f} is an upper bound on dry, not dry"
        )

    # The wet anchor is the FIRST plateau after a pump, not the highest reading
    # anywhere: the highest reading is a wet day, and only a plateau that formed
    # while the pot was still full is evidence of saturation.
    watered = [
        entry
        for entry in fresh
        if any(
            0 <= entry["from"] - event <= WET_ANCHOR_MAX_LAG_SEC for event in events
        )
    ]
    if not watered:
        return (
            "no wet anchor: no settled plateau follows a watering of this "
            "probe's own pump, so the wettest reading is a wet day and not a "
            "saturated pot"
        )
    wettest = max(watered, key=lambda entry: entry["mean"])

    if abs(wettest["mean"] - driest["mean"]) < 1.0:
        return (
            f"dry {driest['mean']:.1f} and wet {wettest['mean']:.1f} are the "
            "same reading: this probe has no measured span to divide"
        )

    result["twoPoint"]["proposed"] = {
        "dry": round(driest["mean"], 2),
        "wet": round(wettest["mean"], 2),
        "dryFrom": driest["from"],
        "wetFrom": wettest["from"],
        "drySd": driest["sd"],
        "wetSd": wettest["sd"],
    }
    return None


def build_proposal(document, findings):
    """The moisture[] array to POST, or None when nothing was fitted."""
    calibration = [dict(entry) for entry in document.get("moisture", [])]
    changed = False
    for finding in findings:
        proposed = finding["twoPoint"]["proposed"]
        if not proposed:
            continue
        index = finding["identity"]["index"]
        while len(calibration) <= index:
            calibration.append({})
        calibration[index]["dry"] = proposed["dry"]
        calibration[index]["wet"] = proposed["wet"]
        changed = True
    return calibration if changed else None


# ---------------------------------------------------------------------------
# --self-test: the logic pio test -e native cannot reach, being Python
# ---------------------------------------------------------------------------


def synthetic_garden(days=10, period=60.0):
    """Ten days of a garden that does what the labelling rule assumes.

    One watering a day; the probe ramps 45 -> 85 over ten minutes, holds six
    hours, falls back over six and sits at 45 until the next pump. Every slope
    is gentle enough not to register as a step, which is the point: the same
    series has to be a transient to the drift detector and NOT a discontinuity
    to the step detector, and those two thresholds are independent.

    The noise is a sine of the sample index - deterministic, and high-frequency
    enough that no window mistakes it for a drift.
    """
    samples = []
    events = []
    for day in range(days):
        start = day * 86400.0
        watering = start + 43200.0
        events.append(watering)
        index = 0
        while start + index * period < start + 86400.0:
            timestamp = start + index * period
            since = timestamp - watering
            if since < 0.0:
                value = 45.0
            elif since < 600.0:
                value = 45.0 + 40.0 * since / 600.0
            elif since < 600.0 + 6 * 3600.0:
                value = 85.0
            elif since < 600.0 + 12 * 3600.0:
                value = 85.0 - 40.0 * (since - 600.0 - 6 * 3600.0) / (6 * 3600.0)
            else:
                value = 45.0
            samples.append((timestamp, value + 0.05 * math.sin(index)))
            index += 1
    return samples, events


def self_test():
    """The pure logic, against series whose answer is known by construction.

    These live here rather than in test/ because test/ is [env:native], a C++
    Unity environment with no Arduino core and no Python. The rule CLAUDE.md
    states - put the arithmetic somewhere a test can reach it - is kept by
    making the detector a free function over plain lists.
    """
    checks = []

    def check(name, condition, detail=""):
        checks.append((name, bool(condition), detail))

    # A settled probe: noise around a level, going nowhere.
    flat = [(i * 60.0, 73.5 + (0.05 if i % 2 else -0.05)) for i in range(120)]
    check("a settled probe is not a transient", not find_drift_segments(flat))

    # The garden's own drying: -0.323 points/day, extremely clean. Must not fire,
    # or every ordinary night is an equilibration.
    drying = [(i * 60.0, 73.5 - 0.323 * (i * 60.0) / 86400.0) for i in range(1440)]
    check("ordinary drying is not a transient", not find_drift_segments(drying))

    # A handled probe: the measured signature, -0.45 points per 5 min with a
    # residual sd inside 0.14-0.32.
    handled = [
        (i * 60.0, 93.7 - 0.09 * i + (0.2 if i % 3 == 0 else -0.1))
        for i in range(120)
    ]
    segments = find_drift_segments(handled)
    check("a re-seated probe is a transient", len(segments) == 1, str(segments))
    if segments:
        check(
            "and its slope is reported per 5 min",
            abs(segments[0]["slopePer5min"] + 0.45) < 0.05,
            f"{segments[0]['slopePer5min']:.3f}",
        )

    # Noisy but going nowhere: large spread, no drift. The ratio test is what
    # keeps this out, and without it a noisy probe would be called equilibrating.
    noisy = [(i * 60.0, 73.5 + (6.0 if i % 2 else -6.0)) for i in range(120)]
    check("noise alone is not a transient", not find_drift_segments(noisy))

    # A gap does not make a slope: two settled levels either side of an outage.
    split = [(i * 60.0, 73.5) for i in range(40)]
    split += [(40 * 60.0 + 7200.0 + i * 60.0, 60.0) for i in range(40)]
    check("an outage is not a slope", not find_drift_segments(split))
    check("...and not a step either", not find_steps(split))

    # Steps: soil cannot move 17 points between two publishes.
    stepped = [(i * 60.0, 73.0) for i in range(30)]
    stepped += [(30 * 60.0 + i * 60.0, 90.5) for i in range(30)]
    steps = find_steps(stepped)
    check("a 17-point jump is a step", len(steps) == 1 and steps[0]["delta"] > 17.0)
    check("a 0.05-point move is not", not find_steps(flat))

    # Plateaus are the complement of the drifts, and long enough to anchor.
    plateaus = settled_plateaus(flat, find_drift_segments(flat))
    check(
        "a settled hour is one plateau",
        len(plateaus) == 1 and abs(plateaus[0]["mean"] - 73.5) < 0.01,
        str(plateaus),
    )
    check(
        "a drifting hour is no plateau",
        not settled_plateaus(handled, find_drift_segments(handled)),
    )

    # The labelling and the gates, against the firmware's own windows.
    name, confidence = label_for(1000.0 + ABSORPTION_LAG_SEC, 1000.0, None)
    check("a settled wet sample is full confidence", name == "wet" and confidence == 1.0)
    name, confidence = label_for(1000.0 + 60.0, 1000.0, None)
    check(
        "a sample at the pump's edge is discounted",
        name == "wet" and abs(confidence - 0.2) < 1e-9,
        f"{confidence}",
    )
    name, _ = label_for(5000.0, None, 5000.0 + DRY_WINDOW_SEC - 1)
    check("just before the next watering is dry", name == "dry")
    name, _ = label_for(500.0, None, None)
    check("a reading in no cycle carries no label", name is None)

    separated = {
        "dry": {"weight": 100.0, "sum": 3000.0, "sumSq": 90100.0, "n": 100},
        "humid": {"weight": 100.0, "sum": 5000.0, "sumSq": 250100.0, "n": 100},
        "wet": {"weight": 100.0, "sum": 7000.0, "sumSq": 490100.0, "n": 100},
    }
    check("a well separated fit passes", gate_refusal(separated, MIN_EVENTS) is None)
    check(
        "...and four events do not",
        gate_refusal(separated, 4) is not None,
    )
    collapsed = {
        name: {"weight": 100.0, "sum": 5000.0, "sumSq": 250100.0, "n": 100}
        for name in CLASSES
    }
    check(
        "a probe that never dries is refused on separation",
        "separation" in (gate_refusal(collapsed, MIN_EVENTS) or ""),
        str(gate_refusal(collapsed, MIN_EVENTS)),
    )
    unordered = {
        "dry": {"weight": 100.0, "sum": 3000.0, "sumSq": 90100.0, "n": 100},
        "humid": {"weight": 100.0, "sum": 9000.0, "sumSq": 810100.0, "n": 100},
        "wet": {"weight": 100.0, "sum": 7000.0, "sumSq": 490100.0, "n": 100},
    }
    check(
        "humid outside dry..wet is refused on ordering",
        "humid does not lie" in (gate_refusal(unordered, MIN_EVENTS) or ""),
    )
    inverted = {
        "dry": {"weight": 100.0, "sum": 7000.0, "sumSq": 490100.0, "n": 100},
        "humid": {"weight": 100.0, "sum": 5000.0, "sumSq": 250100.0, "n": 100},
        "wet": {"weight": 100.0, "sum": 3000.0, "sumSq": 90100.0, "n": 100},
    }
    check(
        "inverted polarity is accepted, as the firmware accepts it",
        gate_refusal(inverted, MIN_EVENTS) is None,
    )

    # End to end, on a garden that behaves. A tool that can only refuse has not
    # been shown to be able to accept, and "it refused" would then be evidence
    # about the tool rather than about the archive.
    samples, events = synthetic_garden()
    identity = {
        "index": 0,
        "key": "moisture1",
        "name": "synthetic",
        "pin": 36,
        "relay": 0,
        "invert": True,
        "dry": 0.0,
        "wet": 0.0,
    }
    finding = analyse_probe(identity, samples, events, 0)
    check(
        "a garden with ten clean cycles passes every gate",
        finding["model"]["blockedBy"] is None,
        str(finding["model"]["blockedBy"]),
    )
    check(
        "...and yields a two-point calibration",
        finding["twoPoint"]["proposed"] is not None,
        str(finding["twoPoint"]["blockedBy"]),
    )
    proposed = finding["twoPoint"]["proposed"] or {}
    check(
        "...whose anchors are the two plateaus it was built from",
        abs(proposed.get("dry", 0.0) - 45.0) < 1.0
        and abs(proposed.get("wet", 0.0) - 85.0) < 1.0,
        str(proposed),
    )
    check(
        "...and which reaches the document POST /config.json would carry",
        (build_proposal({"moisture": [{}]}, [finding]) or [{}])[0].get("dry")
        == proposed.get("dry"),
    )

    failures = [entry for entry in checks if not entry[1]]
    for name, ok, detail in checks:
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail and not ok else ""))
    print(f"\nself-test: {len(checks) - len(failures)}/{len(checks)} passed")
    return 1 if failures else 0
