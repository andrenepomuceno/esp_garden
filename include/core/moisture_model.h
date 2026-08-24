#pragma once

#include "BuildConfig.h"
#include "core/moisture_classifier.h"
#include <Arduino.h>

// Trains one Gaussian naive Bayes model per probe from the I/O history, using
// the relay record as weak labels, and persists it across reboots.
//
// THE LABELLING RULE, which is the whole design:
//
//   A watering is an event with a known effect. Within one watering cycle the
//   soil is wettest shortly AFTER the pump ran and driest just BEFORE the next
//   one — the second is not an assumption but arithmetic, since moisture
//   decreases monotonically between waterings. So:
//
//     WET    readings in (T, T + wetWindow]        after a watering
//     DRY    readings in [T - dryWindow, T)        before the next watering
//     HUMID  everything in between
//
//   That is what makes this tractable where clustering was not. The earlier
//   attempt is documented in moisture_classifier.h: a month of unlabelled
//   history is 86 % drying trend, and three clusters fitted to it are the
//   outliers plus two arbitrary slices of that trend.
//
// WHY EVIDENCE ACCUMULATES INSTEAD OF BEING REFITTED
//
//   The history buffer holds 24 h. One zone gets watered once or twice a day,
//   so a from-scratch daily fit would have one or two events — a description
//   of yesterday, not a model. Training therefore DECAYS the stored
//   sufficient statistics and adds the new day to them, so the model is built
//   from weeks of evidence while still following a soil that changes. It
//   reports nothing at all until enough has accumulated.

// Per-probe model plus the numbers needed to judge it. Everything here is
// surfaced by GET /moisture.json, because a classifier whose parameters cannot
// be inspected is a threshold that appeared from nowhere.
struct MoistureProbeModel
{
    GaussianStats classes[MOISTURE_CLASS_COUNT];
    uint32_t wateringEvents; // cumulative, decayed like the statistics
    bool usable;             // passed the weight, ordering and separation gates
    float separation;        // Fisher's J between dry and wet
};

struct MoistureModelState
{
    MoistureProbeModel probe[MOISTURE_MAX];
    uint32_t trainedAt;      // epoch of the last training run, 0 if never
    uint32_t recordsScanned; // from the last run only
    uint32_t samplesUsed;    // from the last run only
    uint32_t outliersDropped;
};

// Gates. A model that fails any of these reports MOISTURE_UNKNOWN rather than
// a badge, and /moisture.json says which gate it failed.
extern const double g_moistureMinWeightPerClass;
extern const double g_moistureMinSeparation;
extern const unsigned g_moistureMinEvents;

// Per-training-run forgetting factor applied before the new day is folded in.
// 0.93 gives evidence a half-life of about ten days: long enough to accumulate
// the watering events a single day cannot supply, short enough that a probe
// moved to a different pot stops being described by the old one within a
// fortnight.
extern const double g_moistureDecayPerRun;

// Loads the persisted state, or starts empty. Called from tasksSetup().
void
moistureModelSetup();

// One training run: decay, scan the history for watering cycles, label, fold
// the new samples in, re-evaluate the gates, persist. Called daily.
//
// Streams the history file one record at a time — the sufficient statistics
// are three numbers per class, so this never holds more than one record in RAM
// regardless of how large the buffer is.
void
moistureModelTrain();

// MAP class for `value` on probe `index`, or MOISTURE_UNKNOWN when that
// probe's model is not usable. `confidence` receives the winning posterior.
int
moistureModelClassify(unsigned index, double value, double* confidence);

const MoistureModelState&
moistureModelState();
