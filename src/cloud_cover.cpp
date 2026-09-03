#include "core/cloud_cover.h"

#include <math.h>

const char*
cloudStateName(int state)
{
    switch (state) {
        case CLOUD_OVERCAST:
            return "overcast";
        case CLOUD_PARTLY:
            return "partly cloudy";
        case CLOUD_CLEAR:
            return "clear";
        default:
            // Deliberately empty, like moistureState() on an uncalibrated
            // probe. A badge reading "unknown" on every night hour trains the
            // eye to ignore the field.
            return "";
    }
}

static bool
insideWindow(const CloudModelParams& params, int minuteOfDay)
{
    return params.clearSky != nullptr && params.bins > 0 &&
           minuteOfDay >= (int)params.firstMinute &&
           minuteOfDay <= (int)params.lastMinute;
}

float
cloudClearSky(const CloudModelParams& params, int minuteOfDay)
{
    if (!insideWindow(params, minuteOfDay)) {
        return 0.0f;
    }

    // Bin b represents the reading at its CENTRE, b * binMinutes +
    // binMinutes / 2. Anything else puts the fitted quantile half a bin away
    // from the time it describes, which on the morning ramp is a systematic
    // error in one direction all the way up.
    const float half = params.binMinutes * 0.5f;
    const float x = ((float)minuteOfDay - half) / (float)params.binMinutes;

    int lower = (int)floorf(x);
    const float frac = x - (float)lower;

    // The table is a full day, so the ends wrap rather than clamp. Clamping
    // would flatten the first and last half-bin of the day; wrapping is also
    // simply true — 23:55 is five minutes from 00:00.
    const int n = (int)params.bins;
    lower = ((lower % n) + n) % n;
    const int upper = (lower + 1) % n;

    const float a = (float)params.clearSky[lower] / 100.0f;
    const float b = (float)params.clearSky[upper] / 100.0f;
    return a + (b - a) * frac;
}

float
cloudClearness(const CloudModelParams& params, int minuteOfDay, float measured)
{
    const float reference = cloudClearSky(params, minuteOfDay);
    if (reference <= 0.0f) {
        return -1.0f;
    }
    return measured / reference;
}

int
cloudClassify(const CloudModelParams& params, float k)
{
    if (k >= params.clearAbove) {
        return CLOUD_CLEAR;
    }
    if (k < params.overcastBelow) {
        return CLOUD_OVERCAST;
    }
    return CLOUD_PARTLY;
}

int
cloudStateWithHysteresis(const CloudModelParams& params, int current, float k)
{
    if (current == CLOUD_UNKNOWN) {
        return cloudClassify(params, k);
    }

    // Widen whichever boundaries the CURRENT state would have to cross. Leaving
    // costs `stateBand`; the state being entered pays it too, because the same
    // widening moves both edges of the middle class outward.
    float lower = params.overcastBelow;
    float upper = params.clearAbove;
    if (current == CLOUD_OVERCAST) {
        lower += params.stateBand;
    } else if (current == CLOUD_CLEAR) {
        upper -= params.stateBand;
    } else {
        lower -= params.stateBand;
        upper += params.stateBand;
    }

    if (k >= upper) {
        return CLOUD_CLEAR;
    }
    if (k < lower) {
        return CLOUD_OVERCAST;
    }
    return CLOUD_PARTLY;
}

void
cloudModelReset(CloudModel& model)
{
    model.smoothK = 0.0f;
    model.prevK = 0.0f;
    model.variability = 0.0f;
    model.state = CLOUD_UNKNOWN;
    model.candidate = CLOUD_UNKNOWN;
    model.candidateRun = 0;
    model.inTransient = false;
    model.transientRun = 0;
    model.transientBelow = 0;
    model.transientPeak = 0.0f;
    model.varSum = 0.0;
    model.varSamples = 0;
    model.hasSmoothK = false;
    model.hasPrevK = false;
}

// Closes the day without discarding what the periodic payload has not yet
// taken: varSum/varSamples belong to the publish period, not to the daylight
// window, and clearing them here would silently drop the last minutes of every
// afternoon from the stored series.
static int
endDay(CloudModel& model)
{
    int events = CLOUD_EVENT_NONE;

    if (model.inTransient) {
        events |= CLOUD_EVENT_TRANSIENT_ENDED;
    }
    if (model.state != CLOUD_UNKNOWN) {
        events |= CLOUD_EVENT_STATE;
    }

    model.state = CLOUD_UNKNOWN;
    model.candidate = CLOUD_UNKNOWN;
    model.candidateRun = 0;
    model.inTransient = false;
    model.transientRun = 0;
    model.transientBelow = 0;
    model.transientPeak = 0.0f;
    model.smoothK = 0.0f;
    model.prevK = 0.0f;
    model.variability = 0.0f;
    model.hasSmoothK = false;
    model.hasPrevK = false;

    return events;
}

int
cloudModelMinute(CloudModel& model,
                 const CloudModelParams& params,
                 int minuteOfDay,
                 float measured)
{
    const float k = cloudClearness(params, minuteOfDay, measured);
    if (k < 0.0f || !isfinite(k)) {
        return endDay(model);
    }

    int events = CLOUD_EVENT_NONE;

    // ---- variability, and the transient episode built on it ---------------
    //
    // |dk| and not |d(reading)|: dividing by the reference is what removes the
    // diurnal ramp. Measured on this device, the raw level spread cannot tell a
    // smooth sunrise ramp from a flickering sky at all — over five-minute
    // windows with a range of 5 points or more, ramps and flicker both sit at a
    // level sd of 3.2 — while the step spread separates them 1.1 against 3.6.
    // That is the same mistake probe_health.h records having made once.
    if (model.hasPrevK) {
        const float dk = fabsf(k - model.prevK);
        model.variability += params.varAlpha * (dk - model.variability);
        model.varSum += dk;
        model.varSamples++;

        if (!model.inTransient) {
            if (model.variability > params.transientEnter) {
                model.inTransient = true;
                model.transientRun = 1;
                model.transientBelow = 0;
                model.transientPeak = model.variability;
                events |= CLOUD_EVENT_TRANSIENT_BEGAN;
            }
        } else {
            model.transientRun++;
            if (model.variability > model.transientPeak) {
                model.transientPeak = model.variability;
            }
            if (model.variability < params.transientExit) {
                model.transientBelow++;
                if (model.transientBelow >= params.transientExitRun) {
                    model.inTransient = false;
                    events |= CLOUD_EVENT_TRANSIENT_ENDED;
                }
            } else {
                model.transientBelow = 0;
            }
        }
    }
    model.prevK = k;
    model.hasPrevK = true;

    // ---- state -----------------------------------------------------------
    if (!model.hasSmoothK) {
        model.smoothK = k;
        model.hasSmoothK = true;
    } else {
        model.smoothK += params.stateAlpha * (k - model.smoothK);
    }

    const int next = cloudStateWithHysteresis(params, model.state, model.smoothK);
    if (next == model.state) {
        model.candidate = CLOUD_UNKNOWN;
        model.candidateRun = 0;
    } else if (model.state == CLOUD_UNKNOWN) {
        // The first classified minute of the day is taken immediately. A dwell
        // here would only delay the answer by its own length every morning.
        model.state = next;
        model.candidate = CLOUD_UNKNOWN;
        model.candidateRun = 0;
        events |= CLOUD_EVENT_STATE;
    } else {
        if (next == model.candidate) {
            model.candidateRun++;
        } else {
            model.candidate = next;
            model.candidateRun = 1;
        }
        if (model.candidateRun >= params.stateDwell) {
            model.state = next;
            model.candidate = CLOUD_UNKNOWN;
            model.candidateRun = 0;
            events |= CLOUD_EVENT_STATE;
        }
    }

    return events;
}

float
cloudModelTakeVariability(CloudModel& model)
{
    if (model.varSamples == 0) {
        return -1.0f;
    }
    const float mean = (float)(model.varSum / (double)model.varSamples);
    model.varSum = 0.0;
    model.varSamples = 0;
    return mean;
}
