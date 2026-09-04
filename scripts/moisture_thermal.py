#!/usr/bin/env python3
"""Does ambient temperature bias the soil-moisture reading, and can this archive say?

WHY THE QUESTION IS REAL

The probes on this garden are RESISTIVE (`kind: "resistive"`, confirmed against
the pot on 2026-09-03). Soil conduction is ionic, and ionic conductivity rises
roughly 2 % per K, so the same water content reads differently warm and cold.
Working the divider through with the measured transfer curve in CLAUDE.md - wet
soil at ADC 1002 and dry soil at 1920, under `shown = 100 - ADC%` with
`invert: true` - a 2 %/K change in soil resistance moves the SHOWN value by
+0.37 points/K at the wet end and +0.50 at the dry end. Warmer soil conducts
better and therefore reads WETTER, and that sign is a falsifiable prediction
rather than a fitted parameter. It is the one this module tests against.

WHY IT IS HARD, WHICH IS THE ACTUAL FINDING

Overnight - the natural experiment, because soil water moves slowly and
monotonically while air temperature swings several degrees - air temperature is
almost a straight function of elapsed time. Measured over the ten nights in
backups/telemetry.sqlite, r(T, t) runs -0.81 to -0.97, i.e. VIF 2.9 to 17.7. A
monotone falling temperature and a monotone falling drying trend are the SAME
regressor to within a few per cent of variance, so the coefficient that comes
out is whichever one the trend model declines to absorb. Measured on the
longest undisturbed in-soil segment (moisture1, 61 h, 2026-09-01..09-03), the
partial slope is +0.120 / +0.047 / -0.195 points/K for a linear / quadratic /
cubic trend. The DATA did not change. The sign did.

So this module is built to REFUSE, in the shape moisture_fit.py and
drying_fit.py already use: it names the check that refused, and it never emits a
correction it cannot defend. Four checks, cheapest first:

  1. COLLINEAR    r^2 of the covariate on the trend basis is at or above
                  COLLINEARITY_MAX_R2. The design is degenerate; nothing that
                  comes out of it is a measurement of the covariate.
  2. UNSTABLE     the sign of the slope flips across trend orders 1..3. A
                  coefficient set by the analyst's choice of trend is not a
                  property of the garden.
  3. PLACEBO      the same window, regressed against ANOTHER DAY'S temperature
                  at the same clock time, explains as much. This is the decisive
                  one and it is cheap: any smooth diurnal curve fits a smooth
                  drying residual, so if a temperature that was never in this
                  room does as well, what was measured is the shape of a day.
  4. INCONSISTENT probes in the same garden over the same window disagree in
                  sign. One air mass cannot push two pots opposite ways.

The lag scan is REPORTED and deliberately NOT gated on. A real soil-thermal
effect should peak at a positive lag, because soil temperature is a damped and
delayed version of air temperature, and moisture3 does peak at 4 h (r^2 0.04 ->
0.38 -> 0.11). But maximising over nine lags is nine chances to find one, and
moisture1 over the overlapping window peaks at lag 0 with the opposite sign. A
statistic that is allowed to pick its own lag has already stopped being a test.

WHAT IS EXPORTED FOR THE TRAINING TOOLS, AND WHY IT IS A GATE AND NOT A FIX

Nothing here corrects a reading. Two admission checks come out of it instead:

  anchor_thermal_refusal()  a two-point dry/wet pair whose two anchors were
                            measured in different thermal regimes has part of
                            its span made of temperature rather than water.
  class_thermal_refusal()   the same for the classifier: if the DRY samples come
                            from cold hours and the WET samples from hot ones,
                            Fisher's J is separating clocks.

Both are scored against WORST_CASE_SENSITIVITY, an UPPER BOUND this archive can
defend rather than an estimate it cannot.

Standard library only, and free of any device, file or clock, so `--self-test`
reaches all of it. The least-squares solver is drying_models.solve_normal(),
reused rather than rewritten.
"""

from __future__ import annotations

import datetime as dt
import math
import statistics

from drying_models import solve_normal

# ---------------------------------------------------------------------------
# Thresholds. Every one is a judgement, so every one says what it was set
# against. Measurements are from backups/telemetry.sqlite, 2026-08-24..09-03.
# ---------------------------------------------------------------------------

# The textbook multicollinearity rule of thumb, VIF >= 10. It is BORROWED
# rather than fitted, on purpose: a threshold reverse-engineered from this
# archive would be a threshold chosen to produce this archive's answer. Ten
# nights here sit at r^2 0.66..0.94, so the line falls inside the data and is
# not a formality.
COLLINEARITY_MAX_R2 = 0.90

# Trend orders the slope has to keep its sign across. Order 3 is where a
# 12-hour window stops being able to tell a cubic from a diurnal cycle, so
# going further would refuse everything by construction.
TREND_ORDERS = (1, 2, 3)

# How many whole-day shifts to try for the placebo null, and how many have to
# land before the null is worth quoting. Four is the fewest that gives a
# one-sided rank test any resolution at all (p >= 1/5), and it is quoted as a
# rank rather than a p-value for exactly that reason.
PLACEBO_MAX_DAYS = 9
PLACEBO_MIN_COUNT = 4

# A placebo pool where the real covariate is not clearly best is a refusal. Half
# is deliberately lax: the point is to catch "the shape of a day fits", not to
# demand significance from four samples.
PLACEBO_MAX_BEATEN_FRACTION = 0.5

# A window with fewer samples, or a covariate that moved less than this, cannot
# support a slope. 1 K is four times the DHT11's quantisation and below every
# night in this archive except 2026-08-30 (0.88 K), which is the one it excludes.
MIN_SAMPLES = 60
MIN_COVARIATE_RANGE = 1.0

# The lag scan, reported only. Soil at probe depth lags air by hours; eight is
# past the point where a 12-hour window can still tell a lag from a phase.
LAG_STEP_SEC = 3600.0
LAG_MAX_SEC = 8 * 3600.0

# The largest |slope| any window in this archive supports, in points per K of
# DHT temperature. This is an UPPER BOUND and not an estimate: no in-soil
# segment exceeds it, the 2026-09-03 sun patch bounds the module's own
# electronics at 0.03 (16.67 K through the DHT in two hours moved moisture1 by
# 1.49 points and moisture2 by 0.42, with partial slopes -0.027 and -0.008),
# and the only larger number in the record - 0.206 at r^2 0.83 on moisture1
# over 2026-08-26..27 - comes from a window CLAUDE.md records the probes as
# UNPLUGGED in, where a floating ADC pin's leakage current is what tracks
# temperature. The gates below multiply by this, so a wrong value here makes
# them stricter or laxer, never wronger in kind.
WORST_CASE_SENSITIVITY = 0.2

# How much of a fitted span, or of a class separation, may be thermal before the
# parameter is refused. A tenth is arithmetic about what it buys: at the bound
# above it takes a 10 K regime gap on the 20.4-point span CLAUDE.md's two-point
# calibration measured, where the shipped anchors are 0.69 K apart. So the gate
# is inert on today's numbers and fires on a dry anchor read at a 45 C
# afternoon against a wet one read at a 25 C dawn, which is the mistake it
# exists to catch.
MAX_THERMAL_SHARE = 0.10


# ---------------------------------------------------------------------------
# small numerics
# ---------------------------------------------------------------------------


def correlation(xs, ys):
    count = len(xs)
    if count < 3:
        return 0.0
    mean_x = sum(xs) / count
    mean_y = sum(ys) / count
    sxx = sum((x - mean_x) ** 2 for x in xs)
    syy = sum((y - mean_y) ** 2 for y in ys)
    if sxx <= 0.0 or syy <= 0.0:
        return 0.0
    return sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / math.sqrt(
        sxx * syy
    )


def detrend(times, values, order):
    """Residuals of `values` after removing a polynomial of `order` in time.

    Time is carried in hours from the window's own start. Raw epochs would put
    1.79e9 into a cubic design matrix and the normal equations would lose every
    digit that mattered - the one place where this file's reuse of
    solve_normal() needs care, since its docstring's "well-scaled columns"
    promise is about the caller, not the solver.
    """
    rows = [[t ** power for power in range(order + 1)] for t in times]
    coefficients = solve_normal(rows, list(values))
    if coefficients is None:
        return None
    return [
        value - sum(c * row[i] for i, c in enumerate(coefficients))
        for value, row in zip(values, rows)
    ]


def interpolate(series, stamp, max_gap=900.0):
    """Linear interpolation into an irregular series, refusing across a gap.

    The archive is 60 s until 2026-09-03 18:43 and 300 s after, plus reboots. A
    nearest-neighbour join would silently carry a value across an outage; this
    returns None instead, and the caller drops the point.
    """
    if not series or stamp < series[0][0] or stamp > series[-1][0]:
        return None
    low, high = 0, len(series) - 1
    while low < high - 1:
        middle = (low + high) // 2
        if series[middle][0] <= stamp:
            low = middle
        else:
            high = middle
    before, after = series[low], series[high]
    # An exact hit is an OBSERVATION, not an interpolation, so it is returned
    # even when the next sample is an outage away. Without this a point sitting
    # on the near edge of a gap is discarded, which throws away the last real
    # reading before every reboot in the archive.
    if before[0] == stamp:
        return before[1]
    if after[0] == stamp:
        return after[1]
    if after[0] - before[0] > max_gap:
        return None
    ratio = (stamp - before[0]) / (after[0] - before[0])
    return before[1] + (after[1] - before[1]) * ratio


def align(samples, series, offset=0.0):
    """`samples` paired with `series` at the same instant, shifted by `offset`.

    Points the covariate cannot cover are DROPPED rather than filled. A filled
    point is an invented observation, and it would be invented exactly where the
    record is worst.
    """
    out = []
    for stamp, value in samples:
        other = interpolate(series, stamp + offset)
        if other is not None:
            out.append((stamp, value, other))
    return out


def night_windows(samples, tz_hours, start_hour=20, end_hour=6):
    """The 20:00 -> 06:00 local windows this series covers.

    Overnight is the natural experiment: soil water moves slowly and
    monotonically while air temperature swings several degrees, so a reading
    that tracks temperature beyond its own drying trend has been caught doing
    it. That it turns out to be DEGENERATE on this archive - see the module
    docstring - is the finding, and it is a property of these nights rather than
    of the idea, so the splitter stays.
    """
    if not samples:
        return []
    zone = dt.timezone(dt.timedelta(hours=tz_hours))
    first = dt.datetime.fromtimestamp(samples[0][0], zone).date()
    last = dt.datetime.fromtimestamp(samples[-1][0], zone).date()
    out = []
    day = first - dt.timedelta(days=1)
    while day <= last:
        start = dt.datetime.combine(day, dt.time(start_hour), zone).timestamp()
        end = (
            dt.datetime.combine(day, dt.time(end_hour), zone) + dt.timedelta(days=1)
        ).timestamp()
        out.append((day.isoformat(), start, end))
        day += dt.timedelta(days=1)
    return out


# ---------------------------------------------------------------------------
# The response of one window to one covariate
# ---------------------------------------------------------------------------


def partial_response(triples, order):
    """Slope and partial r^2 of value on covariate, both detrended in time.

    Frisch-Waugh: regressing both sides on the trend and then each other gives
    the same coefficient as the joint fit, and it also yields the partial r^2
    and the collinearity in the same two residual vectors. Returns None when
    the design is rank-deficient rather than a zero, because zero is a number
    somebody will read as "no effect".
    """
    if len(triples) < 3:
        return None
    origin = triples[0][0]
    times = [(stamp - origin) / 3600.0 for stamp, _, _ in triples]
    values = [value for _, value, _ in triples]
    covariate = [other for _, _, other in triples]

    residual_value = detrend(times, values, order)
    residual_cov = detrend(times, covariate, order)
    if residual_value is None or residual_cov is None:
        return None

    count = len(residual_cov)
    mean_cov = sum(residual_cov) / count
    mean_value = sum(residual_value) / count
    sxx = sum((x - mean_cov) ** 2 for x in residual_cov)
    if sxx <= 0.0:
        return None
    slope = (
        sum(
            (x - mean_cov) * (y - mean_value)
            for x, y in zip(residual_cov, residual_value)
        )
        / sxx
    )
    total = statistics.pvariance(covariate) * count
    return {
        "order": order,
        "slope": slope,
        "partialR2": correlation(residual_value, residual_cov) ** 2,
        # How much of the covariate the trend already accounted for. This IS
        # the collinearity, and it is free here.
        "collinearR2": 1.0 - sxx / total if total > 0.0 else 1.0,
        "samples": count,
        "covariateRange": max(covariate) - min(covariate),
    }


def lag_scan(samples, series, order, step=LAG_STEP_SEC, limit=LAG_MAX_SEC):
    """The response at each whole-hour lag. Evidence, never a verdict.

    Reported because a genuine soil-thermal effect has a signature - a peak at a
    positive lag, since soil temperature is a damped and delayed air
    temperature - and because that signature is what the archive was checked
    against. NOT gated on, because taking the best of nine lags is nine chances
    at a false peak, and because the two probes here disagree about where it is.
    """
    scan = []
    lag = 0.0
    while lag <= limit:
        response = partial_response(align(samples, series, -lag), order)
        if response is not None:
            response["lagSec"] = lag
            scan.append(response)
        lag += step
    return scan


def placebo_null(samples, series, order, max_days=PLACEBO_MAX_DAYS):
    """The same window against another day's covariate, at the same clock time.

    A whole-day shift keeps the diurnal SHAPE and destroys the identity, which
    is exactly the null wanted: if the shape alone explains the residual, the
    real covariate has shown nothing. Shifts are in whole days for that reason -
    a six-hour shift would test a different question, since it also moves the
    covariate's phase against the trend.
    """
    null = []
    for days in range(-max_days, max_days + 1):
        if days == 0:
            continue
        triples = align(samples, series, days * 86400.0)
        if len(triples) < max(MIN_SAMPLES, 0.7 * len(samples)):
            continue
        covariate = [other for _, _, other in triples]
        if max(covariate) - min(covariate) < MIN_COVARIATE_RANGE:
            continue
        response = partial_response(triples, order)
        if response is not None:
            response["shiftDays"] = days
            null.append(response)
    return null


def thermal_response(samples, series, orders=TREND_ORDERS):
    """Everything one window says about one covariate, with no verdict attached."""
    triples = align(samples, series)
    result = {
        "samples": len(triples),
        "responses": [],
        "placebos": [],
        "lagScan": [],
        "covariateRange": 0.0,
    }
    if len(triples) < MIN_SAMPLES:
        return result
    covariate = [other for _, _, other in triples]
    result["covariateRange"] = max(covariate) - min(covariate)
    result["responses"] = [
        response
        for response in (partial_response(triples, order) for order in orders)
        if response is not None
    ]
    if result["responses"]:
        primary = result["responses"][0]["order"]
        result["placebos"] = placebo_null(samples, series, primary)
        result["lagScan"] = lag_scan(samples, series, max(orders))
    return result


# ---------------------------------------------------------------------------
# The verdict - which check refused, in the order they are applied
# ---------------------------------------------------------------------------


def thermal_verdict(result, peers=()):
    """None when a coefficient survives every check, else the check that refused.

    `peers` are the same-window results for the OTHER probes in this garden.
    They enter last because the check is the most expensive to arrange and the
    easiest to misread: two probes may legitimately differ in MAGNITUDE, since
    each has its own gain, but one air mass cannot push two pots opposite ways.

    `result["disturbed"]` is checked FIRST and is set by the caller, because the
    detectors that fill it - find_steps() and find_drift_segments() - live in
    moisture_stats.py, which imports this module. Passing the verdict in rather
    than importing back is what keeps that from being a cycle, and it is also
    the rule this repo already states: one detector, reused, never a third one
    written next to the two that exist. Without it a night containing sixteen
    waterings fits +8.0 pts/K and passes all four statistical checks, which is
    what the first run of this tool did.
    """
    if result.get("disturbed"):
        return result["disturbed"]
    if result["samples"] < MIN_SAMPLES:
        return f"only {result['samples']} paired samples, {MIN_SAMPLES} needed"
    if result["covariateRange"] < MIN_COVARIATE_RANGE:
        return (
            f"the covariate moved {result['covariateRange']:.2f} K across this "
            f"window, under the {MIN_COVARIATE_RANGE:.1f} K a slope needs"
        )
    if not result["responses"]:
        return "the design is rank-deficient: no slope can be estimated"

    worst = max(response["collinearR2"] for response in result["responses"])
    if worst >= COLLINEARITY_MAX_R2:
        return (
            f"collinear: the trend already explains {worst:.3f} of the covariate "
            f"(VIF {1.0 / max(1e-9, 1.0 - worst):.1f}), so what comes out is "
            "whatever the trend model declined to absorb, not a measurement"
        )

    signs = {1 if response["slope"] > 0 else -1 for response in result["responses"]}
    if len(signs) > 1:
        detail = ", ".join(
            f"order {response['order']} {response['slope']:+.3f}"
            for response in result["responses"]
        )
        return f"unstable: the sign flips with the trend order ({detail})"

    placebos = result["placebos"]
    if len(placebos) < PLACEBO_MIN_COUNT:
        return (
            f"only {len(placebos)} placebo days are available, "
            f"{PLACEBO_MIN_COUNT} needed: the archive cannot supply its own null"
        )
    primary = result["responses"][0]
    beaten = sum(
        1 for placebo in placebos if placebo["partialR2"] >= primary["partialR2"]
    )
    if beaten > PLACEBO_MAX_BEATEN_FRACTION * len(placebos):
        return (
            f"placebo: another day's covariate explains as much on {beaten} of "
            f"{len(placebos)} shifts, so what was fitted is the shape of a day"
        )

    for peer in peers:
        if not peer.get("responses"):
            continue
        peer_slope = peer["responses"][0]["slope"]
        if peer_slope * primary["slope"] < 0.0:
            return (
                f"inconsistent: another probe over the same window slopes "
                f"{peer_slope:+.3f} against this one's {primary['slope']:+.3f}, "
                "and one air mass cannot push two pots opposite ways"
            )
    return None


def predicted_sign(invert):
    """+1 when warmer soil should read WETTER on this probe, -1 when drier.

    Ionic conduction rises with temperature, so warm soil looks like low
    resistance. Which END of the scale that lands on is the polarity question
    `moisture[i].invert` already answers, and it is answered ONCE here rather
    than assumed - the same rule the two-point calibration and the ordering gate
    are held to.

    This is REPORTED and never gated on. Refusing a coefficient for disagreeing
    with the hypothesis it was fitted to test is how a hypothesis gets
    confirmed, which is the mistake drying_models.py's docstring exists to
    prevent. A slope with the wrong sign is evidence, and it is the evidence
    this archive actually produced.
    """
    return 1 if invert else -1


def thermal_summary(result):
    """The one line worth printing whatever the verdict was."""
    if not result["responses"]:
        return "no slope"
    slopes = [response["slope"] for response in result["responses"]]
    primary = result["responses"][0]
    best_lag = max(result["lagScan"], key=lambda r: r["partialR2"], default=None)
    text = (
        f"slope {primary['slope']:+.3f} pts/K (partial r2 {primary['partialR2']:.3f}, "
        f"collinear r2 {primary['collinearR2']:.3f}); "
        f"across trend orders {min(slopes):+.3f}..{max(slopes):+.3f}"
    )
    if best_lag is not None:
        text += (
            f"; best lag {best_lag['lagSec'] / 3600.0:.0f} h at r2 "
            f"{best_lag['partialR2']:.3f} (reported, not tested)"
        )
    return text


# ---------------------------------------------------------------------------
# The two admission checks the training tools consume
# ---------------------------------------------------------------------------


def regime_mean(series, start, end):
    """Mean covariate over a window, or None when the window is not covered."""
    inside = [value for stamp, value in series if start <= stamp <= end]
    return statistics.fmean(inside) if inside else None


def thermal_share(gap_kelvin, span, sensitivity=WORST_CASE_SENSITIVITY):
    """What fraction of `span` could be temperature rather than water."""
    if span == 0.0:
        return float("inf")
    return abs(gap_kelvin) * sensitivity / abs(span)


def anchor_thermal_refusal(dry, wet, series, sensitivity=WORST_CASE_SENSITIVITY):
    """Why a two-point pair measured in two thermal regimes cannot be trusted.

    `dry` and `wet` are plateau records carrying `from`/`to`/`mean`. Returns
    None when the pair is admissible, else the sentence naming the check.

    This is a GATE and never a correction. Correcting would mean applying a
    coefficient this archive has refused to supply, to every reading including
    the two that define the badge, so a wrong coefficient would move the band
    boundaries systematically rather than merely add noise.
    """
    dry_t = regime_mean(series, dry["from"], dry["to"])
    wet_t = regime_mean(series, wet["from"], wet["to"])
    if dry_t is None or wet_t is None:
        return (
            "no temperature covers one of the two anchors, so the pair cannot "
            "be shown to have been measured in one thermal regime"
        )
    gap = wet_t - dry_t
    span = wet["mean"] - dry["mean"]
    share = thermal_share(gap, span, sensitivity)
    if share > MAX_THERMAL_SHARE:
        return (
            f"thermal regime: the dry anchor was measured at {dry_t:.1f} C and "
            f"the wet one at {wet_t:.1f} C, {abs(gap):.1f} K apart. At the "
            f"{sensitivity:.2f} pts/K upper bound this archive supports, up to "
            f"{share * 100.0:.0f} % of the {abs(span):.1f}-point span is "
            f"temperature and not water, over the {MAX_THERMAL_SHARE * 100.0:.0f} % "
            "a two-point calibration may carry"
        )
    return None


def class_thermal_refusal(labelled, series, stats, sensitivity=WORST_CASE_SENSITIVITY):
    """The same check for the classifier: are DRY and WET separated by the clock?

    The labels come from watering events, and a watering on a schedule happens
    at the same hour every day - so the WET samples can all come from one part
    of the diurnal cycle and the DRY samples from another, and Fisher's J then
    measures a clock. `labelled` is moisture_stats.label_all()'s output.
    """
    temperatures = {"dry": [], "wet": []}
    for stamp, _, name, _ in labelled:
        if name not in temperatures:
            continue
        value = interpolate(series, stamp)
        if value is not None:
            temperatures[name].append(value)
    if len(temperatures["dry"]) < 3 or len(temperatures["wet"]) < 3:
        return (
            "no temperature covers the labelled samples, so the classes cannot "
            "be shown to have been measured in one thermal regime"
        )
    dry_t = statistics.fmean(temperatures["dry"])
    wet_t = statistics.fmean(temperatures["wet"])
    dry_mean = stats["dry"]["sum"] / stats["dry"]["weight"]
    wet_mean = stats["wet"]["sum"] / stats["wet"]["weight"]
    gap = wet_t - dry_t
    span = wet_mean - dry_mean
    share = thermal_share(gap, span, sensitivity)
    if share > MAX_THERMAL_SHARE:
        return (
            f"thermal regime: the dry class averages {dry_t:.1f} C and the wet "
            f"class {wet_t:.1f} C, {abs(gap):.1f} K apart. At the "
            f"{sensitivity:.2f} pts/K upper bound this archive supports, up to "
            f"{share * 100.0:.0f} % of the {abs(span):.1f}-point class "
            "separation is the clock and not the soil"
        )
    return None


# ---------------------------------------------------------------------------
# --self-test: series whose answer is known by construction
# ---------------------------------------------------------------------------


def _diurnal(hours, period_h=24.0, amplitude=5.0, offset=28.0, phase=0.0):
    return offset + amplitude * math.cos(2.0 * math.pi * (hours - phase) / period_h)


def synthetic_weather(days=11, period=300.0):
    """A diurnal temperature whose days DIFFER, which is what a placebo needs.

    A perfectly periodic covariate would reproduce itself exactly under a
    whole-day shift, so every placebo would tie with the real one and the null
    would refuse everything by construction. Real days differ in amplitude and
    mean - measured here, the nightly swing runs 0.88 to 5.76 K - so the
    synthetic ones do too, deterministically.
    """
    covariate = []
    steps = int(days * 86400.0 / period)
    for index in range(steps):
        stamp = index * period
        hours = stamp / 3600.0
        day = int(hours // 24)
        amplitude = 3.0 + 2.0 * ((day * 7) % 5) / 4.0
        offset = 27.0 + ((day * 3) % 4) * 0.8
        covariate.append((stamp, _diurnal(hours, amplitude=amplitude, offset=offset)))
    return covariate


def synthetic_window(days=11, period=300.0, sensitivity=0.0, drift=-2.0, noise=0.02):
    """A probe drying linearly under `synthetic_weather`, with a KNOWN slope.

    Eleven days, so whole-day shifts have somewhere to land and the covariate
    completes eleven cycles against a monotone trend: identified by
    construction, which is precisely what the overnight windows in the real
    archive are not.
    """
    covariate = synthetic_weather(days, period)
    samples = [
        (
            stamp,
            70.0
            + drift * (stamp / 86400.0)
            + sensitivity * (temperature - 28.0)
            + noise * math.sin(index * 1.7),
        )
        for index, (stamp, temperature) in enumerate(covariate)
    ]
    return samples, covariate


def synthetic_collinear(hours=10.0, period=60.0, sensitivity=0.0):
    """One night: a covariate that IS the trend. Nothing may be estimated here.

    The real archive's nights measure r(T, t) = -0.81..-0.97; this is the
    limiting case of that, and the collinearity check has to catch it whether or
    not an effect was injected.
    """
    samples = []
    covariate = []
    steps = int(hours * 3600.0 / period)
    for index in range(steps):
        stamp = index * period
        temperature = 30.0 - 4.0 * stamp / (hours * 3600.0)
        covariate.append((stamp, temperature))
        samples.append(
            (stamp, 70.0 - 1.5 * stamp / 86400.0 + sensitivity * (temperature - 28.0))
        )
    return samples, covariate


def checks():
    """(name, ok, detail) triples, folded into moisture_stats.self_test()."""
    out = []

    def check(name, condition, detail=""):
        out.append((name, bool(condition), detail))

    # An injected coefficient is recovered when the design is identified.
    samples, covariate = synthetic_window(sensitivity=0.35)
    result = thermal_response(samples, covariate)
    primary = result["responses"][0] if result["responses"] else None
    check(
        "an injected +0.35 pts/K is recovered",
        primary is not None and abs(primary["slope"] - 0.35) < 0.02,
        f"{primary['slope']:+.4f}" if primary else "no response",
    )
    check(
        "...and survives every check",
        thermal_verdict(result) is None,
        str(thermal_verdict(result)),
    )
    check(
        "...and is stable across trend orders",
        max(r["slope"] for r in result["responses"])
        - min(r["slope"] for r in result["responses"])
        < 0.02,
    )

    # No effect injected: the estimate must be near zero, not merely refused.
    flat, covariate = synthetic_window(sensitivity=0.0)
    flat_result = thermal_response(flat, covariate)
    check(
        "no injected effect gives a slope near zero",
        abs(flat_result["responses"][0]["slope"]) < 0.02,
        f"{flat_result['responses'][0]['slope']:+.4f}",
    )

    # A collinear night is refused whether or not an effect is present. Both
    # directions matter: refusing only the null one would be a detector that
    # confirms whatever it is shown.
    for injected in (0.0, 0.35):
        night, night_cov = synthetic_collinear(sensitivity=injected)
        verdict = thermal_verdict(thermal_response(night, night_cov))
        check(
            f"a collinear night is refused (injected {injected:+.2f})",
            verdict is not None and verdict.startswith("collinear"),
            str(verdict),
        )

    # The placebo null: a covariate the series never saw must not win. Here the
    # series carries a diurnal cycle of its own with NO dependence on the
    # covariate, which is the real archive's failure mode exactly.
    fake = []
    for stamp, _ in flat:
        hours = stamp / 3600.0
        fake.append((stamp, 70.0 - 2.0 * hours / 24.0 + 1.2 * _diurnal(hours, offset=0.0, amplitude=1.0)))
    shaped = thermal_response(fake, covariate)
    check(
        "a series shaped like a day is refused as placebo or unstable",
        thermal_verdict(shaped) is not None,
        str(thermal_verdict(shaped)),
    )

    # A sign flip across trend orders is refused by name.
    flipping = {
        "samples": 500,
        "covariateRange": 5.0,
        "responses": [
            {"order": 1, "slope": +0.12, "partialR2": 0.03, "collinearR2": 0.4},
            {"order": 3, "slope": -0.19, "partialR2": 0.10, "collinearR2": 0.6},
        ],
        "placebos": [],
        "lagScan": [],
    }
    check(
        "a sign that flips with the trend order is refused",
        (thermal_verdict(flipping) or "").startswith("unstable"),
        str(thermal_verdict(flipping)),
    )

    # Two probes disagreeing in sign over one window is refused.
    peer = {
        "samples": 500,
        "covariateRange": 5.0,
        "responses": [
            {"order": 1, "slope": -0.20, "partialR2": 0.05, "collinearR2": 0.4}
        ],
        "placebos": [],
        "lagScan": [],
    }
    verdict = thermal_verdict(result, peers=[peer])
    check(
        "two probes with opposite signs are refused",
        (verdict or "").startswith("inconsistent"),
        str(verdict),
    )

    # The anchor gate. The shipped calibration's own regimes must pass, or the
    # gate is a veto on the parameter this repo already measured by hand.
    series = [(float(i), 28.0) for i in range(0, 4000)]
    series += [(4000.0 + i, 28.7) for i in range(0, 4000)]
    dry = {"from": 0.0, "to": 3000.0, "mean": 53.1}
    wet = {"from": 4200.0, "to": 7800.0, "mean": 73.5}
    check(
        "0.7 K apart on a 20.4-point span is admissible",
        anchor_thermal_refusal(dry, wet, series) is None,
        str(anchor_thermal_refusal(dry, wet, series)),
    )
    hot = [(float(i), 28.0) for i in range(0, 4000)]
    hot += [(4000.0 + i, 43.0) for i in range(0, 4000)]
    refusal = anchor_thermal_refusal(dry, wet, hot)
    check(
        "15 K apart on the same span is refused",
        refusal is not None and "thermal regime" in refusal,
        str(refusal),
    )
    check(
        "an anchor with no temperature behind it is refused",
        anchor_thermal_refusal(dry, wet, []) is not None,
    )

    # The class gate, on the labelling the classifier actually produces.
    stats = {
        "dry": {"weight": 100.0, "sum": 5300.0, "sumSq": 0.0, "n": 100},
        "wet": {"weight": 100.0, "sum": 7350.0, "sumSq": 0.0, "n": 100},
    }
    labelled = [(float(i), 53.0, "dry", 1.0) for i in range(0, 1000, 10)]
    labelled += [(4000.0 + i, 73.5, "wet", 1.0) for i in range(0, 1000, 10)]
    check(
        "classes in one thermal regime pass",
        class_thermal_refusal(labelled, series, stats) is None,
        str(class_thermal_refusal(labelled, series, stats)),
    )
    check(
        "classes 15 K apart are refused",
        (class_thermal_refusal(labelled, hot, stats) or "").startswith(
            "thermal regime"
        ),
        str(class_thermal_refusal(labelled, hot, stats)),
    )

    # thermal_share is the arithmetic both gates rest on.
    check(
        "a 10 K gap at 0.2 pts/K is 10 % of a 20-point span",
        abs(thermal_share(10.0, 20.0) - 0.1) < 1e-9,
    )
    check(
        "and the sign of the gap does not change the share",
        thermal_share(-10.0, 20.0) == thermal_share(10.0, -20.0),
    )

    # Interpolation must refuse across a gap rather than carry a value over it.
    gapped = [(0.0, 20.0), (60.0, 21.0), (60.0 + 3600.0, 30.0)]
    check("interpolation crosses a 60 s step", interpolate(gapped, 30.0) is not None)
    check(
        "...and refuses a 1 h outage",
        interpolate(gapped, 1800.0) is None,
    )
    check(
        "align drops what the covariate cannot cover",
        len(align([(0.0, 1.0), (1800.0, 1.0), (60.0, 1.0)], gapped)) == 2,
    )

    # The sign the physics predicts, which is reported and never gated on.
    check(
        "an inverted probe should read wetter when warm",
        predicted_sign(True) > 0 and predicted_sign(False) < 0,
    )

    # A disturbed window is refused before any statistic is computed, and the
    # caller's sentence is what comes back - the checks below it never run.
    disturbed = dict(flipping)
    disturbed["disturbed"] = "a watering ran inside this window"
    check(
        "a disturbed window is refused before anything is fitted",
        thermal_verdict(disturbed) == "a watering ran inside this window",
        str(thermal_verdict(disturbed)),
    )

    # The night splitter: a window has to be one night, not two half ones.
    nights = night_windows([(0.0, 1.0), (3.0 * 86400.0, 1.0)], 0)
    check("three days of samples span five 20:00->06:00 windows", len(nights) == 5,
          str(len(nights)))
    check(
        "...each ten hours long",
        all(abs((end - start) - 10 * 3600.0) < 1e-6 for _, start, end in nights),
    )
    check("...and none empty", night_windows([], 0) == [])
    return out


def self_test():
    results = checks()
    for name, ok, detail in results:
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail and not ok else ""))
    failures = [entry for entry in results if not entry[1]]
    print(f"\nthermal self-test: {len(results) - len(failures)}/{len(results)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(self_test())
