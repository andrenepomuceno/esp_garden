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
    // Identifies the physical probe this evidence came from: pin and feeding
    // relay. Deleting a probe in /devices.html shifts every later index down,
    // and without this the probe that moves into the slot inherits weeks of
    // another pot's Gaussians and reports a confident badge for soil the model
    // has never seen. A mismatch resets that probe's statistics.
    uint8_t sourcePin;
    int8_t sourceRelay;

    // The rest of the probe's identity: everything about it that, if changed,
    // means the stored Gaussians describe a different sensor.
    //
    // Pin and relay alone are not enough. Swapping a capacitive module for a
    // resistive one leaves both untouched — same hole in the pot, same pump —
    // while the transfer curve inverts and the span changes completely. The
    // model would carry weeks of the old sensor's evidence into the new one
    // and keep reporting a confident badge, and the separation gate would not
    // catch it because the bands stay well separated. They would just be the
    // wrong bands.
    //
    // `sourceTag` is a hash of moisture[i].kind, so relabelling a probe is
    // also the way to say "this is a different sensor now" when nothing else
    // about the wiring changed.
    uint16_t sourceTag;
    bool sourceInvert;

    GaussianStats classes[MOISTURE_CLASS_COUNT];
    uint32_t wateringEvents; // cumulative, decayed like the statistics
    bool usable;             // passed the weight, ordering and separation gates
    float separation;        // Fisher's J between dry and wet

    // Mean rise from the dry window to the wet one, decayed across runs. The
    // cheapest evidence that this probe is in the pot its pump waters: a
    // response that stays near zero means it is not, or is not connected, or
    // the pump is not running. Free to compute, because the labels the model
    // already needs are exactly what it is made of.
    float response;

    // The soil's absorption time constant, in seconds, measured from this
    // probe's own rises after its pump: the 63.2 % crossing of a first-order
    // response, decayed across runs like everything else here.
    //
    // It exists because a fixed five-minute ramp was a guess. Water reaching a
    // probe is diffusion through soil, and how long that takes depends on the
    // soil, the pot and how well the probe touches either — which is to say it
    // is a property of THIS probe and cannot be a constant shared by four of
    // them. Until one is measured the fixed ramp stands in; 0 means exactly
    // that, and never "instant".
    float tauSec;
};

struct MoistureModelState
{
    MoistureProbeModel probe[MOISTURE_MAX];

    // Epoch of the newest watering event already folded in. Records at or
    // before it are skipped on the next run.
    //
    // Without it the whole design is wrong: the buffer holds 24 h and every run
    // re-scanned all of it, so the decay factor and all three gates — which are
    // sized for one run per 24 h of NEW history — were fed the same evidence
    // repeatedly. A device power-cycled six times in a day trains six times
    // over one buffer and crosses the six-event gate on a single real watering.
    uint32_t consumedUntil;

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
