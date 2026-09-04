#include "core/et0_model.h"

#include "core/config.h"
#include "core/logger.h"
#include "core/sensors.h"
#include "core/tasks.h"

#include <time.h>

static Et0Day g_day;

static portMUX_TYPE g_et0Mux = portMUX_INITIALIZER_UNLOCKED;
static Et0Report g_report = { false, 0.0, 0.0f, 0.0f, 0.0f, 0, 0, 0, 0, 0 };

static bool
et0Active()
{
    return config.et0Enabled && config.dhtFitted;
}

void
et0ModelSetup()
{
    et0DayReset(g_day);

    portENTER_CRITICAL(&g_et0Mux);
    g_report = { false, 0.0, 0.0f, 0.0f, 0.0f, 0, 0, 0, 0, 0 };
    portEXIT_CRITICAL(&g_et0Mux);

    if (!config.et0Enabled) {
        return;
    }
    if (!config.dhtFitted) {
        logger.warning("ET0 enabled but no DHT declared; nothing to measure.");
        return;
    }

    logger.info("ET0: Hargreaves-Samani at latitude " +
                String(config.et0Latitude, 4) + ", scale " +
                String(config.et0Scale, 3) + ", needs " +
                String((int)kEt0MinHoursCovered) + " of 24 h a day.");
    if (config.et0Scale != 1.0f) {
        // Loud on purpose. The scale exists so a measured device can report a
        // usable number, but a value far from 1.0 is a siting fault wearing a
        // calibration coefficient, and the log is the only place that says so
        // to whoever reads the estimate later.
        logger.warning("ET0 scale is not 1.0 - the estimate is being corrected "
                       "for this sensor's exposure, not for physics.");
    }
}

// Local day key and calendar fields, or false while the clock is unset.
//
// The key is tm_year * 366 + tm_yday: monotonic across a year boundary, unique
// per local day, and free of any timezone arithmetic of our own - localtime_r
// has already applied the zone that config.timezone put in the environment.
// The same g_safeTimestamp the schedules and the cloud model use decides
// whether the clock is believable, so there is one definition of that and not
// three.
static bool
localDay(int32_t& key, int& dayOfYear, int& hourOfDay)
{
    const time_t now = time(NULL);
    if (now < g_safeTimestamp) {
        return false;
    }
    struct tm local;
    if (!localtime_r(&now, &local)) {
        return false;
    }
    key = (int32_t)local.tm_year * 366 + (int32_t)local.tm_yday;
    dayOfYear = local.tm_yday + 1; // tm_yday is 0-based; FAO-56 J is 1-based
    hourOfDay = local.tm_hour;
    return true;
}

bool
et0ModelTick()
{
    if (!et0Active()) {
        return false;
    }

    // Never fed: the DHT has not produced a reading yet, and folding a zero in
    // would put 0 C into the day's minimum for ever.
    if (g_temperature.getSamples() == 0) {
        return false;
    }

    int32_t key = 0;
    int dayOfYear = 0;
    int hourOfDay = 0;
    if (!localDay(key, dayOfYear, hourOfDay)) {
        return false;
    }

    // Same task, same thread as the DHT task that writes this accumulator.
    const float temperature = g_temperature.getLast();

    const Et0Result result = et0DayFold(
      g_day, key, dayOfYear, hourOfDay, temperature, (double)config.et0Latitude);

    const uint8_t soFar = et0HoursCovered(g_day);

    if (!result.closed) {
        portENTER_CRITICAL(&g_et0Mux);
        g_report.hoursSoFar = soFar;
        portEXIT_CRITICAL(&g_et0Mux);
        return false;
    }

    if (!result.valid) {
        // A day that closed without being watched. Counted and logged rather
        // than silently skipped: a device rebooting daily would otherwise show
        // an ET0 that simply stopped updating, with nothing saying why.
        portENTER_CRITICAL(&g_et0Mux);
        g_report.hoursCovered = result.hoursCovered;
        g_report.hoursSoFar = soFar;
        g_report.daysRefused++;
        portEXIT_CRITICAL(&g_et0Mux);
        logger.warning("ET0: day closed with only " +
                       String((int)result.hoursCovered) + " of 24 h covered; "
                       "no estimate for it.");
        return false;
    }

    portENTER_CRITICAL(&g_et0Mux);
    g_report.valid = true;
    g_report.et0Mm = result.et0Mm * (double)config.et0Scale;
    g_report.tMin = result.tMin;
    g_report.tMax = result.tMax;
    g_report.rangeK = result.rangeK;
    g_report.dayOfYear = result.dayOfYear;
    g_report.hoursCovered = result.hoursCovered;
    g_report.hoursSoFar = soFar;
    g_report.daysComputed++;
    portEXIT_CRITICAL(&g_et0Mux);
    return true;
}

Et0Report
et0Report()
{
    portENTER_CRITICAL(&g_et0Mux);
    const Et0Report copy = g_report;
    portEXIT_CRITICAL(&g_et0Mux);
    return copy;
}
