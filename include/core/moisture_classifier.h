#pragma once

#include <stdint.h>

// Gaussian naive Bayes over ONE feature — the probe reading — with three
// classes. No Arduino, no SPIFFS, no floats-in-flash: this is the part that
// decides what a reading means, so it is the part that gets host tests.
//
// WHY THIS AND NOT CLUSTERING
//
// Clustering the history was tried and it does not work. Measured on 29.7 days
// / 20 417 samples: a linear drying trend alone explains 86.4 % of the
// variance, and with the trend removed BIC prefers k=2 with the second
// component sitting at -24.5 — the near-zero readings of a disconnected probe,
// not a soil state. Fitting three clusters to the raw series returns three
// groups that are the outliers plus two arbitrary slices of the trend, so the
// thresholds label "dry" whatever happens to come late in any drying period.
//
// The relay history is what changes the problem. A watering is an EVENT, and
// an event is a label: soil is wettest shortly after one and driest just
// before the next. That turns an unsupervised problem nobody can solve on this
// data into a weakly supervised one with a physical basis.
//
// The model is deliberately the simplest thing that fits: one Gaussian per
// class per probe. It needs only three numbers per class, which means training
// can STREAM over the history file instead of holding it in RAM, and it yields
// parameters a human can read and argue with rather than a threshold that
// appeared from nowhere.

enum MoistureClass
{
    MOISTURE_DRY = 0,
    MOISTURE_HUMID = 1,
    MOISTURE_WET = 2,
    MOISTURE_CLASS_COUNT = 3,
    MOISTURE_UNKNOWN = -1
};

// Sufficient statistics for one Gaussian. Everything the fit needs, in a form
// that can be accumulated one sample at a time and carried across reboots.
//
// `weight` rather than a count because samples are DECAYED between training
// runs: a day holds only one or two watering cycles per zone, far too few to
// fit anything, so evidence has to accumulate across weeks while still
// following a soil that changes. It is a real number for the same reason.
struct GaussianStats
{
    double weight; // sum of sample weights, i.e. an effective count
    double sum;    // sum of x * weight
    double sumSq;  // sum of x^2 * weight
};

void
gaussianReset(GaussianStats& stats);

void
gaussianAdd(GaussianStats& stats, double value, double weight = 1.0);

// Multiplies the evidence by `factor`, which ages it without discarding it.
// Applied once per training run, so `factor` is a per-day forgetting rate.
void
gaussianDecay(GaussianStats& stats, double factor);

double
gaussianMean(const GaussianStats& stats);

// Population variance from the sufficient statistics, floored: a class whose
// samples are all identical would otherwise have zero variance and an infinite
// log-likelihood, and the floor is set well below this sensor's noise.
double
gaussianVariance(const GaussianStats& stats);

extern const double g_gaussianVarianceFloor;

// Fisher's discriminant between the dry and wet classes:
//
//     J = (mu_wet - mu_dry)^2 / (var_wet + var_dry)
//
// The one number that says whether this model is worth believing. J = 4 means
// the two means are two pooled standard deviations apart. A probe watered so
// often that it never dries out, or one whose labels are noise, lands near 0 —
// and the right answer there is to report nothing rather than a badge.
double
moistureSeparation(const GaussianStats classes[MOISTURE_CLASS_COUNT]);

// True when the fit is worth using at all. Requires, per class, enough
// accumulated weight to have a meaning; requires the class means to be ORDERED
// dry < humid < wet (or the reverse — the conversion's polarity is not assumed,
// exactly as the two-point calibration does not assume it); and requires the
// separation to clear `minSeparation`.
//
// Ordering is not decoration. If humid does not sit between dry and wet then
// the labelling disagrees with the physics that produced the labels, and every
// classification from it is a coin toss with a confident-looking number
// attached.
bool
moistureModelIsUsable(const GaussianStats classes[MOISTURE_CLASS_COUNT],
                      double minWeightPerClass,
                      double minSeparation);

// Maximum a posteriori class for `value`, or MOISTURE_UNKNOWN when the model
// is not usable. `confidence`, when given, receives the winning posterior in
// [0, 1] — which is the honest way to show a classification that is nearly a
// tie between two bands.
int
moistureClassify(const GaussianStats classes[MOISTURE_CLASS_COUNT],
                 double value,
                 double minWeightPerClass,
                 double minSeparation,
                 double* confidence);

// Number of standard deviations `value` sits from that class's mean. Used to
// reject the readings a disconnected probe produces, which sit at a rail and
// would otherwise drag a class mean toward a value the soil never had.
double
moistureZScore(const GaussianStats& stats, double value);

// ---------------------------------------------------------------------------
// Absorption
// ---------------------------------------------------------------------------

// The time constant of a probe's response to a watering, in seconds, estimated
// from one wet window. Returns 0 when the samples do not describe a rise.
//
// Irrigation is not a step at the probe. Water infiltrates, and the reading
// follows a first-order approach to its new level:
//
//     m(t) = m_baseline + rise * (1 - exp(-(t - T) / tau))
//
// tau is where that curve reaches 63.2 % of the rise, and it is a PHYSICAL
// property of this probe in this pot — depth, soil, distance from the emitter.
// A probe answering in two minutes is shallow or sitting on the dripper; one
// answering in forty is deep or far. A tau that MOVES on its own says something
// physical changed: the probe was knocked, the soil compacted, the emitter
// clogged.
//
// `dtSec[i]` is the seconds since the watering for `values[i]`, ascending.
// Interpolates between the two samples straddling the threshold, because at a
// 60 s history period the raw crossing is quantised to a minute.
//
// Refuses rather than guesses:
//   - fewer than `minSamples` points
//   - a rise smaller than `minRise`, which is a probe that did not respond
//   - a curve that never crosses its own 63.2 % level
//
// Polarity is not assumed: a conversion where a wetter soil reads LOWER gives a
// negative rise and the same time constant.
double
moistureTimeConstant(const float* values,
                     const uint16_t* dtSec,
                     unsigned count,
                     unsigned minSamples,
                     double minRise);

// Confidence that a sample taken `dtSec` after a watering really is "wet",
// given the probe's time constant: 1 - exp(-dt / tau), i.e. how much of the
// response has actually happened.
//
// This weights the LABEL. It is deliberately not a classifier input — see the
// note in moisture_model.h about why time since watering must never become a
// feature.
double
moistureAbsorptionConfidence(double dtSec, double tauSec);

const char*
moistureClassName(int cls);
