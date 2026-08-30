#pragma once

#include <stdint.h>

// Is there actually a sensor on this pin?
//
// WHY THE OBVIOUS STATISTICS DO NOT WORK
//
// Measured on this board with all three probes unplugged, 200 history records
// over 4.3 h:
//
//   probe 0   mean 87.2   sd 4.37   median step 1.00   176 distinct values
//   probe 1   mean 61.1   sd 2.65   median step 0.02    30 distinct values
//   probe 2   mean 50.2   sd 1.29   median step 0.03    96 distinct values
//
// Nothing there separates a floating pin from soil:
//
// - **Not the value.** All three sat mid-scale. An earlier measurement caught
//   a disconnected probe at 52.6 and, hours later, at 86.7. A rail would be
//   easy; mid-scale is indistinguishable from a real reading.
// - **Not the variance OF THOSE MEANS.** Probe 2 looks quieter than wet soil
//   at sd 1.29 — but that is the spread of per-minute AVERAGES, which is
//   precisely what averaging removes. Reading it as "the variance" was a
//   mistake this file made first. The WITHIN-window variance is a different
//   number entirely, and it separates decisively; see below.
// - **Not the correlation between probes.** Probes 0 and 1 track each other at
//   +0.87 here, but real probes in one garden — same schedule, same weather —
//   correlate just as hard. It cannot separate them.
//
// Those three are why this file exists at all: every passive summary of the
// reading is a description of a number that is already plausible.
//
// WHAT WORKS FIRST: THE WITHIN-WINDOW VARIANCE
//
// Measured live on this board, all three probes unplugged at the connector,
// against the luminosity channel on the same ADC at the same moment:
//
//   Luminosidade  (CONNECTED)   var    0.02   sd  0.14
//   Umidade 1     (floating)    var  ~800     sd 27.9
//   Umidade 2     (floating)    var ~1990     sd 44.6
//   Umidade 3     (floating)    var ~1980     sd 44.5
//
// The probes swing 0.00 to 100.00 within seconds. Five orders of magnitude,
// with a control on the same chip. Soil cannot do it — water content does not
// move forty-five points in a minute — so a spread like that is not a reading.
//
// It is a ONE-SIDED test and is only ever used as one. High variance condemns.
// Low variance clears NOTHING: this same board has held a disconnected probe
// at variance 0.01, quieter than any of its connected ones. That asymmetry is
// the whole reason the impedance test below still exists.
//
// WHAT WORKS WHEN IT IS QUIET: SOURCE IMPEDANCE
//
// The ADC gives it away. An ESP32 conversion samples onto a small hold
// capacitor, and that capacitor still carries charge from the PREVIOUS
// channel. A real sensor — a divider, or a capacitive module's buffered output
// — is a stiff source: it recharges the cap well inside the sampling window,
// so two conversions in a row read the same. A floating pin cannot: the first
// conversion is dragged toward wherever the previous channel sat, and only the
// second is near the node's own potential.
//
// So read each probe TWICE and regress the difference on the step it was asked
// to make:
//
//     drive  = previous channel's reading - this probe's settled reading
//     delta  = first reading - second reading
//     slope  = d(delta)/d(drive)
//
// A stiff source gives slope 0. A floating node gives a positive slope. The
// null hypothesis is a NUMBER rather than a calibration, which is the whole
// point: it needs no healthy probe to compare against, and this garden has no
// healthy probe to offer.
//
// It is also free of new hardware, and the second reading is the better one —
// the settled one — so the measurement improves the value it diagnoses.
//
// WHAT IT CANNOT DO
//
// A resistive probe in dry soil is genuinely high impedance, so it will show a
// real slope. That is not a false positive to engineer away; it is the same
// physics. The slope threshold is therefore deliberately loose and the raw
// number is published either way, to be tightened once a probe known to be
// connected has reported one.
//
// The VARIANCE threshold needs no such apology: the luminosity channel is a
// connected control on the same ADC, and it sits four orders of magnitude
// below the floating probes.

// A short of the pin to a rail, which IS distinguishable and is a different
// failure from the floating one above.
static const int PROBE_RAIL_LOW = 8;     // ADC counts
static const int PROBE_RAIL_HIGH = 4087; // 4095 - 8

struct ProbeHealth
{
    // Incremental least squares of `delta` on `drive`. Sums rather than
    // samples, so this is O(1) per reading and allocates nothing — the same
    // rule AccumulatorV2 is held to.
    double n;
    double sumX, sumY, sumXX, sumXY, sumYY;

    // Everything below is about the settled reading, not the settling.
    uint32_t samples;
    uint32_t railLow;
    uint32_t railHigh;
    int32_t minRaw;
    int32_t maxRaw;

    // Sum and sum of squares of the settled reading, for the within-window
    // spread. In ADC counts, so the threshold is stated in the units the
    // hardware actually produces rather than in a percentage that a per-probe
    // polarity or calibration could move.
    double sumV;
    double sumVV;
};

void probeHealthReset(ProbeHealth& health);

// `previousRaw` is the raw count the ADC returned for whatever channel was read
// immediately before this probe; `first` and `second` are two back-to-back
// conversions of the probe itself.
void probeHealthAdd(ProbeHealth& health, int previousRaw, int first, int second);

// Ages the evidence without changing the estimate, exactly as gaussianDecay()
// does for the classifier: a probe that is unplugged today should not be
// exonerated by a week of being fine, and one plugged back in should not carry
// its accusation forever.
void probeHealthDecay(ProbeHealth& health, double factor);

// The regression slope, or 0 when there is not enough spread in `drive` to
// estimate one. Zero here means "no evidence", never "healthy" — read
// probeHealthSamples() to tell those apart.
double probeHealthSlope(const ProbeHealth& health);

// Standard error of that slope. A slope without one is a number, not a finding.
double probeHealthSlopeStdErr(const ProbeHealth& health);

// slope / standard error. The statistic the verdict is actually made on, and
// the one worth publishing: a large slope measured badly and a small one
// measured well look identical until you divide them.
double probeHealthT(const ProbeHealth& health);

enum ProbeVerdict
{
    PROBE_UNKNOWN = 0,  ///< not enough evidence yet
    PROBE_CONNECTED,    ///< nothing found: stiff, plausible, and moving
    PROBE_NOISY,        ///< swinging further and faster than soil physically can
    PROBE_FLOATING,     ///< high source impedance; most likely nothing attached
    PROBE_RAILED,       ///< pinned at 0 or full scale: shorted, or no divider
    PROBE_STUCK         ///< has not moved at all over the whole window
};

// Standard deviation of the settled reading, in ADC counts.
double probeHealthSd(const ProbeHealth& health);

// `minSamples` guards against judging a probe on a handful of readings;
// `minSlope` and `minT` are the effect size and the confidence the settling
// test has to reach before it accuses anything.
// `maxSd` is the spread, in ADC counts, above which the signal stops being
// something soil could produce. Measured on this board: a connected channel
// holds about 6 counts, a floating one 1100-1800. Everything between is a wide
// and empty margin.
int probeHealthVerdict(const ProbeHealth& health,
                       uint32_t minSamples,
                       double maxSd,
                       double minSlope,
                       double minT);

const char* probeVerdictName(int verdict);
