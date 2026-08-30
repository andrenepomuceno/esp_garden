#include "core/probe_health.h"
#include <math.h>

void
probeHealthReset(ProbeHealth& health)
{
    health.n = 0.0;
    health.sumX = 0.0;
    health.sumY = 0.0;
    health.sumXX = 0.0;
    health.sumXY = 0.0;
    health.sumYY = 0.0;
    health.samples = 0;
    health.railLow = 0;
    health.railHigh = 0;
    // Deliberately inverted, so the first sample sets both.
    health.minRaw = 5000;
    health.maxRaw = -1;
}

void
probeHealthAdd(ProbeHealth& health, int previousRaw, int first, int second)
{
    // The settled value is the SECOND conversion. Everything below that judges
    // the level uses it; only the regression uses the first.
    const double drive = (double)previousRaw - (double)second;
    const double delta = (double)first - (double)second;

    health.n += 1.0;
    health.sumX += drive;
    health.sumY += delta;
    health.sumXX += drive * drive;
    health.sumXY += drive * delta;
    health.sumYY += delta * delta;

    ++health.samples;
    if (second <= PROBE_RAIL_LOW) {
        ++health.railLow;
    }
    if (second >= PROBE_RAIL_HIGH) {
        ++health.railHigh;
    }
    if (second < health.minRaw) {
        health.minRaw = second;
    }
    if (second > health.maxRaw) {
        health.maxRaw = second;
    }
}

void
probeHealthDecay(ProbeHealth& health, double factor)
{
    if (factor < 0.0) {
        factor = 0.0;
    } else if (factor > 1.0) {
        factor = 1.0;
    }

    // Every sum scales together, so the slope this evidence implies does not
    // move — only how much of it there is. That is the same contract
    // gaussianDecay() keeps, and the reason a decayed accumulator can be read
    // at any time without a discontinuity.
    health.n *= factor;
    health.sumX *= factor;
    health.sumY *= factor;
    health.sumXX *= factor;
    health.sumXY *= factor;
    health.sumYY *= factor;

    health.samples = (uint32_t)(health.samples * factor);
    health.railLow = (uint32_t)(health.railLow * factor);
    health.railHigh = (uint32_t)(health.railHigh * factor);

    // The observed range is a running extreme, not a sum, so it cannot be
    // scaled. It is collapsed back onto the midpoint instead, which lets a
    // probe that has started moving again escape a STUCK verdict.
    if (health.maxRaw >= health.minRaw) {
        const int32_t mid = (health.minRaw + health.maxRaw) / 2;
        health.minRaw = mid;
        health.maxRaw = mid;
    }
}

// Denominator of the least-squares slope: n * Sxx - Sx^2, which is n^2 times
// the variance of `drive`. It is zero when every reading followed the same
// step, and then there is no slope to speak of.
static double
spread(const ProbeHealth& health)
{
    return health.n * health.sumXX - health.sumX * health.sumX;
}

double
probeHealthSlope(const ProbeHealth& health)
{
    const double sxx = spread(health);
    if (health.n < 3.0 || sxx <= 1e-9) {
        return 0.0;
    }
    return (health.n * health.sumXY - health.sumX * health.sumY) / sxx;
}

double
probeHealthSlopeStdErr(const ProbeHealth& health)
{
    if (health.n < 4.0) {
        return 0.0;
    }

    // Ordinary least squares, on centred sums. Exact rather than bounded: an
    // earlier version dodged accumulating Syy and estimated the residual from
    // the spread of the FITTED values, which is not a bound on anything —
    // fitted spread is what the model explains, and the standard error is
    // about what it does not.
    const double sxxC = health.sumXX - health.sumX * health.sumX / health.n;
    const double syyC = health.sumYY - health.sumY * health.sumY / health.n;
    const double sxyC = health.sumXY - health.sumX * health.sumY / health.n;
    if (sxxC <= 1e-9) {
        return 0.0;
    }

    // RSS = Syy - slope * Sxy, both centred. Clamped at zero: the sums are
    // incremental and catastrophic cancellation can push it slightly negative
    // on a near-perfect fit, which is exactly when it should be ~0 anyway.
    double rss = syyC - (sxyC / sxxC) * sxyC;
    if (rss < 0.0) {
        rss = 0.0;
    }

    return sqrt(rss / ((health.n - 2.0) * sxxC));
}

double
probeHealthT(const ProbeHealth& health)
{
    const double se = probeHealthSlopeStdErr(health);
    if (se <= 0.0) {
        return 0.0;
    }
    return probeHealthSlope(health) / se;
}

int
probeHealthVerdict(const ProbeHealth& health,
                   uint32_t minSamples,
                   double minSlope,
                   double minT)
{
    if (health.samples < minSamples) {
        return PROBE_UNKNOWN;
    }

    // Checked before the settling test because it is unambiguous: a pin held
    // at a rail is shorted, or has no divider on it at all, and the regression
    // has nothing to work with anyway since every conversion reads the same.
    if (health.railLow >= health.samples || health.railHigh >= health.samples) {
        return PROBE_RAILED;
    }

    // The settling test comes BEFORE the flatline one, because it is positive
    // evidence and the other is an inference from absence. A floating pin can
    // sit nearly still for a long stretch — measured on this board, one held a
    // median step of 0.02 % between minutes — so checking "has not moved"
    // first would name the wrong fault for the commonest case. `drive` still
    // varies while the node does not, so the regression is estimable either
    // way.
    const double slope = probeHealthSlope(health);
    const double t = probeHealthT(health);
    if (slope >= minSlope && t >= minT) {
        return PROBE_FLOATING;
    }

    // Not one ADC count of movement across the whole window, and no coupling
    // to the previous channel either: a module that died while still driving a
    // level, which a floating pin does not look like.
    if (health.maxRaw >= 0 && health.maxRaw == health.minRaw) {
        return PROBE_STUCK;
    }

    return PROBE_CONNECTED;
}

const char*
probeVerdictName(int verdict)
{
    switch (verdict) {
        case PROBE_CONNECTED:
            return "connected";
        case PROBE_FLOATING:
            return "floating";
        case PROBE_RAILED:
            return "railed";
        case PROBE_STUCK:
            return "stuck";
        default:
            return "";
    }
}
