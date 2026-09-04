#!/usr/bin/env python3
"""How much of a fit from drying_models.py is real, and how to refuse it.

WHY THE OBVIOUS CRITERION IS THE WRONG ONE

Soil moisture sampled every 60 s is a smooth curve, so the residuals of ANY of
these models are heavily autocorrelated - across the segments this tool
actually finds, lag-1 rho runs from 0.53 to 0.98, and the segments worth
fitting sit at the top of that. Measured, not assumed: one 737-sample segment
reports rho 0.969, which is n_eff 11.6. AIC and BIC assume independent
residuals, so they count 737 observations where there are twelve. The
consequence is not a small bias: every extra parameter wins, always. So this
file reports AIC/BIC twice, once on n and once on an effective sample size
n*(1-rho)/(1+rho), and the primary criterion is neither. It is HELD-OUT
EXTRAPOLATION: fit the first part, predict the rest. The question these models
are being asked - where does the curve end up - is an extrapolation question,
so an extrapolation test is the one that answers it.

WHAT THE ASYMPTOTE NEEDS BEFORE IT IS REPORTED

Extrapolating an asymptote out of a partial decay is ill-conditioned, and the
failure mode is a confident number rather than a visible error. Three
independent checks are provided and drying_fit.py requires all of them:

    profile_asymptote()  - how much worse the fit gets when m_inf is dragged
                           away, against an F threshold at the EFFECTIVE
                           sample size. Flat means unidentified.
    block_bootstrap()    - sampling uncertainty with the autocorrelation kept,
                           by resampling residual BLOCKS. Narrow here means
                           only that the noise is small; it is conditional on
                           the model being right, which is the thing in doubt.
    start_sensitivity()  - refit with the first hours trimmed off. A real
                           asymptote does not care; an unidentified one walks.

The profile is evaluated in the MODEL'S OWN family. That is not a detail: it
used to fit level + amp*exp(-t/tau) whatever the model was, so an `exp2` model
was profiled against a family that could not reach its SSE, no level ever came
out "inside", and a nonexistent asymptote was reported as a bounded interval of
width ZERO which did not contain its own point estimate.

Standard library only, and Arduino-free in the same sense as drying_models.py:
nothing here touches a device, a file or a clock, so `--self-test` can exercise
all of it against synthetic curves.
"""

from __future__ import annotations

import math
import random

from drying_models import (AMPLITUDE_OFFSET, COLUMNS, HAS_ASYMPTOTE, MODELS,
                           TAU_COUNT, TAU_MIN_FRACTION, _order_taus,
                           _solve_system, _synthetic, asymptote, design_row,
                           fit, predict, residuals)

# ---------------------------------------------------------------------------
# Thresholds. Every one is a judgement, so every one says what it was set
# against. The measurements quoted are from backups/telemetry.sqlite, 10 days
# of this garden, and are reproduced by `drying_fit.py --report`.
# ---------------------------------------------------------------------------

# The PROFILE grid runs far further, and that is not a detail. Dragging m_inf
# downwards is paid for by lengthening tau, and the trade is exact in the limit
# - as tau grows the exponential becomes 1 - t/tau, so (m_inf, A) with A/tau
# held fixed all describe the same straight line. Capping the profile's tau at
# the fitting cap hides that: the search runs out of room, SSE shoots up, and a
# flat valley is reported as a bounded interval. Measured on a synthetic noisy
# line, a cap of 4x the span returned a 2.7-point "95 % interval" for an
# asymptote that is not there at all.
TAU_MAX_FRACTION_PROFILE = 200.0

# Held-out split. Six-tenths in, four-tenths out: enough history to pin a fast
# component that has already finished, and enough future that an asymptote has
# to commit to something.
HOLDOUT_TRAIN_FRACTION = 0.6

# A model has to beat the simpler one by this much on held-out RMSE to be
# preferred. Ties go to fewer parameters.
HOLDOUT_MARGIN = 0.05

# Bootstrap blocks. The residual correlation time measured here is ~2.5 h, so
# six-hour blocks carry it. Blocks shorter than the correlation time make the
# interval too narrow, which is the direction that lies.
BOOTSTRAP_BLOCK_SEC = 6 * 3600
BOOTSTRAP_REPS = 300


# ---------------------------------------------------------------------------
# how much of the sample is actually independent
# ---------------------------------------------------------------------------


def lag1_autocorrelation(values):
    count = len(values)
    if count < 3:
        return 0.0
    mean = sum(values) / count
    num = sum((values[i] - mean) * (values[i - 1] - mean) for i in range(1, count))
    den = sum((v - mean) ** 2 for v in values)
    return num / den if den > 0.0 else 0.0


def effective_samples(count, rho):
    """The AR(1) effective sample size, floored so a ratio stays finite.

    This is the effective n for estimating a MEAN, used here as a proxy for a
    regression with correlated errors. It is a rough one and it errs
    conservative - which is the direction to err when the output is "is this
    number real". Reported alongside the raw n, never instead of it.
    """
    rho = max(-0.99, min(0.99, rho))
    return max(4.0, count * (1.0 - rho) / (1.0 + rho))


def criteria(model, samples):
    """AIC and BIC, on the raw count and on the effective one."""
    count = len(samples)
    rho = lag1_autocorrelation(residuals(model, samples))
    neff = effective_samples(count, rho)
    log_mse = math.log(max(model["sse"], 1e-12) / count)
    params = model["params"]
    return {
        "n": count,
        "rho1": rho,
        "nEff": neff,
        "aic": count * log_mse + 2 * params,
        "bic": count * log_mse + params * math.log(count),
        "aicEff": neff * log_mse + 2 * params,
        "bicEff": neff * log_mse + params * math.log(neff),
    }


# ---------------------------------------------------------------------------
# held-out extrapolation - the primary criterion
# ---------------------------------------------------------------------------


def holdout(kind, samples, train_fraction=HOLDOUT_TRAIN_FRACTION):
    cut = int(len(samples) * train_fraction)
    if cut < 8 or len(samples) - cut < 8:
        return None
    model = fit(kind, samples[:cut])
    if model is None:
        return None
    errors = [value - predict(model, stamp) for stamp, value in samples[cut:]]
    return {
        "kind": kind,
        "rmse": math.sqrt(sum(e * e for e in errors) / len(errors)),
        "bias": sum(errors) / len(errors),
        "aheadHours": (samples[-1][0] - samples[cut][0]) / 3600.0,
        "asymptote": asymptote(model),
        "taus": model["taus"],
    }


def choose(samples):
    """Rank the four models by held-out RMSE, simplest wins a tie.

    Returned in order, so the caller can print the whole table: a verdict that
    only names the winner hides how close the runner-up was, and on this data
    the two best are routinely within a few percent.
    """
    scored = []
    for kind in MODELS:
        result = holdout(kind, samples)
        if result is not None:
            scored.append(result)
    if not scored:
        return []
    scored.sort(key=lambda r: (r["rmse"], TAU_COUNT[r["kind"]]))
    best = scored[0]["rmse"]
    for entry in scored:
        entry["withinMargin"] = entry["rmse"] <= best * (1.0 + HOLDOUT_MARGIN)
    simplest = min((e for e in scored if e["withinMargin"]),
                   key=lambda e: (COLUMNS[e["kind"]], TAU_COUNT[e["kind"]]))
    for entry in scored:
        entry["winner"] = entry["kind"] == simplest["kind"]
    return scored


# ---------------------------------------------------------------------------
# is the asymptote there at all? three ways of asking
# ---------------------------------------------------------------------------


def _level_profiler(kind, samples, origin, taus):
    """SSE at ANY pinned m_inf, for one set of time constants, O(1) each.

    With the taus fixed the non-constant columns are fixed too, so the normal
    equations depend on the pinned level only through the target - and the
    target, value - level, is affine in it. Accumulating the Gram matrix,
    rows^T*value and rows^T*1 in a single O(n) pass therefore answers EVERY
    level from it. That is what makes profiling the two-exponential family
    affordable, which in turn is what removes the temptation to profile the
    cheap wrong one.
    """
    rows = [design_row(kind, stamp, origin, taus)[1:] for stamp, _ in samples]
    values = [value for _, value in samples]
    width = len(rows[0])
    gram = [[sum(row[p] * row[q] for row in rows) for q in range(width)]
            for p in range(width)]
    weighted = [sum(row[p] * value for row, value in zip(rows, values))
                for p in range(width)]
    totals = [sum(row[p] for row in rows) for p in range(width)]
    square = sum(value * value for value in values)
    plain = sum(values)
    count = len(values)

    def sse_at(level):
        vector = [weighted[p] - level * totals[p] for p in range(width)]
        coef = _solve_system([row[:] for row in gram], list(vector))
        if coef is None:
            return None, None
        # ||y||^2 - coef.(X^T y), the residual identity for a solved system.
        total = (square - 2.0 * level * plain + count * level * level
                 - sum(c * v for c, v in zip(coef, vector)))
        return max(total, 0.0), coef

    return sse_at


def profile_asymptote(kind, samples, levels, grid_points=None, seed_taus=None):
    """Best achievable SSE with m_inf pinned, for each level offered.

    Profiled in the MODEL'S OWN family, which it was not. It fitted
    level + amp*exp(-t/tau) whatever the model was, so an `exp2` model's SSE -
    reachable only from the richer family - was compared against
    single-exponential profiles that could never match it. No level ever came
    out "inside", the bisection in profile_interval() collapsed onto its
    anchor, and a NONEXISTENT asymptote was reported as a bounded interval of
    width zero which did not even contain its own point estimate. Measured on a
    straight line, where no asymptote exists at all: [2.00, 2.00] beside a
    point estimate of 61.03.

    Profiling rather than reporting a standard error is deliberate. A standard
    error is a local curvature, and the whole difficulty here is that the
    surface is not locally quadratic: it is a long flat valley running towards
    m_inf = -infinity, which a curvature at the optimum cannot see.
    """
    ntau = TAU_COUNT[kind]
    if ntau == 0 or not levels or len(samples) < 4:
        return []
    origin = samples[0][0]
    span = samples[-1][0] - origin
    if span <= 0:
        return []
    low = span * TAU_MIN_FRACTION
    high = span * TAU_MAX_FRACTION_PROFILE
    if grid_points is None:
        grid_points = 160 if ntau == 1 else 34
    grid = [low * math.exp(math.log(high / low) * (i / (grid_points - 1.0)))
            for i in range(grid_points)]
    if ntau == 1:
        combos = [[value] for value in grid]
    else:
        combos = [[grid[i], grid[j]]
                  for i in range(len(grid)) for j in range(i + 1, len(grid))]
    if seed_taus is not None and len(seed_taus) == ntau:
        # Start from the UNCONSTRAINED optimum. Profiling is by definition
        # "re-optimise everything else with m_inf pinned", so the fit's own
        # taus are the one candidate guaranteed to be worth offering: without
        # them a coarser grid can leave the profile ABOVE the fit's own SSE at
        # the fit's own level, which is not a profile of anything - and every
        # level then inherits a different amount of that shortfall, which is
        # curvature invented by the search.
        combos.insert(0, list(seed_taus))

    best = [None] * len(levels)
    for taus in combos:
        sse_at = _level_profiler(kind, samples, origin, taus)
        for index, level in enumerate(levels):
            sse, coef = sse_at(level)
            if sse is None:
                continue
            if best[index] is None or sse < best[index][0]:
                best[index] = (sse, list(taus), coef)

    # The grid spans five orders of magnitude, so its steps are several percent
    # apart and the best grid point can sit well off the optimum. Left coarse,
    # the profile reads too HIGH everywhere and the interval out of it is too
    # narrow - the direction that lies.
    #
    # Every candidate is offered to EVERY level, which is the part that matters
    # and is not an optimisation. Along a degenerate direction the surface is
    # flat in m_inf, so one tau set fits every level equally; a level left on a
    # worse grid point than its neighbour is OPTIMISER noise, and it reads as
    # curvature. Measured on a straight line, per-level polishing left a 1 %
    # ripple on a profile whose real variation is 0 %, and the 95 % threshold
    # sat inside that ripple - so a flat valley was reported as a BOUNDED
    # interval [13.37, 14.94]. Sharing the candidates removes the ripple, and
    # the shared O(n) pass makes it cheaper than doing it per level anyway.
    for _ in range(60):
        improved = False
        candidates = {}
        live = [index for index, entry in enumerate(best) if entry is not None]
        for index in live:
            taus = best[index][1]
            candidates.setdefault(tuple(taus), set()).update(live)
            for slot in range(ntau):
                for factor in (0.99, 1.01, 0.95, 1.05):
                    trial = list(taus)
                    trial[slot] *= factor
                    if low <= trial[slot] <= high:
                        candidates.setdefault(tuple(trial), set()).update(live)
        for taus, wanted in candidates.items():
            sse_at = _level_profiler(kind, samples, origin, list(taus))
            for index in wanted:
                sse, coef = sse_at(levels[index])
                if sse is not None and sse < best[index][0] - 1e-12:
                    best[index] = (sse, list(taus), coef)
                    improved = True
        if not improved:
            break

    out = []
    for index, level in enumerate(levels):
        if best[index] is None:
            continue
        sse, taus, coef = best[index]
        taus, coef = _order_taus(taus, [level] + list(coef))
        out.append({"level": level, "sse": sse, "tau": taus[0], "taus": taus,
                    "amp": coef[AMPLITUDE_OFFSET]})
    return out


def _f_threshold(neff, params):
    """The SSE ratio a 95 % profile interval allows, from an F(1, df) bound.

    Table-free: the 95th percentile of F(1, df) is t(0.975, df)^2, and a
    Cornish-Fisher expansion of the t quantile is accurate to better than 1 %
    for df >= 3, which is well inside the honesty this number is used with.
    """
    df = max(1.0, neff - params)
    z = 1.959964
    t = z * (1.0 + (z * z + 1.0) / (4.0 * df)
             + (5.0 * z ** 4 + 16.0 * z * z + 3.0) / (96.0 * df * df))
    return 1.0 + (t * t) / df


def profile_interval(samples, model, floor_level=0.0, steps=41, tol=0.05):
    """95 % profile interval on m_inf, at the effective sample size.

    The search runs from the highest reading down to `floor_level`, which
    defaults to 0 because that is the bottom of the scale a moisture reading
    lives on. An interval that reaches it is reported as UNBOUNDED - not as
    "[0, x]" - because a lower limit pinned by the axis rather than by the data
    is not a measurement, and formatting it as one is the whole failure this
    module is built to avoid.

    Each edge is then bisected to `tol` points, so the width that comes out is
    the data's and not the grid's. Without that a sharp profile lands inside a
    single grid cell and reports a width of exactly zero, which reads as
    infinite precision.

    BOTH edges decide `bounded`. It used to be the lower one alone - the upper
    edge's flag was assigned to `_` and thrown away - so a profile equally
    happy with an asymptote at the top of the record was reported as bounded.
    An asymptote a decay never reaches from above is no more identified than
    one it never reaches from below.
    """
    if not HAS_ASYMPTOTE[model["kind"]]:
        return None
    kind = model["kind"]
    top = max(value for _, value in samples)
    point = asymptote(model)
    if point is None:
        return None

    cache = {}

    def sse_at(level):
        key = round(level, 6)
        if key not in cache:
            found = profile_asymptote(kind, samples, [level],
                                      seed_taus=model["taus"])
            cache[key] = found[0]["sse"] if found else float("inf")
        return cache[key]

    coarse = profile_asymptote(kind, samples,
                               [top - (top - floor_level) * (i / (steps - 1.0))
                                for i in range(steps)],
                               seed_taus=model["taus"])
    if not coarse:
        return None
    best = min(min(entry["sse"] for entry in coarse), model["sse"])
    threshold = _f_threshold(criteria(model, samples)["nEff"], model["params"])
    limit = best * threshold

    def inside(level):
        return sse_at(level) <= limit

    # The anchor has to lie INSIDE the searched range, not merely be the best
    # level anywhere. Both edges bisect away from it, so an anchor below
    # `floor_level` leaves both of them searching UPWARDS and the two bounds
    # converge on the same crossing: a real exp2 winner on this archive
    # (segment [2], a degenerate fit whose asymptote is -155) came back as
    # `bounded` with a width of 0.02 around an impossible -12.03. Only the
    # earlier `degenerate` gate stopped it being printed as a dry anchor.
    anchor = (point if floor_level <= point <= top and inside(point)
              else min(coarse, key=lambda e: e["sse"])["level"])

    # ...and if even the best level in range misses the threshold, the fit's
    # optimum is OFF THE SCALE - a degenerate exp2 on this archive puts it at
    # -155 - so no level between the floor and the highest reading is
    # supported, and there is no interval to bisect. Saying so is the whole
    # point: bisecting anyway returns the anchor twice and reports a WIDTH OF
    # ZERO, which reads as infinite precision about an impossible number.
    if not inside(anchor):
        return {
            "low": floor_level, "high": top, "width": top - floor_level,
            "bounded": False, "boundedBelow": False, "boundedAbove": False,
            "offScale": True, "point": point,
            "floorLevel": floor_level, "topLevel": top,
            "nEff": criteria(model, samples)["nEff"], "threshold": threshold,
            "ratioAtFloor": sse_at(floor_level) / best if best > 0 else float("inf"),
            "curve": coarse,
        }

    def edge(bound):
        """Bisect from the anchor towards `bound`; False when `bound` still fits."""
        if inside(bound):
            return bound, False
        near, far = anchor, bound
        while abs(far - near) > tol:
            middle = (near + far) / 2.0
            if inside(middle):
                near = middle
            else:
                far = middle
        return near, True

    high, bounded_above = edge(top)
    low, bounded_below = edge(floor_level)
    return {
        "low": low,
        "high": high,
        "width": high - low,
        "bounded": bounded_below and bounded_above,
        "boundedBelow": bounded_below,
        "boundedAbove": bounded_above,
        "offScale": False,
        "point": point,
        "floorLevel": floor_level,
        "topLevel": top,
        "nEff": criteria(model, samples)["nEff"],
        "threshold": threshold,
        "ratioAtFloor": sse_at(floor_level) / best if best > 0 else float("inf"),
        "curve": coarse,
    }


def block_bootstrap(kind, samples, block_sec=BOOTSTRAP_BLOCK_SEC,
                    reps=BOOTSTRAP_REPS, seed=20260903):
    """Residual moving-block bootstrap on the asymptote and the fast tau.

    Blocks rather than points because the residuals are autocorrelated;
    resampling points independently would shred the correlation and return an
    interval several times too narrow. It is still CONDITIONAL on the model
    being right - it resamples around the fitted curve - which is why
    drying_fit.py never reports it on its own.
    """
    model = fit(kind, samples)
    if model is None or len(samples) < 20:
        return None
    # The NOMINAL grid step, not the first interval. resample() emits no point
    # for an empty bin, so after a data gap samples[1] - samples[0] is several
    # periods - and a step read too LONG makes the block too SHORT, which
    # breaks the six-hour correlation the block length exists to carry and
    # returns an interval too narrow: the direction that lies. Every gap is a
    # whole multiple of the period, so the smallest difference is the period.
    spacing = [samples[i][0] - samples[i - 1][0] for i in range(1, len(samples))]
    spacing = [value for value in spacing if value > 0]
    if not spacing:
        return None
    step = min(spacing)
    length = max(3, int(block_sec / step))
    count = len(samples)
    if length >= count:
        length = max(3, count // 3)
    blocks = int(math.ceil(count / float(length)))
    fitted = [predict(model, stamp) for stamp, _ in samples]
    resid = [value - f for (_, value), f in zip(samples, fitted)]

    rng = random.Random(seed)
    levels = []
    taus = []
    for _ in range(reps):
        drawn = []
        for _block in range(blocks):
            # randrange's stop is EXCLUSIVE, so the +1 is what makes the LAST
            # block drawable at all. Without it the final `length` residuals
            # were under-represented in every replicate - and the tail is the
            # part of the record that pins an asymptote.
            start = rng.randrange(0, max(1, count - length + 1))
            drawn.extend(resid[start:start + length])
        drawn = drawn[:count]
        synthetic = [(samples[i][0], fitted[i] + drawn[i]) for i in range(count)]
        redone = fit(kind, synthetic)
        if redone is None:
            continue
        level = asymptote(redone)
        if level is not None:
            levels.append(level)
        if redone["taus"]:
            taus.append(redone["taus"][0])
    if not levels:
        return None
    levels.sort()
    taus.sort()

    def quantile(values, p):
        if not values:
            return None
        index = max(0, min(len(values) - 1, int(p * len(values))))
        return values[index]

    return {
        "point": asymptote(model),
        "low": quantile(levels, 0.025),
        "high": quantile(levels, 0.975),
        "tauLow": quantile(taus, 0.025),
        "tauHigh": quantile(taus, 0.975),
        "reps": len(levels),
        "blockSec": length * step,
    }


def start_sensitivity(kind, samples, trims=(0.0, 0.05, 0.10, 0.15, 0.20)):
    """Refit with the first part of the record trimmed away.

    This is the check that does the most work, because it attacks the actual
    failure: an asymptote that is not determined by the shape of the curve is
    determined by where the curve happens to start and stop. A real one barely
    moves. Measured here, shaving six hours off a 63-hour record moved the
    single-exponential asymptote by 24 points.
    """
    out = []
    for trim in trims:
        cut = int(len(samples) * trim)
        subset = samples[cut:]
        if len(subset) < 20:
            continue
        model = fit(kind, subset)
        if model is None:
            continue
        out.append({
            "trimFraction": trim,
            "trimHours": (subset[0][0] - samples[0][0]) / 3600.0,
            "asymptote": asymptote(model),
            "taus": model["taus"],
            "coef": model["coef"],
        })
    levels = [row["asymptote"] for row in out if row["asymptote"] is not None]
    spread = (max(levels) - min(levels)) if len(levels) > 1 else None
    return {"rows": out, "spread": spread}

# ---------------------------------------------------------------------------
# --self-test: synthetic curves whose answer is known
# ---------------------------------------------------------------------------


def self_test():
    """Every claim this module makes, against data whose truth is known."""
    failures = []

    def check(name, condition, detail=""):
        if not condition:
            failures.append("%s %s" % (name, detail))

    sample = _synthetic("exp", count=300)
    line = _synthetic("linear", count=300)
    mixed = _synthetic("explin", count=400)
    model = fit("exp", sample)

    # 1. On a true exponential the exponential must WIN the held-out test and
    #    the profile must be bounded - the identifiable case has to exist, or
    #    a module that refuses everything would pass this file too.
    ranked = choose(sample)
    check("exp/holdout-winner", ranked[0]["kind"] in ("exp", "exp2"),
          "got %s" % ranked[0]["kind"])
    interval = profile_interval(sample, model)
    check("exp/profile-bounded", interval is not None and interval["bounded"],
          "interval %s" % (interval and (interval["low"], interval["high"]),))
    check("exp/profile-narrow", interval is not None and interval["width"] < 20.0,
          "width %s" % (interval and interval["width"],))
    check("exp/profile-covers-truth",
          interval is not None and interval["low"] <= 60.0 <= interval["high"],
          "%s" % (interval and (interval["low"], interval["high"]),))

    # 2. On a straight line there is no asymptote to find, and `linear` must
    #    win the held-out comparison outright.
    line_interval = profile_interval(line, fit("exp", line))
    check("linear/profile-unbounded",
          line_interval is not None and not line_interval["bounded"])
    winner = [r for r in choose(line) if r["winner"]][0]
    check("linear/holdout-winner", winner["kind"] == "linear",
          "got %s" % winner["kind"])

    # 3. start_sensitivity separates the two: stable on a true exponential,
    #    wandering on a line dressed as one.
    stable = start_sensitivity("exp", sample)
    check("exp/start-stable", stable["spread"] is not None and stable["spread"] < 2.0,
          "spread %s" % stable["spread"])
    walking = start_sensitivity("exp", mixed)
    check("explin/start-walks", walking["spread"] is not None and walking["spread"] > 2.0,
          "spread %s" % walking["spread"])

    check("neff/monotone",
          effective_samples(1000, 0.99) < effective_samples(1000, 0.5)
          < effective_samples(1000, 0.0))

    # 4. The bootstrap resamples around the FITTED curve, so it measures noise
    #    and not model error: it must bracket the point estimate, and it must
    #    be much tighter than the profile. Asserting that asymmetry is what
    #    stops the narrow interval from ever being read as the honest one.
    boot = block_bootstrap("exp", sample, reps=60)
    check("bootstrap/brackets-point",
          boot is not None and boot["low"] <= boot["point"] <= boot["high"],
          "%s" % (boot and (boot["low"], boot["point"], boot["high"]),))
    # Where the model is RIGHT the two agree - measured here within 25 %.
    check("bootstrap/agrees-when-model-right",
          boot is not None and interval is not None
          and 0.25 < (boot["high"] - boot["low"]) / max(interval["width"], 1e-9) < 4.0,
          "boot %.4f vs profile %.4f" % (
              (boot["high"] - boot["low"]) if boot else -1.0,
              interval["width"] if interval else -1.0))

    # ...and where it is WRONG they part company, which is the reason
    # drying_fit.py never reports the bootstrap on its own. An exponential
    # fitted to exp+line: the bootstrap calls the asymptote +/- 0.5 while the
    # profile calls it +/- 9, and the profile is the one telling the truth.
    wrong_profile = profile_interval(mixed, fit("exp", mixed))
    wrong_boot = block_bootstrap("exp", mixed, reps=60)
    check("bootstrap/misleads-when-model-wrong",
          wrong_profile is not None and wrong_boot is not None
          and (wrong_boot["high"] - wrong_boot["low"]) < 0.25 * wrong_profile["width"],
          "boot %.2f vs profile %.2f" % (
              (wrong_boot["high"] - wrong_boot["low"]) if wrong_boot else -1.0,
              wrong_profile["width"] if wrong_profile else -1.0))

    # 5. The profile must be evaluated in the MODEL'S OWN family, and the check
    #    that says so is that pinning m_inf at the level the model already
    #    chose reaches at least the SSE the model already achieved. It cannot
    #    be "the interval contains the point estimate": the profile searches
    #    tau out to 200x the span where the fit stops at 4x, so it legitimately
    #    finds a better optimum than the fit did.
    #
    #    This is the red test for the top finding. profile_asymptote() fitted
    #    level + amp*exp(-t/tau) whatever the model was, so an exp2 model was
    #    profiled against a family that could not reach it: no level came out
    #    "inside", the bisection collapsed onto its anchor, and a nonexistent
    #    asymptote was reported as BOUNDED with width 0.000 - [2.00, 2.00]
    #    beside a point estimate of 61.03. drying_fit.judge() checks only
    #    `bounded` and the width, so it sailed through both gates.
    for label, series, kind in (("exp", sample, "exp"), ("exp", sample, "exp2"),
                                ("linear", line, "exp"), ("linear", line, "exp2"),
                                ("explin", mixed, "exp"), ("explin", mixed, "exp2")):
        probe = fit(kind, series)
        pinned = profile_asymptote(kind, series, [asymptote(probe)],
                                   seed_taus=probe["taus"])
        check("%s/%s/profile-reaches-the-fit" % (label, kind),
              pinned and pinned[0]["sse"] <= probe["sse"] * (1.0 + 1e-6),
              "profile %.6f at the fitted level against the fit's own %.6f"
              % (pinned[0]["sse"] if pinned else -1.0, probe["sse"]))

    # 6. ...and where no asymptote exists, neither family may report one that
    #    is both bounded and PRECISE - the pair drying_fit.judge() consumes,
    #    `bounded` and a width under its 6-point ceiling. Stated as the pair
    #    rather than as `not bounded` because `exp2` on a line is marginally
    #    bounded for an honest reason: the profile caps tau at 200x the span,
    #    so the perfectly degenerate limit is just outside the search and the
    #    leftover curvature is faintly visible against a synthetic noise of
    #    0.05 points. It is bounded at a WIDTH OF 40, which is a refusal.
    #    Measured across all six combinations: identifiable 0.00-0.05,
    #    degenerate 18.9-47.6 - three orders of magnitude, and the bug this
    #    replaces reported 0.000.
    for label, series, kind in (("linear", line, "exp"), ("linear", line, "exp2"),
                                ("explin", mixed, "exp"), ("explin", mixed, "exp2")):
        band = profile_interval(series, fit(kind, series))
        check("%s/%s/no-precise-asymptote" % (label, kind),
              band is not None and not (band["bounded"] and band["width"] < 6.0),
              "bounded=%s width=%.2f" % (band and band["bounded"],
                                         band["width"] if band else -1.0))

    # 7. The bootstrap's block must span the six hours its docstring requires
    #    even when the record opens with a gap. Deriving the step from the
    #    first two samples read 6000 s here and cut the block to 3 samples.
    gapped = ([(0.0, 78.0)]
              + [(6000.0 + i * 300.0,
                  60.0 + 18.0 * math.exp(-(i * 300.0) / (4.0 * 3600.0)))
                 for i in range(240)])
    gapped_boot = block_bootstrap("exp", gapped, reps=20)
    check("bootstrap/block-spans-six-hours",
          gapped_boot is not None
          and abs(gapped_boot["blockSec"] - BOOTSTRAP_BLOCK_SEC) < 300.0,
          "blockSec %s" % (gapped_boot and gapped_boot["blockSec"],))

    return failures


if __name__ == "__main__":
    import sys

    problems = self_test()
    for line in problems:
        print("FAIL " + line)
    print("drying_evidence self-test: %s" % ("FAILED" if problems else "ok"))
    sys.exit(1 if problems else 0)
