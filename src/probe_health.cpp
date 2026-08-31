#include "core/probe_health.h"
#include <math.h>

void
probeHealthReset(ProbeHealth& health)
{
    health.n = 0.0;
    health.sumD = 0.0;
    health.sumDD = 0.0;
    health.prevRaw = 0;
    health.hasPrev = false;
    health.samples = 0;
}

void
probeHealthAdd(ProbeHealth& health, int settled)
{
    if (health.hasPrev) {
        const double step = (double)settled - (double)health.prevRaw;
        health.n += 1.0;
        health.sumD += step;
        health.sumDD += step * step;
    }
    health.prevRaw = settled;
    health.hasPrev = true;
    ++health.samples;
}

void
probeHealthDecay(ProbeHealth& health, double factor)
{
    if (factor < 0.0) {
        factor = 0.0;
    } else if (factor > 1.0) {
        factor = 1.0;
    }

    // All three scale together, so the spread this evidence implies does not
    // move — only how much of it there is. That is the contract gaussianDecay()
    // keeps, and the reason a decayed accumulator can be read at any moment
    // without a discontinuity.
    health.n *= factor;
    health.sumD *= factor;
    health.sumDD *= factor;
    health.samples = (uint32_t)(health.samples * factor);

    // Deliberately NOT reset: the last reading is still the right baseline for
    // the next difference. Clearing it would manufacture one enormous step out
    // of nothing every time the evidence ages.
}

double
probeHealthStepSd(const ProbeHealth& health)
{
    if (health.n < 2.0) {
        return 0.0;
    }
    const double mean = health.sumD / health.n;
    double var = health.sumDD / health.n - mean * mean;
    // E[x^2] - E[x]^2 can go slightly negative on a nearly constant signal,
    // which is exactly when the answer should be zero.
    if (var <= 0.0) {
        return 0.0;
    }
    return sqrt(var);
}

int
probeHealthVerdict(const ProbeHealth& health,
                   uint32_t minSamples,
                   double maxStepSd)
{
    // "Not enough evidence" and "evidence of health" are different claims, and
    // the caller has to be able to tell them apart: a probe is not declared
    // sound because nobody has looked yet.
    if (health.samples < minSamples) {
        return PROBE_UNKNOWN;
    }

    if (probeHealthStepSd(health) > maxStepSd) {
        return PROBE_NOISY;
    }

    return PROBE_CONNECTED;
}

const char*
probeVerdictName(int verdict)
{
    switch (verdict) {
        case PROBE_CONNECTED:
            return "connected";
        case PROBE_NOISY:
            return "noisy";
        default:
            return "";
    }
}
