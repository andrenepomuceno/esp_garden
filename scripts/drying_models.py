#!/usr/bin/env python3
"""The four models offered for a falling soil-moisture trace, and the fit.

The whole point of this file is that the models are offered TOGETHER. Fitting
only the one the hypothesis names is how a hypothesis gets confirmed:

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

HOW MUCH OF A FIT IS REAL is the other half, and it lives next door in
drying_evidence.py - the model comparison, the profile likelihood, the
bootstrap and the start-trim check. This file stops at producing a curve;
that one decides whether to believe it. They were one file until it crossed the
1000-line gate check_lines.py enforces.

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

# A tau past this multiple of the fitted window is flagged: over that span the
# exponential is linear to within the noise, so its amplitude and its asymptote
# trade off against each other without limit.
TAU_DEGENERATE_FRACTION = 1.0


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


def _solve_system(matrix, vector):
    """Gaussian elimination with partial pivoting. Both arguments are consumed.

    Split out of solve_normal() so the profiler next door can reuse it on a
    Gram matrix it accumulated itself, which is what lets a pinned-asymptote
    SSE be answered in O(1) per level instead of O(n).
    """
    width = len(vector)
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
    return _solve_system(matrix, vector)


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

# How many LINEAR columns each design has, declared rather than measured by
# building a row with dummy arguments. choose() did the latter, which made its
# tie-break depend on `e['taus'] or [1.0]` carrying as many entries as
# design_row would index - true today, and silently wrong for the first kind
# added with more time constants than the placeholder supplies.
COLUMNS = {"linear": 2, "exp": 2, "exp2": 3, "explin": 3}

# Every design puts the constant first and the amplitude belonging to taus[i]
# at coef[1 + i]. That is the whole reason a time constant can be reordered at
# all - as long as its amplitude travels with it.
AMPLITUDE_OFFSET = 1


def _order_taus(taus, coef):
    """Sort the time constants, carrying each one's amplitude along.

    fit() used to return `sorted(taus)` beside coefficients fitted against the
    UNSORTED order. The refinement loop multiplies each tau by 0.9/1.1 for up
    to 80 rounds and can walk taus[0] past taus[1]; sorting then silently
    re-pairs every amplitude with the wrong exponential, so predict() draws a
    curve the fit never saw and `sse` describes one that is no longer
    reachable. Everything downstream reads the wrong one: residuals(),
    criteria() (rho1/nEff/aicEff/bicEff off garbage residuals while log_mse
    uses the true sse), holdout(), choose()'s ranking, block_bootstrap().
    Measured before the fix, one exp2 fit stored sse 0.361 while predict()
    gave 10 618 - and reversing its taus reproduced the stored value exactly.
    """
    if len(taus) < 2:
        return list(taus), list(coef)
    order = sorted(range(len(taus)), key=lambda i: taus[i])
    ordered = list(coef)
    for slot, source in enumerate(order):
        ordered[AMPLITUDE_OFFSET + slot] = coef[AMPLITUDE_OFFSET + source]
    return [taus[i] for i in order], ordered


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

    taus, coef = _order_taus(best[2], best[1])
    return {"kind": kind, "coef": coef, "taus": taus,
            "origin": origin, "sse": best[0], "span": span,
            "params": len(coef) + ntau}


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



# ---------------------------------------------------------------------------
# --self-test: synthetic curves whose answer is known
# ---------------------------------------------------------------------------


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

    # 2. On a straight line the exponential's tau must run away. This is the
    #    degeneracy the whole pair of files exists to detect, so a green here
    #    is load-bearing; drying_evidence.py checks what the profile then says
    #    about it.
    line = _synthetic("linear", count=300)
    line_exp = fit("exp", line)
    check("linear/exp-degenerate", is_degenerate(line_exp),
          "tau %.1f h vs span %.1f h" % (line_exp["taus"][0] / 3600.0,
                                         line_exp["span"] / 3600.0))

    # 3. exp + line: the fast tau is recoverable, and exp2 collapses onto
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
    check("asymptote/explin-none", asymptote(mixed_fit) is None)

    # 4. THE ROUND TRIP THAT WAS MISSING. predict() must reproduce the sse the
    #    fit reported, for every kind on every curve here. Nothing asserted
    #    this, which is exactly why sorted taus beside unsorted coefficients
    #    shipped green: `fit` returned a model whose stored sse belonged to a
    #    curve predict() would never draw. `crossed` is the dataset whose
    #    refinement walks taus[0] past taus[1] - before the fix
    #    fit("exp2", crossed) stored sse 0.361 while predict() gave 10 618.
    crossed = _synthetic("exp", count=150, noise=0.05, seed=50)
    for label, series in (("exp", sample), ("linear", line),
                          ("explin", mixed), ("crossed", crossed)):
        for kind in MODELS:
            round_trip = fit(kind, series)
            if round_trip is None:
                continue
            got = sum(value * value for value in residuals(round_trip, series))
            check("%s/%s/sse-round-trip" % (label, kind),
                  abs(got - round_trip["sse"])
                  <= 1e-6 * max(1.0, round_trip["sse"]),
                  "stored %.6f, predict() gives %.6f" % (round_trip["sse"], got))
            check("%s/%s/taus-sorted" % (label, kind),
                  round_trip["taus"] == sorted(round_trip["taus"]),
                  "%s" % round_trip["taus"])

    # 5. COLUMNS is a claim about design_row, so it is checked against it
    #    rather than trusted.
    for kind in MODELS:
        check("columns/%s" % kind,
              COLUMNS[kind] == len(design_row(kind, 1.0, 0.0,
                                              [1.0] * max(1, TAU_COUNT[kind]))),
              "declared %d" % COLUMNS[kind])

    # 6. Housekeeping the rest of the pair leans on. The docstring promises an
    #    empty bin produces NO POINT, and that is what has to be tested: this
    #    used to read `all(v == v for _, v in binned)`, a NaN test on values
    #    resample() cannot make NaN, so it asserted nothing. The 300-600 s bin
    #    here is empty, so nothing may sit at its centre.
    binned = resample([(0.0, 1.0), (10.0, 3.0), (700.0, 5.0)], 300.0)
    check("resample/bins", len(binned) == 2 and abs(binned[0][1] - 2.0) < 1e-9,
          "%s" % (binned,))
    check("resample/gap-makes-no-point",
          len(binned) == 2 and all(abs(stamp - 450.0) > 1e-9
                                   for stamp, _ in binned),
          "%s" % (binned,))

    return failures


if __name__ == "__main__":
    import sys

    problems = self_test()
    for line in problems:
        print("FAIL " + line)
    print("drying_models self-test: %s" % ("FAILED" if problems else "ok"))
    sys.exit(1 if problems else 0)
