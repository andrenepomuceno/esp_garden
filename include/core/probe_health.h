#pragma once

#include <stdint.h>

// Is there a sensor on this pin, or is the pin just floating?
//
// ONE test, deliberately. Earlier versions of this file also tried to name a
// shorted pin, a dead-but-still-driving module, and a quiet floating one by its
// source impedance. All three were removed after they accused working hardware:
//
//   Umidade Zona 1   railed     - healthy, simply lifted out of the soil
//   Umidade Zona 2   railed     - a real fault, pin tied to 3V3
//   Umidade Zona 3   floating   - healthy, sitting in wet soil
//
// Two false accusations out of three readings. A capacitive probe in air reads
// full scale and is genuinely indistinguishable from a pin shorted to 3V3 —
// there is nothing there for any statistic to separate — and the impedance
// regression crossed its threshold on a probe that was working. A detector that
// cries wolf on a good sensor is worse than one that occasionally stays quiet,
// so what is left is the single check that has never been wrong on this
// hardware.
//
// WHAT THE ONE TEST IS
//
// A disconnected pin swings between CONSECUTIVE conversions. Measured live,
// three probes unplugged at the connector, against the luminosity channel on
// the same ADC at the same moment:
//
//   Luminosidade  (CONNECTED)   var    0.02   sd  0.14
//   Umidade 1     (floating)    var  ~800     sd 27.9
//   Umidade 2     (floating)    var ~1990     sd 44.6
//   Umidade 3     (floating)    var ~1980     sd 44.5
//
// Four orders of magnitude, with a control on the same chip.
//
// The statistic is the SAMPLE-TO-SAMPLE difference, not the spread of the
// level, and that distinction is another false accusation this made on real
// hardware: a healthy probe lifted from wet soil into the air went 287 -> 4095
// counts inside the window and was reported noisy at sd 1889. A WATERING does
// the same thing more gently, so the most important event in the system was on
// course to look like a fault. Soil cannot jump between consecutive
// conversions however fast it is watered; a floating pin does nothing else.
//
// The same 400-count threshold separates every case measured or modelled:
//
//   watering, 1200 counts over 5 min at 1 Hz        4
//   connected probe, level sd 80 (white noise)    113
//   probe moved from soil to air by hand          204
//   ---- threshold ----                           400
//   floating pin alternating rail to rail        3910
//
// A single large step contributes S/sqrt(N), which is why moving a probe by
// hand stays well under the line while continuous swinging does not.
//
// WHAT IT DELIBERATELY DOES NOT CATCH
//
// A disconnected pin that happens to sit quiet. This board has held one at
// variance 0.01, quieter than any of its connected probes, and nothing here
// would flag it. That is the price of not accusing working sensors. Such a
// probe is still caught by the classifier, days later, through its failure to
// answer its own pump — the watering-response check and the separation gate.

struct ProbeHealth
{
    // Sum and sum of squares of the difference between consecutive readings,
    // in ADC counts. Sums rather than samples, so this is O(1) per reading and
    // allocates nothing — the rule AccumulatorV2 is held to.
    double n;
    double sumD;
    double sumDD;
    int32_t prevRaw;
    bool hasPrev;
    uint32_t samples;
};

void probeHealthReset(ProbeHealth& health);

// `settled` is this probe's reading, in raw ADC counts.
void probeHealthAdd(ProbeHealth& health, int settled);

// Ages the evidence without changing the estimate, as gaussianDecay() does for
// the classifier: a probe unplugged today should not be exonerated by a week of
// being fine, and one plugged back in should not carry its accusation forever.
void probeHealthDecay(ProbeHealth& health, double factor);

// Standard deviation of the difference between consecutive readings, in ADC
// counts. See above for why this and not the spread of the level.
double probeHealthStepSd(const ProbeHealth& health);

enum ProbeVerdict
{
    PROBE_UNKNOWN = 0, ///< too few readings yet; NOT a claim of health
    PROBE_CONNECTED,   ///< nothing found
    PROBE_NOISY        ///< jumping further between readings than soil can
};

int probeHealthVerdict(const ProbeHealth& health,
                       uint32_t minSamples,
                       double maxStepSd);

const char* probeVerdictName(int verdict);
