#!/usr/bin/env python3
"""The curve fitting behind scripts/drying_fit.py, with no archive in it.

Four models are offered for a stretch of falling soil moisture, and the whole
point of the file is that they are offered TOGETHER. Fitting only the one the
hypothesis names is how a hypothesis gets confirmed:

    linear   m(t) = c0 + c1 * t                      no asymptote
    exp      m(t) = m_inf + A * exp(-t/tau)          ONE asymptote
    exp2     m(t) = m_inf + A1*exp(-t/tau1)
                          + A2*exp(-t/tau2)          ONE asymptote, two rates
    explin   m(t) = c0 + A * exp(-t/tau) + c1 * t    NO asymptote, one rate

`explin` is in the list because it is what `exp2` becomes when the second time
constant runs past the window: an exponential whose tau is several times the
span is a straight line wearing three parameters. Without it in the comparison
that degeneracy is invisible - exp2 simply "wins", and its m_inf, which is then
the intercept of a line and not an asymptote of anything, gets read as a
physical number. Measured on this archive: exp2 and explin land within 3 % of
each other on SSE while exp2 reports an asymptote of 44.81 and explin reports
that there is none.

WHY THE OBVIOUS CRITERION IS THE WRONG ONE

Soil moisture sampled every 60 s is a smooth curve, so the residuals of ANY of
these models are almost perfectly autocorrelated - measured here, lag-1 rho of
0.92 to 0.99. AIC and BIC assume independent residuals, and with rho that high
they count roughly n independent observations where there are a few dozen. The
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

Standard library only, same as moisture_stats.py next door, and Arduino-free in
the same sense: nothing here touches a device, a file or a clock, so
`--self-test` can exercise all of it against synthetic curves.
"""

from __future__ import annotations

import math
import random

# ---------------------------------------------------------------------------
# Thresholds. Every one is a judgement, so every one says what it was set
# against. The measurements quoted are from backups/telemetry.sqlite, 10 days
# of this garden, and are reproduced by `drying_fit.py --report`.
# ---------------------------------------------------------------------------

# The tau grid spans span/500 .. span*4. The low end is below any rate a 300 s
# sample can resolve; the high end is where an exponential has already become a
# straight line, and is deliberately INSIDE the grid rather than excluded, so a
# degenerate fit is reported as degenerate instead of being clipped into
# looking plausible.
TAU_MIN_FRACTION = 1.0 / 500.0
TAU_MAX_FRACTION = 4.0

# The PROFILE grid runs far further, and that is not a detail. Dragging m_inf
# downwards is paid for by lengthening tau, and the trade is exact in the limit
# - as tau grows the exponential becomes 1 - t/tau, so (m_inf, A) with A/tau
# held fixed all describe the same straight line. Capping the profile's tau at
# the fitting cap hides that: the search runs out of room, SSE shoots up, and a
# flat valley is reported as a bounded interval. Measured on a synthetic noisy
# line, a cap of 4x the span returned a 2.7-point "95 % interval" for an
# asymptote that is not there at all.
TAU_MAX_FRACTION_PROFILE = 200.0

# A tau past this multiple of the fitted window is flagged: over that span the
# exponential is linear to within the noise, so its amplitude and its asymptote
# trade off against each other without limit.
TAU_DEGENERATE_FRACTION = 1.0

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
# small numerics
# ---------------------------------------------------------------------------


def resample(samples, step_sec):
    """Bin-average onto a uniform grid.

    The archive is not uniformly spaced - 60 s until 2026-09-03 18:43 and 300 s
    after it, plus reboots - and a least-squares fit weights every sample
    equally, so the dense stretch would silently dominate a segment that spans
    the change. Averaging into fixed bins removes that without pretending to
    interpolate across a gap: an empty bin produces no point at all.
    """
    if not samples:
        return []
    out = []
    edge = samples[0][0] + step_sec
    bucket = []
    for stamp, value in samples:
        while stamp >= edge:
            if bucket:
                out.append((edge - step_sec / 2.0, sum(bucket) / len(bucket)))
            bucket = []
            edge += step_sec
        bucket.append(value)
    if bucket:
        out.append((edge - step_sec / 2.0, sum(bucket) / len(bucket)))
    return out


def solve_normal(rows, target):
    """Least squares through the normal equations, with partial pivoting.

    At most four columns and well-scaled ones - a constant, one or two
    exponentials in [0,1] and a time axis in hours - so the conditioning that
    makes normal equations a bad idea in general does not arise here, and the
    alternative would be a QR nobody can check by eye.
    """
    width = len(rows[0])
    count = len(target)
    matrix = [[sum(rows[i][p] * rows[i][q] for i in range(count))
               for q in range(width)] for p in range(width)]
    vector = [sum(rows[i][p] * target[i] for i in range(count))
              for p in range(width)]
    for col in range(width):
        pivot = max(range(col, width), key=lambda r: abs(matrix[r][col]))
        if abs(matrix[pivot][col]) < 1e-12:
            return None
        matrix[col], matrix[pivot] = matrix[pivot], matrix[col]
        vector[col], vector[pivot] = vector[pivot], vector[col]
        for row in range(col + 1, width):
            factor = matrix[row][col] / matrix[col][col]
            for q in range(col, width):
                matrix[row][q] -= factor * matrix[col][q]
            vector[row] -= factor * vector[col]
    coef = [0.0] * width
    for row in range(width - 1, -1, -1):
        rest = sum(matrix[row][q] * coef[q] for q in range(row + 1, width))
        coef[row] = (vector[row] - rest) / matrix[row][row]
    return coef


def design_row(kind, stamp, origin, taus):
    elapsed = stamp - origin
    if kind == "linear":
        return [1.0, elapsed / 3600.0]
    if kind == "exp":
        return [1.0, math.exp(-elapsed / taus[0])]
    if kind == "exp2":
        return [1.0, math.exp(-elapsed / taus[0]), math.exp(-elapsed / taus[1])]
    if kind == "explin":
        return [1.0, math.exp(-elapsed / taus[0]), elapsed / 3600.0]
    raise ValueError("unknown model %r" % (kind,))


MODELS = ("linear", "exp", "exp2", "explin")
TAU_COUNT = {"linear": 0, "exp": 1, "exp2": 2, "explin": 1}

# Which models claim an asymptote at all. `explin` deliberately does not: its
# constant term is the intercept of a line, and reading it as "fully dry" is
# exactly the mistake this module exists to make visible.
HAS_ASYMPTOTE = {"linear": False, "exp": True, "exp2": True, "explin": False}


def _sse(kind, samples, origin, taus):
    rows = [design_row(kind, t, origin, taus) for t, _ in samples]
    coef = solve_normal(rows, [v for _, v in samples])
    if coef is None:
        return None, None
    total = 0.0
    for row, (_, value) in zip(rows, samples):
        total += (value - sum(a * b for a, b in zip(row, coef))) ** 2
    return total, coef


def fit(kind, samples, grid_points=None):
    """Separable least squares: grid + local refinement over tau, linear inside.

    The linear coefficients have a closed form once tau is fixed, so only the
    time constants are searched. That is what keeps a two-exponential fit
    honest without an optimiser: the grid is coarse but exhaustive, so it
    cannot sit in a local minimum the way a gradient method started from a
    guess can, and the refinement only polishes.
    """
    if len(samples) < 4:
        return None
    origin = samples[0][0]
    span = samples[-1][0] - origin
    if span <= 0:
        return None
    ntau = TAU_COUNT[kind]
    if ntau == 0:
        sse, coef = _sse(kind, samples, origin, [])
        if coef is None:
            return None
        return {"kind": kind, "coef": coef, "taus": [], "origin": origin,
                "sse": sse, "span": span, "params": len(coef)}

    if grid_points is None:
        grid_points = 96 if ntau == 1 else 44
    low = span * TAU_MIN_FRACTION
    high = span * TAU_MAX_FRACTION
    grid = [low * math.exp(math.log(high / low) * (i / (grid_points - 1.0)))
            for i in range(grid_points)]
    if ntau == 1:
        combos = [[value] for value in grid]
    else:
        combos = [[grid[i], grid[j]]
                  for i in range(len(grid)) for j in range(i + 1, len(grid))]

    best = None
    for taus in combos:
        sse, coef = _sse(kind, samples, origin, taus)
        if coef is None:
            continue
        if best is None or sse < best[0]:
            best = (sse, coef, list(taus))
    if best is None:
        return None

    for _ in range(80):
        improved = False
        for index in range(ntau):
            for factor in (0.97, 1.03, 0.9, 1.1):
                taus = list(best[2])
                taus[index] *= factor
                if not (low <= taus[index] <= high):
                    continue
                sse, coef = _sse(kind, samples, origin, taus)
                if coef is not None and sse < best[0] - 1e-12:
                    best = (sse, coef, taus)
                    improved = True
        if not improved:
            break

    return {"kind": kind, "coef": best[1], "taus": sorted(best[2]),
            "origin": origin, "sse": best[0], "span": span,
            "params": len(best[1]) + ntau}


def predict(model, stamp):
    row = design_row(model["kind"], stamp, model["origin"], model["taus"])
    return sum(a * b for a, b in zip(row, model["coef"]))


def residuals(model, samples):
    return [value - predict(model, stamp) for stamp, value in samples]


def asymptote(model):
    """The level the model settles at, or None when it does not settle."""
    return model["coef"][0] if HAS_ASYMPTOTE[model["kind"]] else None


def is_degenerate(model):
    """True when a time constant has run so long the exponential is a line."""
    return any(tau > model["span"] * TAU_DEGENERATE_FRACTION
               for tau in model["taus"])


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
                   key=lambda e: (len(design_row(e["kind"], 0.0, 0.0,
                                                 e["taus"] or [1.0])),
                                  TAU_COUNT[e["kind"]]))
    for entry in scored:
        entry["winner"] = entry["kind"] == simplest["kind"]
    return scored


# ---------------------------------------------------------------------------
# is the asymptote there at all? three ways of asking
# ---------------------------------------------------------------------------


def _sse_at_asymptote(samples, origin, level, tau):
    num = 0.0
    den = 0.0
    for stamp, value in samples:
        weight = math.exp(-(stamp - origin) / tau)
        num += weight * (value - level)
        den += weight * weight
    if den <= 0.0:
        return None, 0.0
    amp = num / den
    total = sum((value - (level + amp * math.exp(-(stamp - origin) / tau))) ** 2
                for stamp, value in samples)
    return total, amp


def profile_asymptote(samples, levels, grid_points=160):
    """Best achievable SSE with m_inf pinned, for each level offered.

    Profiling rather than reporting a standard error is deliberate. A standard
    error is a local curvature, and the whole difficulty here is that the
    surface is not locally quadratic: it is a long flat valley running towards
    m_inf = -infinity, which a curvature at the optimum cannot see.
    """
    origin = samples[0][0]
    span = samples[-1][0] - origin
    low = span * TAU_MIN_FRACTION
    high = span * TAU_MAX_FRACTION_PROFILE
    grid = [low * math.exp(math.log(high / low) * (i / (grid_points - 1.0)))
            for i in range(grid_points)]
    out = []
    for level in levels:
        best = None
        for tau in grid:
            sse, amp = _sse_at_asymptote(samples, origin, level, tau)
            if sse is not None and (best is None or sse < best[0]):
                best = (sse, tau, amp)
        if best is None:
            continue
        # The grid spans five orders of magnitude, so its steps are ~7 % apart
        # and the best grid point can sit well off the optimum. Left coarse,
        # the profile reads too HIGH everywhere and the interval that comes out
        # of it is too narrow - which is the direction that lies.
        for _ in range(60):
            improved = False
            for factor in (0.99, 1.01, 0.95, 1.05):
                tau = best[1] * factor
                if not (low <= tau <= high):
                    continue
                sse, amp = _sse_at_asymptote(samples, origin, level, tau)
                if sse is not None and sse < best[0] - 1e-12:
                    best = (sse, tau, amp)
                    improved = True
            if not improved:
                break
        out.append({"level": level, "sse": best[0], "tau": best[1],
                    "amp": best[2]})
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
    """
    if not HAS_ASYMPTOTE[model["kind"]]:
        return None
    top = max(value for _, value in samples)
    point = asymptote(model)
    if point is None:
        return None

    cache = {}

    def sse_at(level):
        key = round(level, 6)
        if key not in cache:
            found = profile_asymptote(samples, [level])
            cache[key] = found[0]["sse"] if found else float("inf")
        return cache[key]

    coarse = profile_asymptote(samples,
                               [top - (top - floor_level) * (i / (steps - 1.0))
                                for i in range(steps)])
    if not coarse:
        return None
    best = min(min(entry["sse"] for entry in coarse), model["sse"])
    threshold = _f_threshold(criteria(model, samples)["nEff"], model["params"])
    limit = best * threshold

    def inside(level):
        return sse_at(level) <= limit

    anchor = point if inside(point) else min(coarse, key=lambda e: e["sse"])["level"]

    def edge(direction, bound):
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

    high, _ = edge(+1, top)
    low, bounded_below = edge(-1, floor_level)
    return {
        "low": low,
        "high": high,
        "width": high - low,
        "bounded": bounded_below,
        "floorLevel": floor_level,
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
    step = samples[1][0] - samples[0][0]
    if step <= 0:
        return None
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
            start = rng.randrange(0, max(1, count - length))
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


def _synthetic(kind, count=400, step=300.0, noise=0.05, seed=5):
    rng = random.Random(seed)
    out = []
    for i in range(count):
        t = i * step
        if kind == "exp":
            value = 60.0 + 25.0 * math.exp(-t / (4.0 * 3600.0))
        elif kind == "linear":
            value = 80.0 - 2.5 * (t / 86400.0)
        elif kind == "explin":
            value = 80.0 + 12.0 * math.exp(-t / (2.0 * 3600.0)) - 2.9 * (t / 86400.0)
        else:
            raise ValueError(kind)
        out.append((t, value + rng.gauss(0.0, noise)))
    return out


def self_test():
    """Every claim this module makes, against data whose truth is known."""
    failures = []

    def check(name, condition, detail=""):
        if not condition:
            failures.append("%s %s" % (name, detail))

    # 1. A known exponential is recovered - tau and asymptote both.
    sample = _synthetic("exp", count=300)
    model = fit("exp", sample)
    check("exp/tau", abs(model["taus"][0] / 3600.0 - 4.0) < 0.4,
          "got %.2f h" % (model["taus"][0] / 3600.0))
    check("exp/asymptote", abs(asymptote(model) - 60.0) < 0.5,
          "got %.2f" % asymptote(model))

    # 2. On that curve the exponential must WIN the held-out test, and the
    #    profile interval must be bounded - the identifiable case exists.
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

    # 3. On a straight line, the exponential's tau must run away and its
    #    asymptote must come out unidentified. This is the degeneracy the
    #    whole file exists to detect, so a green here is load-bearing.
    line = _synthetic("linear", count=300)
    line_exp = fit("exp", line)
    check("linear/exp-degenerate", is_degenerate(line_exp),
          "tau %.1f h vs span %.1f h" % (line_exp["taus"][0] / 3600.0,
                                         line_exp["span"] / 3600.0))
    line_interval = profile_interval(line, line_exp)
    check("linear/profile-unbounded",
          line_interval is not None and not line_interval["bounded"])
    ranked_line = choose(line)
    winner = [r for r in ranked_line if r["winner"]][0]
    check("linear/holdout-winner", winner["kind"] == "linear",
          "got %s" % winner["kind"])

    # 4. exp + line: the fast tau is recoverable, and exp2 collapses onto
    #    explin rather than resolving two real rates.
    mixed = _synthetic("explin", count=400)
    mixed_fit = fit("explin", mixed)
    check("explin/tau", abs(mixed_fit["taus"][0] / 3600.0 - 2.0) < 0.3,
          "got %.2f h" % (mixed_fit["taus"][0] / 3600.0))
    mixed_two = fit("exp2", mixed)
    check("explin/exp2-collapses", is_degenerate(mixed_two),
          "taus %s" % [round(t / 3600.0, 2) for t in mixed_two["taus"]])
    # The claim is not that exp2 ties explin to the last digit - it is that
    # exp2 buys nothing by having a second exponential, because it spends it on
    # being a line. So: never MORE than 10 % better than the model that says so.
    check("explin/exp2-buys-nothing",
          mixed_two["sse"] >= 0.90 * mixed_fit["sse"],
          "sse %.4f vs %.4f" % (mixed_two["sse"], mixed_fit["sse"]))

    # 5. start_sensitivity separates the two: stable on a true exponential,
    #    wandering on a line dressed as one.
    stable = start_sensitivity("exp", sample)
    check("exp/start-stable", stable["spread"] is not None and stable["spread"] < 2.0,
          "spread %s" % stable["spread"])
    walking = start_sensitivity("exp", mixed)
    check("explin/start-walks", walking["spread"] is not None and walking["spread"] > 2.0,
          "spread %s" % walking["spread"])

    # 6. Housekeeping the rest of the file leans on.
    binned = resample([(0.0, 1.0), (10.0, 3.0), (700.0, 5.0)], 300.0)
    check("resample/bins", len(binned) == 2 and abs(binned[0][1] - 2.0) < 1e-9,
          "%s" % (binned,))
    check("resample/no-empty-bin", all(v == v for _, v in binned))
    check("neff/monotone",
          effective_samples(1000, 0.99) < effective_samples(1000, 0.5)
          < effective_samples(1000, 0.0))
    check("asymptote/explin-none", asymptote(mixed_fit) is None)

    # The bootstrap resamples around the FITTED curve, so it measures noise and
    # not model error: it must bracket the point estimate, and it must be much
    # tighter than the profile. Asserting that asymmetry is what stops the
    # narrow interval from ever being read as the honest one.
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
    wrong_model = fit("exp", mixed)
    wrong_profile = profile_interval(mixed, wrong_model)
    wrong_boot = block_bootstrap("exp", mixed, reps=60)
    check("bootstrap/misleads-when-model-wrong",
          wrong_profile is not None and wrong_boot is not None
          and (wrong_boot["high"] - wrong_boot["low"]) < 0.25 * wrong_profile["width"],
          "boot %.2f vs profile %.2f" % (
              (wrong_boot["high"] - wrong_boot["low"]) if wrong_boot else -1.0,
              wrong_profile["width"] if wrong_profile else -1.0))

    return failures


if __name__ == "__main__":
    import sys

    problems = self_test()
    for line in problems:
        print("FAIL " + line)
    print("drying_models self-test: %s" % ("FAILED" if problems else "ok"))
    sys.exit(1 if problems else 0)
