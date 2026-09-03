#include "core/cloud_model.h"

#include "core/clear_sky_table.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/sensors.h"
#include "core/tasks.h"

#include <time.h>

static CloudModel g_model;

// The minute bucket. The model is fed ONE mean per minute, not one sample per
// second, because that is the quantity the thresholds were fitted on: while
// mqtt.publishSec was 60 the archived `luminosity` series was exactly the mean
// of each minute's 1 Hz samples, so nothing about the scale has to be assumed.
static double g_bucketSum = 0.0;
static uint32_t g_bucketCount = 0;
static int g_bucketMinute = -1;

static portMUX_TYPE g_cloudMux = portMUX_INITIALIZER_UNLOCKED;
static CloudReport g_report = { CLOUD_UNKNOWN, -1.0f, 0.0f, 0.0f,
                                false,        0,     0.0f, 0 };

static bool
cloudActive()
{
    return config.cloudEnabled && config.luminosityFitted;
}

void
cloudModelSetup()
{
    cloudModelReset(g_model);
    g_bucketSum = 0.0;
    g_bucketCount = 0;
    g_bucketMinute = -1;

    portENTER_CRITICAL(&g_cloudMux);
    g_report = { CLOUD_UNKNOWN, -1.0f, 0.0f, 0.0f, false, 0, 0.0f, 0 };
    portEXIT_CRITICAL(&g_cloudMux);

    if (!config.cloudEnabled) {
        return;
    }
    if (!config.luminosityFitted) {
        logger.warning("Cloud model enabled but no luminosity sensor declared.");
        return;
    }

    logger.info("Cloud model: clear-sky reference " +
                String(g_cloudParams.bins) + " x " +
                String(g_cloudParams.binMinutes) + " min, daylight " +
                String(g_cloudParams.firstMinute / 60) + ":" +
                String(g_cloudParams.firstMinute % 60) + "-" +
                String(g_cloudParams.lastMinute / 60) + ":" +
                String(g_cloudParams.lastMinute % 60) + " local.");
}

// Local minute of the day, or -1 while the clock is unset.
//
// Every threshold in the table is indexed by LOCAL time, so an unsynchronised
// clock must produce no answer rather than an answer against 1970. The same
// g_safeTimestamp the schedules use decides it — one definition of "the clock
// is believable", not two.
static int
localMinute()
{
    const time_t now = time(NULL);
    if (now < g_safeTimestamp) {
        return -1;
    }
    struct tm local;
    if (!localtime_r(&now, &local)) {
        return -1;
    }
    return local.tm_hour * 60 + local.tm_min;
}

int
cloudModelTick()
{
    if (!cloudActive()) {
        return CLOUD_EVENT_NONE;
    }

    const int minute = localMinute();
    if (minute < 0) {
        return CLOUD_EVENT_NONE;
    }

    // Only readings taken this minute go into this minute's mean; the io task
    // is where g_luminosity was just fed, so this runs on the same thread that
    // wrote it.
    if (g_luminosity.getSamples() > 0) {
        if (g_bucketMinute < 0) {
            g_bucketMinute = minute;
        }
        if (minute == g_bucketMinute) {
            g_bucketSum += (double)g_luminosity.getLast();
            g_bucketCount++;
            return CLOUD_EVENT_NONE;
        }
    } else if (g_bucketCount == 0) {
        return CLOUD_EVENT_NONE;
    }

    // The minute rolled over. A bucket with no samples in it — the luminosity
    // task stalled, or the clock jumped — is dropped rather than folded in as a
    // zero, which would read as full darkness at noon.
    int events = CLOUD_EVENT_NONE;
    if (g_bucketCount > 0) {
        const float mean = (float)(g_bucketSum / (double)g_bucketCount);
        events = cloudModelMinute(g_model, g_cloudParams, g_bucketMinute, mean);

        const float reference = cloudClearSky(g_cloudParams, g_bucketMinute);
        const float k =
          cloudClearness(g_cloudParams, g_bucketMinute, mean);

        portENTER_CRITICAL(&g_cloudMux);
        g_report.state = g_model.state;
        g_report.clearness = k;
        g_report.reference = reference;
        g_report.variability = g_model.variability;
        g_report.inTransient = g_model.inTransient;
        g_report.transientMinutes = g_model.transientRun;
        g_report.transientPeak = g_model.transientPeak;
        g_report.minutes++;
        portEXIT_CRITICAL(&g_cloudMux);
    }

    g_bucketMinute = minute;
    g_bucketSum = (g_luminosity.getSamples() > 0) ? (double)g_luminosity.getLast() : 0.0;
    g_bucketCount = (g_luminosity.getSamples() > 0) ? 1 : 0;
    return events;
}

CloudReport
cloudReport()
{
    portENTER_CRITICAL(&g_cloudMux);
    const CloudReport copy = g_report;
    portEXIT_CRITICAL(&g_cloudMux);
    return copy;
}

float
cloudTakeVariability()
{
    if (!cloudActive()) {
        return -1.0f;
    }
    return cloudModelTakeVariability(g_model);
}
