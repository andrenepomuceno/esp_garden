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
