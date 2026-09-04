#include "core/evapotranspiration.h"
#include <math.h>
#include <unity.h>

// The arithmetic behind the daily ET0 figure: extraterrestrial radiation,
// Hargreaves-Samani, and the day-rollover state machine that decides whether a
// day was watched long enough for its extremes to mean anything.
//
// Everything here is arithmetic the device runs and nobody can watch: a wrong
// answer crashes nothing and reorders nothing, it just reports the wrong water
// demand -- the same reason segment_index.h and cloud_cover.cpp have tests.
//
// The Ra expectations are FAO-56's own worked value and a closed form derived
// by hand, never this implementation's output recorded after the fact. A test
// that asserts what the code already did proves only that nobody changed it --
// and the first draft of this file did exactly that twice, asserting a
// half-remembered book figure and an imprecise hand multiplication. Both were
// caught by computing them somewhere else.

static const double kLatBrasilia = -15.7885; // the garden this was written for

// ---------------------------------------------------------------------------
// extraterrestrial radiation
// ---------------------------------------------------------------------------

static void
test_ra_matches_fao56_worked_example()
{
    // FAO-56 Example 8: 3 September (J = 246) at latitude -20 deg gives
    // Ra = 32.2 MJ m-2 d-1. The book rounds to one decimal; a tenth of a
    // megajoule is 0.04 mm/day of ET0, far below anything this model claims.
    TEST_ASSERT_DOUBLE_WITHIN(0.1, 32.2,
                              et0ExtraterrestrialRadiation(-20.0, 246));
}

static void
test_ra_matches_the_closed_form_at_the_equator()
{
    // A second anchor that owes nothing to the implementation, because at the
    // equator the whole expression collapses by hand. phi = 0 makes
    // tan(phi) = 0, so ws = acos(0) = pi/2 -- a twelve-hour day, whatever the
    // declination -- and sin(phi) = 0 kills the first term. What is left is
    //
    //     Ra = (24*60/pi) * Gsc * dr * cos(dec)
    //
    // and at J = 80, dec = 0.409*sin(2*pi*80/365 - 1.39) = 0.00337 rad, so
    // cos(dec) = 0.9999943, while dr = 1 + 0.033*cos(2*pi*80/365) = 1.0063665.
    // (1440/pi) * 0.0820 = 37.58535, giving 37.58535 * 1.0063665 * 0.9999943
    // = 37.8246. Every number there is arithmetic done outside this file.
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 37.8246,
                              et0ExtraterrestrialRadiation(0.0, 80));
}

static void
test_ra_peaks_at_the_local_summer_solstice()
{
    // Southern hemisphere: the December solstice is the bright end, the June
    // one the dim end. Getting the sign of the declination wrong swaps these
    // and produces a model that is confidently out of phase by six months --
    // and, on ten days of one season, indistinguishable from a correct one.
    const double december = et0ExtraterrestrialRadiation(kLatBrasilia, 355);
    const double june = et0ExtraterrestrialRadiation(kLatBrasilia, 172);
    TEST_ASSERT_TRUE(december > june);

    // Northern hemisphere at the mirror latitude, the other way round.
    TEST_ASSERT_TRUE(et0ExtraterrestrialRadiation(-kLatBrasilia, 172) >
                     et0ExtraterrestrialRadiation(-kLatBrasilia, 355));
}

static void
test_ra_handles_the_poles_instead_of_returning_nan()
{
    // Beyond the polar circles -tan(phi)tan(dec) leaves [-1, 1] and acos() is
    // undefined. Polar night must be zero and polar day must be large; a NaN
    // here would propagate silently into every ET0 the device ever reported.
    const double polarNight = et0ExtraterrestrialRadiation(80.0, 355);
    const double polarDay = et0ExtraterrestrialRadiation(80.0, 172);
    TEST_ASSERT_FALSE(isnan(polarNight));
    TEST_ASSERT_FALSE(isnan(polarDay));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 0.0, polarNight);
    TEST_ASSERT_TRUE(polarDay > 40.0);

    // The equator is nearly flat across the year, which is the other end of the
    // same behaviour.
    const double eqMar = et0ExtraterrestrialRadiation(0.0, 80);
    const double eqSep = et0ExtraterrestrialRadiation(0.0, 264);
    TEST_ASSERT_DOUBLE_WITHIN(1.5, eqMar, eqSep);
}

static void
test_ra_refuses_nonsense_inputs()
{
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, et0ExtraterrestrialRadiation(0.0, 0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, et0ExtraterrestrialRadiation(0.0, 400));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0,
                              et0ExtraterrestrialRadiation(NAN, 100));
}

// ---------------------------------------------------------------------------
// Hargreaves-Samani
// ---------------------------------------------------------------------------

static void
test_hargreaves_reproduces_a_hand_computed_day()
{
    // 0.0023 * (Tmean + 17.8) * sqrt(Tmax - Tmin) * Ra / 2.45, worked outside
    // this file: Tmin 18.3, Tmax 29.6 -> Tmean 23.95, range 11.3,
    // sqrt(11.3) = 3.3615473, Ra 33.5, so
    //   0.0023 * 41.75 * 3.3615473 * 33.5 / 2.45 = 4.4136944
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 4.4136944,
                              et0Hargreaves(18.3, 29.6, 33.5));
}

static void
test_hargreaves_is_driven_by_the_range_not_only_the_level()
{
    // The same mean temperature with half the diurnal range must give a
    // markedly smaller answer -- this IS the mechanism, and a version that
    // ignored the range would still look plausible on a single day.
    const double wide = et0Hargreaves(18.0, 30.0, 33.0);  // mean 24, range 12
    const double narrow = et0Hargreaves(21.0, 27.0, 33.0); // mean 24, range 6
    TEST_ASSERT_TRUE(wide > narrow);
    // sqrt(12)/sqrt(6) = sqrt(2), and everything else in the formula is equal.
    TEST_ASSERT_DOUBLE_WITHIN(0.001, sqrt(2.0), wide / narrow);
}

static void
test_hargreaves_returns_zero_rather_than_nan_on_a_flat_day()
{
    // A range of zero has no radiation proxy in it. sqrt of a negative range
    // would be NaN, which would then be published as a number.
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, et0Hargreaves(25.0, 25.0, 33.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, et0Hargreaves(30.0, 25.0, 33.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, et0Hargreaves(18.0, 30.0, 0.0));
    TEST_ASSERT_FALSE(isnan(et0Hargreaves(NAN, 30.0, 33.0)));
}

// ---------------------------------------------------------------------------
// the day tracker
// ---------------------------------------------------------------------------

// Feeds one reading in every hour of `hours`, at `temp` degrees, so a test can
// state the coverage it wants in one line.
static Et0Result
feedHours(Et0Day& day, int32_t key, int dayOfYear, int hours, float lo, float hi)
{
    Et0Result last = { false, false, 0.0, 0.0f, 0.0f, 0.0f, 0, 0, 0 };
    for (int h = 0; h < hours; ++h) {
        // Two readings an hour so the extremes land inside the covered hours.
        const Et0Result a =
          et0DayFold(day, key, dayOfYear, h, lo, kLatBrasilia);
        const Et0Result b =
          et0DayFold(day, key, dayOfYear, h, hi, kLatBrasilia);
        if (a.closed) {
            last = a;
        }
        if (b.closed) {
            last = b;
        }
    }
    return last;
}

static void
test_a_complete_day_closes_and_reports_its_extremes()
{
    Et0Day day;
    et0DayReset(day);

    Et0Result r = feedHours(day, 100, 246, 24, 18.3f, 29.6f);
    TEST_ASSERT_FALSE(r.closed); // nothing has rolled over yet

    // The next day arriving is what closes the previous one.
    r = et0DayFold(day, 101, 247, 0, 20.0f, kLatBrasilia);
    TEST_ASSERT_TRUE(r.closed);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(24, r.hoursCovered);
    TEST_ASSERT_EQUAL_INT16(246, r.dayOfYear);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 18.3f, r.tMin);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 29.6f, r.tMax);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 11.3f, r.rangeK);

    // ...and the ET0 is the one the formula gives for those extremes.
    const double ra = et0ExtraterrestrialRadiation(kLatBrasilia, 246);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, et0Hargreaves(18.3, 29.6, ra), r.et0Mm);

    // The new day is already holding its first reading.
    TEST_ASSERT_EQUAL_UINT8(1, et0HoursCovered(day));
}

static void
test_a_short_day_closes_but_is_refused()
{
    // The trap this gate exists for: a device that booted at noon has a min and
    // a max, and they look like an ordinary pair of numbers. Unlike a mean, an
    // extreme carries nothing that reveals half the day is missing.
    Et0Day day;
    et0DayReset(day);
    feedHours(day, 100, 246, kEt0MinHoursCovered - 1, 24.0f, 29.0f);

    const Et0Result r = et0DayFold(day, 101, 247, 0, 20.0f, kLatBrasilia);
    TEST_ASSERT_TRUE(r.closed);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(kEt0MinHoursCovered - 1, r.hoursCovered);
    // Refused means no number at all, not a number with a flag beside it.
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, r.et0Mm);

    // ...and one more hour is enough.
    et0DayReset(day);
    feedHours(day, 100, 246, kEt0MinHoursCovered, 24.0f, 29.0f);
    const Et0Result ok = et0DayFold(day, 101, 247, 0, 20.0f, kLatBrasilia);
    TEST_ASSERT_TRUE(ok.valid);
    TEST_ASSERT_TRUE(ok.et0Mm > 0.0);
}

static void
test_coverage_counts_distinct_hours_not_samples()
{
    // A thousand readings inside one hour is not a day. Counting samples
    // instead of hours would pass a device that woke up for an hour at 1 Hz.
    Et0Day day;
    et0DayReset(day);
    for (int i = 0; i < 5000; ++i) {
        et0DayFold(day, 100, 246, 13, 25.0f + (float)(i % 7), kLatBrasilia);
    }
    TEST_ASSERT_EQUAL_UINT8(1, et0HoursCovered(day));

    const Et0Result r = et0DayFold(day, 101, 247, 0, 20.0f, kLatBrasilia);
    TEST_ASSERT_TRUE(r.closed);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_EQUAL_UINT32(5000, r.samples);
}

static void
test_a_clock_stepping_backwards_discards_the_day_rather_than_reporting_it()
{
    // An NTP correction can move the local day backwards. The day in progress
    // was accumulating against a timeline that turned out not to exist, so
    // reporting it would stamp real-looking extremes on a day that never ran.
    Et0Day day;
    et0DayReset(day);
    feedHours(day, 100, 246, 24, 18.0f, 30.0f);

    const Et0Result r = et0DayFold(day, 99, 245, 12, 22.0f, kLatBrasilia);
    TEST_ASSERT_FALSE(r.closed);
    TEST_ASSERT_FALSE(r.valid);

    // The tracker is now on the earlier day, holding only that reading.
    TEST_ASSERT_EQUAL_UINT32(1, day.samples);
    TEST_ASSERT_EQUAL_UINT8(1, et0HoursCovered(day));
}

static void
test_a_non_finite_reading_is_dropped_and_not_folded_in()
{
    // The DHT already refuses to add a NaN to its accumulator; this is the same
    // rule one layer up, because a NaN min or max would poison the whole day
    // and there is no way back from it.
    Et0Day day;
    et0DayReset(day);
    et0DayFold(day, 100, 246, 8, 25.0f, kLatBrasilia);
    et0DayFold(day, 100, 246, 9, NAN, kLatBrasilia);
    et0DayFold(day, 100, 246, 10, 27.0f, kLatBrasilia);

    TEST_ASSERT_EQUAL_UINT32(2, day.samples);
    TEST_ASSERT_EQUAL_UINT8(2, et0HoursCovered(day)); // hour 9 never counted
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, day.tMin);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 27.0f, day.tMax);
}

static void
test_a_single_short_excursion_owns_the_whole_day()
{
    // Not a bug being asserted -- a property being pinned down. On 2026-09-03 a
    // 38-minute patch of direct sun took this board's sensor to 45 C, and the
    // day's ET0 estimate went from ~3.3 to 7.35 mm against a station's 4.57.
    // A daily maximum has no averaging in it, so 38 minutes out of 1440 moved
    // the answer by more than a factor of two. Anything that later tries to
    // "clean" the extremes has to reckon with this test.
    Et0Day quiet;
    et0DayReset(quiet);
    feedHours(quiet, 100, 246, 24, 25.7f, 31.0f);
    const Et0Result ordinary =
      et0DayFold(quiet, 101, 247, 0, 26.0f, kLatBrasilia);

    Et0Day spiked;
    et0DayReset(spiked);
    feedHours(spiked, 100, 246, 24, 25.7f, 31.0f);
    // One reading, in an hour already covered, at the temperature the sun patch
    // produced.
    et0DayFold(spiked, 100, 246, 15, 45.04f, kLatBrasilia);
    const Et0Result burnt =
      et0DayFold(spiked, 101, 247, 0, 26.0f, kLatBrasilia);

    TEST_ASSERT_TRUE(ordinary.valid);
    TEST_ASSERT_TRUE(burnt.valid);
    TEST_ASSERT_EQUAL_UINT8(ordinary.hoursCovered, burnt.hoursCovered);
    TEST_ASSERT_TRUE(burnt.et0Mm > 2.0 * ordinary.et0Mm);
}

static void
test_the_first_reading_ever_opens_a_day_without_closing_one()
{
    // A freshly reset tracker has no day in progress, and the first fold must
    // not report a day made of one sample.
    Et0Day day;
    et0DayReset(day);
    const Et0Result r = et0DayFold(day, 500, 12, 6, 19.5f, kLatBrasilia);
    TEST_ASSERT_FALSE(r.closed);
    TEST_ASSERT_EQUAL_INT32(500, day.dayKey);
    TEST_ASSERT_EQUAL_UINT32(1, day.samples);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.5f, day.tMin);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.5f, day.tMax);
}

static void
test_a_skipped_day_still_closes_the_one_before_it()
{
    // The device can be off for a day. The day in progress must still close on
    // whatever day arrives next, not wait for its immediate successor.
    Et0Day day;
    et0DayReset(day);
    feedHours(day, 100, 246, 24, 18.0f, 30.0f);

    const Et0Result r = et0DayFold(day, 105, 251, 0, 21.0f, kLatBrasilia);
    TEST_ASSERT_TRUE(r.closed);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_INT16(246, r.dayOfYear); // the day that actually ran
}

void
run_evapotranspiration_tests(void)
{
    RUN_TEST(test_ra_matches_fao56_worked_example);
    RUN_TEST(test_ra_matches_the_closed_form_at_the_equator);
    RUN_TEST(test_ra_peaks_at_the_local_summer_solstice);
    RUN_TEST(test_ra_handles_the_poles_instead_of_returning_nan);
    RUN_TEST(test_ra_refuses_nonsense_inputs);
    RUN_TEST(test_hargreaves_reproduces_a_hand_computed_day);
    RUN_TEST(test_hargreaves_is_driven_by_the_range_not_only_the_level);
    RUN_TEST(test_hargreaves_returns_zero_rather_than_nan_on_a_flat_day);
    RUN_TEST(test_a_complete_day_closes_and_reports_its_extremes);
    RUN_TEST(test_a_short_day_closes_but_is_refused);
    RUN_TEST(test_coverage_counts_distinct_hours_not_samples);
    RUN_TEST(test_a_clock_stepping_backwards_discards_the_day_rather_than_reporting_it);
    RUN_TEST(test_a_non_finite_reading_is_dropped_and_not_folded_in);
    RUN_TEST(test_a_single_short_excursion_owns_the_whole_day);
    RUN_TEST(test_the_first_reading_ever_opens_a_day_without_closing_one);
    RUN_TEST(test_a_skipped_day_still_closes_the_one_before_it);
}
