#pragma once

#include <stdint.h>

// Reference evapotranspiration (ET0) by Hargreaves-Samani: how much water a
// short grass reference surface lost in a day, in millimetres.
//
// No Arduino, no clock, no filesystem - this is the part that decides what a
// day's temperatures mean, so it is the part that gets host tests. The Arduino
// half (the local clock, the day rollover, publishing) lives in
// src/et0_model.cpp, exactly as cloud_cover.cpp is split from cloud_model.cpp.
//
// ---------------------------------------------------------------------------
// WHY HARGREAVES-SAMANI AND NOT PENMAN-MONTEITH
// ---------------------------------------------------------------------------
// FAO-56 Penman-Monteith is the reference method and it needs wind speed and
// net radiation. This device has neither. Hargreaves-Samani needs only the
// day's temperature extremes and extraterrestrial radiation, and Ra is a
// closed form in latitude and day of year - a dozen lines of trigonometry, no
// table, no network. That is the whole reason it is the one implemented here:
// the firmware never calls a weather API, and this estimate stands or falls on
// what the board already measures.
//
// ---------------------------------------------------------------------------
// WHAT THE DIURNAL RANGE IS DOING IN THE FORMULA, AND WHY IT IS THE WEAK POINT
// ---------------------------------------------------------------------------
// Hargreaves' insight is that (Tmax - Tmin) is a proxy for how much sun reached
// the ground: a clear day heats hard and radiates away at night, an overcast
// one does neither. So sqrt(Tmax - Tmin) stands in for the solar radiation term
// nobody could measure cheaply in 1985.
//
// That proxy is only as good as the thermometer's exposure. A sensor behind
// thermal mass - in a box, under a roof, indoors - reports a compressed range
// that describes its enclosure rather than the sky, and no scale factor
// repairs that: it removes the bias and leaves the estimate uncorrelated with
// the thing it is estimating. `scripts/et0_fit.py` measures exactly this
// against a public station and says so in as many words, and on the archive it
// was written against it said so loudly. Read its output before believing a
// number this file produced.
//
// ---------------------------------------------------------------------------
// AND WHY A DAY MUST BE WATCHED BEFORE ITS EXTREMES MEAN ANYTHING
// ---------------------------------------------------------------------------
// Tmin lands near sunrise and Tmax mid-afternoon. A device that booted at noon
// has neither, but it does have a min and a max, and they will look like a
// perfectly ordinary pair of numbers. Unlike a mean, an extreme has no
// averaging in it, so nothing about the value itself reveals that half the day
// is missing. The coverage gate below is therefore not defensive tidiness - it
// is the only thing standing between a partial day and a confident wrong
// answer.

// Solar constant, MJ m-2 min-1 (FAO-56).
const double kEt0SolarConstant = 0.0820;

// Latent heat of vaporisation, MJ kg-1: converts MJ m-2 d-1 to mm d-1.
const double kEt0LatentHeat = 2.45;

// Distinct local hours a day must contain before its extremes are reported.
// 22 of 24 leaves room for a reboot or a stalled sensor without admitting a
// day that is missing either end of its own cycle. It is a choice, not a fit:
// no amount of data can say what fraction of a day is "enough", only physics
// can say that both turning points have to be inside it.
const uint8_t kEt0MinHoursCovered = 22;

// FAO-56 eq. 21/23/24/25 - extraterrestrial radiation on a horizontal surface,
// MJ m-2 day-1. Pure geometry: no weather in it at all.
//
// Beyond the polar circles the sunset hour angle has no solution because the
// sun either never sets or never rises; both are answered rather than left to
// produce a NaN, so a device at any latitude returns a number or zero.
double
et0ExtraterrestrialRadiation(double latitudeDeg, int dayOfYear);

// FAO-56 eq. 52 - Hargreaves-Samani reference ET, mm day-1. A range of zero or
// less returns zero, which is what the formula says and not a special case.
double
et0Hargreaves(double tMinC, double tMaxC, double raMjPerDay);

// The day in progress. POD, so the Arduino half can keep one at file scope and
// a test can build its own.
struct Et0Day
{
    int32_t dayKey;    ///< opaque, monotonic per local day; <0 means none open
    int16_t dayOfYear; ///< of the day in progress, for Ra when it closes
    float tMin;
    float tMax;
    uint32_t hourMask; ///< bit h set once local hour h has contributed
    uint32_t samples;
};

// What closing a day produced.
struct Et0Result
{
    bool closed; ///< a day ended on this fold
    bool valid;  ///< ...and it was watched long enough to be believed
    double et0Mm;
    float tMin;
    float tMax;
    float rangeK;
    int16_t dayOfYear;
    uint8_t hoursCovered;
    uint32_t samples;
};

void
et0DayReset(Et0Day& day);

// Number of local hours the day in progress has seen. Exposed because the
// Arduino half reports it live, before any day has closed.
uint8_t
et0HoursCovered(const Et0Day& day);

// Folds one reading into the day identified by `dayKey`.
//
// Returns `closed == false` on every ordinary call. When `dayKey` names a
// different day from the one in progress, the day in progress is CLOSED and
// returned - with `valid` set only if it met the coverage gate - and the new
// day starts holding this reading.
//
// `dayKey` must increase from one local day to the next; the Arduino half
// builds it from a `struct tm` so no timezone arithmetic happens here. A key
// that moves BACKWARDS is a clock correction, and the day in progress is
// discarded rather than closed: it was accumulating against a timeline that
// turned out not to exist.
//
// A non-finite reading is dropped without touching the day, exactly as the
// accumulator drops one rather than poisoning a running sum.
Et0Result
et0DayFold(Et0Day& day,
           int32_t dayKey,
           int dayOfYear,
           int hourOfDay,
           float temperatureC,
           double latitudeDeg);
