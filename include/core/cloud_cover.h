#pragma once

#include <stdint.h>

// Cloud cover from one luminosity channel: a state, and a transient episode.
//
// No Arduino, no clock, no filesystem — this is the part that decides what a
// reading means, so it is the part that gets host tests. The Arduino half
// (bucketing 1 Hz samples into minutes, reading the local clock, publishing)
// lives in src/cloud_model.cpp, exactly as moisture_classifier.cpp is split
// from moisture_model.cpp.
//
// WHY THE CLEAR-SKY REFERENCE IS EMPIRICAL AND NOT ASTRONOMICAL
//
// This device's luminosity peaks at about 15:00 local, roughly three hours
// after solar noon, and its mornings are attenuated by a factor of three to
// five relative to its afternoons. Measured, not assumed: over six days the
// ratio of each hour's maximum to the same hour on a reference day rises
// monotonically from 0.11-0.25 at 06:00 to 0.79-1.02 at 15:00, with the SAME
// shape on days whose weather had nothing in common (30 % RH and 70 % RH, clear
// afternoons and cloudy ones). Weather does not repeat a time-of-day-locked
// profile across six unrelated days. That is the mounting: something shades the
// sensor in the morning.
//
// A solar-position model would call all of that cloud, every morning, forever.
// The reference therefore comes from the device's own history — an upper
// envelope per time of day — fitted by scripts/cloud_fit.py and emitted as
// include/core/clear_sky_table.h. The cost is that the table describes ONE
// sensor at ONE mounting: move the sensor, or run this firmware on another
// board, and the table is wrong. That is why config.cloud.enabled defaults to
// false.
//
// WHAT THE CLEARNESS INDEX IS HERE
//
// k = measured / reference(time of day). It is NOT an irradiance ratio. The
// sensor is an LDR read as a fraction of full scale, which is a compressive
// transform of irradiance, so the scale does not match the solar literature's
// K_t and its thresholds must not be borrowed from it. Measured on this
// device: four settled fine days sit at k p25..p95 = 0.83..1.00, while the one
// overcast day in the archive spent 42 % of its daylight below k = 0.50 where
// the three brightest days spent 0.3-3.9 %. Both thresholds below come from
// those two facts.

enum CloudState
{
    CLOUD_UNKNOWN = 0,  ///< outside the fitted daylight window, or no reading yet
    CLOUD_OVERCAST = 1,
    CLOUD_PARTLY = 2,
    CLOUD_CLEAR = 3
};

// "", "overcast", "partly cloudy" or "clear". CLOUD_UNKNOWN is the empty
// string, so a caller renders nothing rather than the word "unknown" — the
// same convention moistureState() uses for an uncalibrated probe.
const char*
cloudStateName(int state);

// The fitted model. One instance, `g_cloudParams`, is defined by the generated
// clear_sky_table.h; the struct is separate from it so a test can build its own.
struct CloudModelParams
{
    /// Clear-sky reference per time-of-day bin, in hundredths of a percent of
    /// full scale (the units sensorsReadIo() produces, x100). uint16_t because
    /// 0..10000 fits and 144 floats would be four times the flash for precision
    /// nothing downstream can use.
    const uint16_t* clearSky;
    uint16_t bins;       ///< entries in clearSky; bins * binMinutes == 1440
    uint16_t binMinutes; ///< width of one bin

    /// The fitted daylight window, as local minutes of the day, inclusive.
    /// Outside it the reference is too small for a ratio to mean anything and
    /// the model reports CLOUD_UNKNOWN rather than a number it cannot support.
    uint16_t firstMinute;
    uint16_t lastMinute;

    float overcastBelow; ///< k below this is overcast
    float clearAbove;    ///< k at or above this is clear
    float stateBand;     ///< hysteresis half-width on both boundaries
    float stateAlpha;    ///< EWMA weight applied to k before classifying
    uint16_t stateDwell; ///< consecutive minutes a new state must hold

    float varAlpha;            ///< EWMA weight on the per-minute |dk|
    float transientEnter;      ///< episode opens above this
    float transientExit;       ///< ...and closes below this
    uint16_t transientExitRun; ///< after this many consecutive minutes below
};

// The reference for a local minute of the day, in percent of full scale, or 0
// outside the fitted window.
//
// Linearly interpolated between BIN CENTRES, which is not decoration: the
// morning ramp climbs about 1.6 points a minute at its steepest, so treating a
// ten-minute bin as a constant would bias every reading in the bin by up to
// eight points — larger than the difference between two of the three states.
float
cloudClearSky(const CloudModelParams& params, int minuteOfDay);

// measured / reference. Negative when the minute is outside the fitted window,
// which is the caller's signal to report nothing at all.
float
cloudClearness(const CloudModelParams& params, int minuteOfDay, float measured);

// The bare threshold decision, without hysteresis.
int
cloudClassify(const CloudModelParams& params, float k);

// The decision a model already in `current` makes. Leaving a state costs
// `stateBand` past its boundary; entering the next one costs the same. Without
// it a k sitting on a threshold flaps, and every flap is a published datapoint.
int
cloudStateWithHysteresis(const CloudModelParams& params, int current, float k);

// Everything that carries across a minute.
struct CloudModel
{
    float smoothK;     ///< EWMA of k, what the state is classified from
    float prevK;       ///< previous minute's raw k, for |dk|
    float variability; ///< EWMA of |dk| — the transient detector's input

    int state;
    int candidate;         ///< a state waiting out its dwell
    uint16_t candidateRun; ///< how many minutes it has waited

    bool inTransient;
    uint16_t transientRun;   ///< minutes since the episode opened
    uint16_t transientBelow; ///< consecutive minutes under the exit threshold
    float transientPeak;     ///< highest `variability` inside the episode

    /// Sum and count of |dk| since the last take, for the periodic payload.
    /// This is the only quantity here that is a LEVEL rather than an event, so
    /// it is the only one that rides the periodic tick.
    double varSum;
    uint32_t varSamples;

    bool hasSmoothK;
    bool hasPrevK;
};

void
cloudModelReset(CloudModel& model);

enum CloudEvent
{
    CLOUD_EVENT_NONE = 0,
    CLOUD_EVENT_STATE = 1 << 0,           ///< the state changed
    CLOUD_EVENT_TRANSIENT_BEGAN = 1 << 1, ///< an episode opened
    CLOUD_EVENT_TRANSIENT_ENDED = 1 << 2  ///< ...and closed
};

// Feeds ONE minute's mean reading. Returns a bitmask of CloudEvent.
//
// A minute, and not a second, because that is the resolution the thresholds
// were fitted at: the archive this model was trained on holds the mean of the
// 1 Hz samples over each 60 s publish window, so the device reproduces exactly
// the quantity that was measured and no scaling has to be assumed. It is also
// the resolution at which a cloud edge is still one sample rather than sixty.
//
// Outside the fitted daylight window this resets the model and returns whatever
// closing an open episode implies. A night that carried a state across to the
// next morning would report last evening's sky as this morning's.
int
cloudModelMinute(CloudModel& model,
                 const CloudModelParams& params,
                 int minuteOfDay,
                 float measured);

// Mean |dk| since the last call, and clears the accumulator. Negative when no
// minute has been folded in since — which the caller must render as an absent
// key, not as a zero. Zero is a perfectly still sky.
float
cloudModelTakeVariability(CloudModel& model);
