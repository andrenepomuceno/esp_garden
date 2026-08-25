#include "core/moisture_classifier.h"
#include <math.h>

// A tenth of a percentage point of reading, squared. Well below this sensor's
// noise, so it never distorts a real fit — it exists only so a class whose
// samples happen to be identical cannot produce an infinite log-likelihood and
// win every comparison.
const double g_gaussianVarianceFloor = 0.01;

void
gaussianReset(GaussianStats& stats)
{
    stats.weight = 0.0;
    stats.sum = 0.0;
    stats.sumSq = 0.0;
}

void
gaussianAdd(GaussianStats& stats, double value, double weight)
{
    if (weight <= 0.0 || !isfinite(value)) {
        return;
    }
    stats.weight += weight;
    stats.sum += value * weight;
    stats.sumSq += value * value * weight;
}

void
gaussianDecay(GaussianStats& stats, double factor)
{
    if (factor < 0.0) {
        factor = 0.0;
    } else if (factor > 1.0) {
        factor = 1.0;
    }
    // All three scale together, so the mean and variance are unchanged and
    // only the CONFIDENCE in them decays. That is the intent: yesterday's soil
    // is still evidence about today's, just less of it.
    stats.weight *= factor;
    stats.sum *= factor;
    stats.sumSq *= factor;
}

double
gaussianMean(const GaussianStats& stats)
{
    if (stats.weight <= 0.0) {
        return 0.0;
    }
    return stats.sum / stats.weight;
}

double
gaussianVariance(const GaussianStats& stats)
{
    if (stats.weight <= 0.0) {
        return g_gaussianVarianceFloor;
    }

    const double mean = stats.sum / stats.weight;
    double variance = (stats.sumSq / stats.weight) - (mean * mean);

    // Catastrophic cancellation in E[x^2] - E[x]^2 can push this slightly
    // negative when the spread is tiny relative to the mean, which is exactly
    // the case here: readings cluster near 40 with a spread under 1.
    if (!(variance > g_gaussianVarianceFloor)) {
        variance = g_gaussianVarianceFloor;
    }
    return variance;
}

double
moistureSeparation(const GaussianStats classes[MOISTURE_CLASS_COUNT])
{
    const GaussianStats& dry = classes[MOISTURE_DRY];
    const GaussianStats& wet = classes[MOISTURE_WET];

    if (dry.weight <= 0.0 || wet.weight <= 0.0) {
        return 0.0;
    }

    const double delta = gaussianMean(wet) - gaussianMean(dry);
    const double pooled = gaussianVariance(wet) + gaussianVariance(dry);
    if (pooled <= 0.0) {
        return 0.0;
    }
    return (delta * delta) / pooled;
}

bool
moistureModelIsUsable(const GaussianStats classes[MOISTURE_CLASS_COUNT],
                      double minWeightPerClass,
                      double minSeparation)
{
    for (int i = 0; i < MOISTURE_CLASS_COUNT; ++i) {
        if (classes[i].weight < minWeightPerClass) {
            return false;
        }
    }

    if (moistureSeparation(classes) < minSeparation) {
        return false;
    }

    // Humid must lie BETWEEN dry and wet. Polarity is not assumed: with the
    // current 100-ADC% conversion a wetter soil reads higher, but a different
    // probe or conversion inverts that, exactly as the two-point calibration
    // makes no assumption either.
    //
    // This is not a cosmetic check. If humid does not sit between the other
    // two then the labels disagree with the physics that produced them, and
    // every classification is a coin toss wearing a posterior.
    const double dry = gaussianMean(classes[MOISTURE_DRY]);
    const double humid = gaussianMean(classes[MOISTURE_HUMID]);
    const double wet = gaussianMean(classes[MOISTURE_WET]);

    const bool ascending = (dry < humid) && (humid < wet);
    const bool descending = (dry > humid) && (humid > wet);

    return ascending || descending;
}

int
moistureClassify(const GaussianStats classes[MOISTURE_CLASS_COUNT],
                 double value,
                 double minWeightPerClass,
                 double minSeparation,
                 double* confidence)
{
    if (confidence != nullptr) {
        *confidence = 0.0;
    }

    if (!isfinite(value) ||
        !moistureModelIsUsable(classes, minWeightPerClass, minSeparation)) {
        return MOISTURE_UNKNOWN;
    }

    double totalWeight = 0.0;
    for (int i = 0; i < MOISTURE_CLASS_COUNT; ++i) {
        totalWeight += classes[i].weight;
    }
    if (totalWeight <= 0.0) {
        return MOISTURE_UNKNOWN;
    }

    // Log domain throughout: the likelihoods here differ by many orders of
    // magnitude, and computing them directly underflows to zero for every
    // class at once, which reads as a tie.
    double logPosterior[MOISTURE_CLASS_COUNT];
    double best = 0.0;
    int bestClass = MOISTURE_UNKNOWN;

    for (int i = 0; i < MOISTURE_CLASS_COUNT; ++i) {
        const double prior = classes[i].weight / totalWeight;
        const double mean = gaussianMean(classes[i]);
        const double variance = gaussianVariance(classes[i]);
        const double delta = value - mean;

        logPosterior[i] = log(prior) - 0.5 * log(2.0 * M_PI * variance) -
                          (delta * delta) / (2.0 * variance);

        if (bestClass == MOISTURE_UNKNOWN || logPosterior[i] > best) {
            best = logPosterior[i];
            bestClass = i;
        }
    }

    if (confidence != nullptr) {
        // Softmax normalised against the winner, so the largest exponent is
        // exp(0) = 1 and nothing overflows.
        double sum = 0.0;
        for (int i = 0; i < MOISTURE_CLASS_COUNT; ++i) {
            sum += exp(logPosterior[i] - best);
        }
        *confidence = (sum > 0.0) ? (1.0 / sum) : 0.0;
    }

    return bestClass;
}

double
moistureZScore(const GaussianStats& stats, double value)
{
    if (stats.weight <= 0.0 || !isfinite(value)) {
        return 0.0;
    }
    const double sigma = sqrt(gaussianVariance(stats));
    if (sigma <= 0.0) {
        return 0.0;
    }
    return fabs(value - gaussianMean(stats)) / sigma;
}

const char*
moistureClassName(int cls)
{
    switch (cls) {
        case MOISTURE_DRY:
            return "Dry";
        case MOISTURE_HUMID:
            return "Humid";
        case MOISTURE_WET:
            return "Wet";
        default:
            return "";
    }
}

// ---------------------------------------------------------------------------
// Absorption
// ---------------------------------------------------------------------------

double
moistureTimeConstant(const float* values,
                     const uint16_t* dtSec,
                     unsigned count,
                     unsigned minSamples,
                     double minRise)
{
    if (values == nullptr || dtSec == nullptr || count < minSamples ||
        count < 2) {
        return 0.0;
    }

    const double baseline = values[0];

    // The plateau is the mean of the last third rather than the final sample:
    // one noisy reading at the end would set the target for the whole estimate.
    const unsigned tailFrom = count - (count / 3 > 0 ? count / 3 : 1);
    double tail = 0.0;
    unsigned tailCount = 0;
    for (unsigned i = tailFrom; i < count; ++i) {
        if (isfinite(values[i])) {
            tail += values[i];
            ++tailCount;
        }
    }
    if (tailCount == 0 || !isfinite(baseline)) {
        return 0.0;
    }

    const double plateau = tail / tailCount;
    const double rise = plateau - baseline;
    if (fabs(rise) < minRise) {
        return 0.0; // the probe did not respond; nothing to fit
    }

    const double target = baseline + 0.632 * rise;

    for (unsigned i = 1; i < count; ++i) {
        if (!isfinite(values[i])) {
            continue;
        }

        // Crossing in the direction the rise actually went, so an inverted
        // conversion works without a sign convention anywhere.
        const bool crossed =
          (rise > 0.0) ? (values[i] >= target) : (values[i] <= target);
        if (!crossed) {
            continue;
        }

        // Interpolate between the straddling samples: at a 60 s history period
        // the raw crossing is quantised to a whole minute, which on a probe
        // that answers in three is a 30 % error.
        const double previous = values[i - 1];
        const double span = values[i] - previous;
        double fraction = 0.0;
        if (fabs(span) > 1e-9) {
            fraction = (target - previous) / span;
            if (fraction < 0.0) {
                fraction = 0.0;
            } else if (fraction > 1.0) {
                fraction = 1.0;
            }
        }

        const double tau = (double)dtSec[i - 1] +
                           fraction * ((double)dtSec[i] - (double)dtSec[i - 1]);
        return (tau > 0.0) ? tau : 0.0;
    }

    return 0.0; // never reached its own 63 % level within the window
}

double
moistureAbsorptionConfidence(double dtSec, double tauSec)
{
    if (!(tauSec > 0.0) || dtSec <= 0.0) {
        return 0.0;
    }
    const double progressed = 1.0 - exp(-dtSec / tauSec);
    if (progressed < 0.0) {
        return 0.0;
    }
    return (progressed > 1.0) ? 1.0 : progressed;
}
