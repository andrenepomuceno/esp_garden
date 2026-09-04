#include "core/evapotranspiration.h"

#include <math.h>

static const double kPi = 3.14159265358979323846;

double
et0ExtraterrestrialRadiation(double latitudeDeg, int dayOfYear)
{
    if (!isfinite(latitudeDeg) || dayOfYear < 1 || dayOfYear > 366) {
        return 0.0;
    }

    const double phi = latitudeDeg * kPi / 180.0;

    // Inverse relative earth-sun distance and solar declination, FAO-56 eq. 23
    // and 24. Both are functions of the day of year alone.
    const double dr = 1.0 + 0.033 * cos(2.0 * kPi * (double)dayOfYear / 365.0);
    const double dec = 0.409 * sin(2.0 * kPi * (double)dayOfYear / 365.0 - 1.39);

    // Sunset hour angle, eq. 25. Inside the polar circles this is an arccos of
    // a value in range; outside them the sun does not set (argument <= -1, the
    // whole 24 h is daylight) or does not rise (argument >= 1). Clamping is the
    // physical answer, not a guard against bad input.
    const double x = -tan(phi) * tan(dec);
    double ws;
    if (x <= -1.0) {
        ws = kPi;
    } else if (x >= 1.0) {
        ws = 0.0;
    } else {
        ws = acos(x);
    }

    const double ra = (24.0 * 60.0 / kPi) * kEt0SolarConstant * dr *
                      (ws * sin(phi) * sin(dec) +
                       cos(phi) * cos(dec) * sin(ws));
    return (ra > 0.0) ? ra : 0.0;
}

double
et0Hargreaves(double tMinC, double tMaxC, double raMjPerDay)
{
    if (!isfinite(tMinC) || !isfinite(tMaxC) || !isfinite(raMjPerDay)) {
        return 0.0;
    }
    const double range = tMaxC - tMinC;
    if (range <= 0.0 || raMjPerDay <= 0.0) {
        // A day with no diurnal range has no radiation proxy, so the formula
        // has nothing to work from. Zero is what it evaluates to and there is
        // no more honest number available.
        return 0.0;
    }
    const double tMean = 0.5 * (tMinC + tMaxC);
    const double et0 = 0.0023 * (tMean + 17.8) * sqrt(range) * raMjPerDay /
                       kEt0LatentHeat;
    return (et0 > 0.0) ? et0 : 0.0;
}

void
et0DayReset(Et0Day& day)
{
    day.dayKey = -1;
    day.dayOfYear = 0;
    day.tMin = 0.0f;
    day.tMax = 0.0f;
    day.hourMask = 0;
    day.samples = 0;
}

uint8_t
et0HoursCovered(const Et0Day& day)
{
    uint8_t count = 0;
    for (int h = 0; h < 24; ++h) {
        if (day.hourMask & (1u << h)) {
            ++count;
        }
    }
    return count;
}

// Starts `day` on a new local day, holding this one reading.
static void
openDay(Et0Day& day, int32_t dayKey, int dayOfYear, int hourOfDay, float t)
{
    day.dayKey = dayKey;
    day.dayOfYear = (int16_t)dayOfYear;
    day.tMin = t;
    day.tMax = t;
    day.hourMask = (hourOfDay >= 0 && hourOfDay < 24) ? (1u << hourOfDay) : 0u;
    day.samples = 1;
}

Et0Result
et0DayFold(Et0Day& day,
           int32_t dayKey,
           int dayOfYear,
           int hourOfDay,
           float temperatureC,
           double latitudeDeg)
{
    Et0Result out = { false, false, 0.0, 0.0f, 0.0f, 0.0f, 0, 0, 0 };

    if (!isfinite(temperatureC)) {
        return out;
    }

    if (day.dayKey < 0) {
        openDay(day, dayKey, dayOfYear, hourOfDay, temperatureC);
        return out;
    }

    if (dayKey == day.dayKey) {
        if (temperatureC < day.tMin) {
            day.tMin = temperatureC;
        }
        if (temperatureC > day.tMax) {
            day.tMax = temperatureC;
        }
        if (hourOfDay >= 0 && hourOfDay < 24) {
            day.hourMask |= (1u << hourOfDay);
        }
        ++day.samples;
        return out;
    }

    if (dayKey < day.dayKey) {
        // The clock moved backwards - an NTP correction, most likely. The day
        // in progress was accumulating against a timeline that turned out not
        // to exist, so it is discarded rather than reported.
        openDay(day, dayKey, dayOfYear, hourOfDay, temperatureC);
        return out;
    }

    // The day rolled over. Close the old one before opening the new.
    out.closed = true;
    out.tMin = day.tMin;
    out.tMax = day.tMax;
    out.rangeK = day.tMax - day.tMin;
    out.dayOfYear = day.dayOfYear;
    out.hoursCovered = et0HoursCovered(day);
    out.samples = day.samples;
    out.valid = out.hoursCovered >= kEt0MinHoursCovered;
    if (out.valid) {
        const double ra =
          et0ExtraterrestrialRadiation(latitudeDeg, day.dayOfYear);
        out.et0Mm = et0Hargreaves(day.tMin, day.tMax, ra);
    }

    openDay(day, dayKey, dayOfYear, hourOfDay, temperatureC);
    return out;
}
