#include "core/cloud_cover.h"
#include <math.h>
#include <unity.h>

// The maths behind the Dry/Humid/Wet badge's daylight cousin: an empirical
// clear-sky reference, a clearness index, three states with hysteresis, and a
// transient episode built on how far the index moves in a minute.
//
// Everything here is arithmetic the device runs and nobody can watch: a wrong
// answer reorders nothing and crashes nothing, it just reports the wrong sky,
// which is the same reason segment_index.h and step_publisher.h have tests.

// A stand-in day, 24 bins of one hour, deliberately NOT the fitted table:
// a test that depends on the generated header would start failing whenever the
// weather did. Reference rises from 0 to 100 at 12:00 and back, so bin b is
// |b - 12| away from the peak.
static uint16_t g_table[24];

static CloudModelParams
params()
{
    for (int b = 0; b < 24; ++b) {
        const int distance = (b > 12) ? (b - 12) : (12 - b);
        const int percent = (distance >= 6) ? 0 : (100 - distance * 15);
        g_table[b] = (uint16_t)(percent * 100);
    }

    CloudModelParams p;
    p.clearSky = g_table;
    p.bins = 24;
    p.binMinutes = 60;
    p.firstMinute = 7 * 60;  // reference 25 % at the 07:30 bin centre
    p.lastMinute = 17 * 60 - 1;
    p.overcastBelow = 0.50f;
    p.clearAbove = 0.85f;
    p.stateBand = 0.05f;
    p.stateAlpha = 1.0f; // no smoothing, so a test states one thing at a time
    p.stateDwell = 3;
    p.varAlpha = 1.0f;
    p.transientEnter = 0.070f;
    p.transientExit = 0.035f;
    p.transientExitRun = 3;
    return p;
}

// ---------------------------------------------------------------------------
// the reference
// ---------------------------------------------------------------------------

static void
test_reference_is_interpolated_between_bin_centres()
{
    const CloudModelParams p = params();

    // Bin 11 covers 11:00-11:59 and its CENTRE is 11:30, where the table's own
    // value must be returned exactly.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 85.0f, cloudClearSky(p, 11 * 60 + 30));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, cloudClearSky(p, 12 * 60 + 30));

    // Halfway between the two centres is halfway between the two values. A
    // bin treated as a constant would answer 85 here, which on the real
    // morning ramp is an error larger than the gap between two states.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 92.5f, cloudClearSky(p, 12 * 60));
}

static void
test_outside_the_window_there_is_no_reference_and_no_index()
{
    const CloudModelParams p = params();

    TEST_ASSERT_EQUAL_FLOAT(0.0f, cloudClearSky(p, 6 * 60));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cloudClearSky(p, 18 * 60));

    // Negative, and not zero: zero is a black sky at noon, which is a reading.
    TEST_ASSERT_TRUE(cloudClearness(p, 6 * 60, 40.0f) < 0.0f);
    TEST_ASSERT_TRUE(cloudClearness(p, 12 * 60 + 30, 40.0f) > 0.0f);
}

static void
test_clearness_is_the_ratio_to_the_reference()
{
    const CloudModelParams p = params();
    TEST_ASSERT_FLOAT_WITHIN(
      0.001f, 0.50f, cloudClearness(p, 12 * 60 + 30, 50.0f));
    TEST_ASSERT_FLOAT_WITHIN(
      0.001f, 1.00f, cloudClearness(p, 12 * 60 + 30, 100.0f));

    // The same physical sky at a different hour gives the same index, which is
    // the whole point of dividing by a time-of-day reference rather than
    // thresholding the reading. Bin 10's centre is 10:30 and its reference is
    // 70, so half of it is 35 and not the 50 the noon reading needed.
    TEST_ASSERT_FLOAT_WITHIN(
      0.001f, 0.50f, cloudClearness(p, 10 * 60 + 30, 35.0f));
}

// ---------------------------------------------------------------------------
// states
// ---------------------------------------------------------------------------

static void
test_thresholds_split_the_index_into_three()
{
    const CloudModelParams p = params();
    TEST_ASSERT_EQUAL_INT(CLOUD_CLEAR, cloudClassify(p, 0.95f));
    TEST_ASSERT_EQUAL_INT(CLOUD_CLEAR, cloudClassify(p, 0.85f));
    TEST_ASSERT_EQUAL_INT(CLOUD_PARTLY, cloudClassify(p, 0.84f));
    TEST_ASSERT_EQUAL_INT(CLOUD_PARTLY, cloudClassify(p, 0.50f));
    TEST_ASSERT_EQUAL_INT(CLOUD_OVERCAST, cloudClassify(p, 0.49f));
}

static void
test_leaving_a_state_costs_the_hysteresis_band()
{
    const CloudModelParams p = params();

    // 0.83 is below the clear threshold but inside the band, so a model that is
    // already clear stays clear. Without this a sky sitting on a boundary
    // flaps, and every flap is a published datapoint.
    TEST_ASSERT_EQUAL_INT(CLOUD_CLEAR,
                          cloudStateWithHysteresis(p, CLOUD_CLEAR, 0.83f));
    TEST_ASSERT_EQUAL_INT(CLOUD_PARTLY,
                          cloudStateWithHysteresis(p, CLOUD_CLEAR, 0.79f));

    // Symmetric on the way in: partly needs to clear the band to become clear.
    TEST_ASSERT_EQUAL_INT(CLOUD_PARTLY,
                          cloudStateWithHysteresis(p, CLOUD_PARTLY, 0.88f));
    TEST_ASSERT_EQUAL_INT(CLOUD_CLEAR,
                          cloudStateWithHysteresis(p, CLOUD_PARTLY, 0.91f));

    // ...and at the bottom boundary too.
    TEST_ASSERT_EQUAL_INT(CLOUD_OVERCAST,
                          cloudStateWithHysteresis(p, CLOUD_OVERCAST, 0.53f));
    TEST_ASSERT_EQUAL_INT(CLOUD_PARTLY,
                          cloudStateWithHysteresis(p, CLOUD_OVERCAST, 0.56f));
}

static void
feed(CloudModel& m,
     const CloudModelParams& p,
     int minute,
     float value,
     unsigned times)
{
    for (unsigned i = 0; i < times; ++i) {
        cloudModelMinute(m, p, minute, value);
    }
}

static void
test_the_first_classified_minute_is_taken_immediately()
{
    const CloudModelParams p = params();
    CloudModel m;
    cloudModelReset(m);

    const int events = cloudModelMinute(m, p, 12 * 60 + 30, 95.0f);
    TEST_ASSERT_TRUE((events & CLOUD_EVENT_STATE) != 0);
    TEST_ASSERT_EQUAL_INT(CLOUD_CLEAR, m.state);
}

static void
test_a_state_change_waits_out_its_dwell()
{
    const CloudModelParams p = params();
    CloudModel m;
    cloudModelReset(m);

    feed(m, p, 12 * 60 + 30, 95.0f, 1);
    TEST_ASSERT_EQUAL_INT(CLOUD_CLEAR, m.state);

    // Two minutes of overcast is not a state change on a dwell of three.
    feed(m, p, 12 * 60 + 30, 30.0f, 2);
    TEST_ASSERT_EQUAL_INT(CLOUD_CLEAR, m.state);

    const int events = cloudModelMinute(m, p, 12 * 60 + 30, 30.0f);
    TEST_ASSERT_TRUE((events & CLOUD_EVENT_STATE) != 0);
    TEST_ASSERT_EQUAL_INT(CLOUD_OVERCAST, m.state);
}

static void
test_a_candidate_that_gives_up_does_not_carry_its_run_forward()
{
    const CloudModelParams p = params();
    CloudModel m;
    cloudModelReset(m);

    feed(m, p, 12 * 60 + 30, 95.0f, 1);
    feed(m, p, 12 * 60 + 30, 30.0f, 2); // two of the three minutes needed
    feed(m, p, 12 * 60 + 30, 95.0f, 1); // back to clear: the run is void
    feed(m, p, 12 * 60 + 30, 30.0f, 2); // two again, still not three
    TEST_ASSERT_EQUAL_INT(CLOUD_CLEAR, m.state);
}

static void
test_the_night_resets_the_state_rather_than_carrying_it_over()
{
    const CloudModelParams p = params();
    CloudModel m;
    cloudModelReset(m);

    feed(m, p, 12 * 60 + 30, 95.0f, 1);
    TEST_ASSERT_EQUAL_INT(CLOUD_CLEAR, m.state);

    // 20:00 is outside the window. Without this the state survives to the next
    // morning and reports last evening's sky as today's.
    const int events = cloudModelMinute(m, p, 20 * 60, 0.0f);
    TEST_ASSERT_TRUE((events & CLOUD_EVENT_STATE) != 0);
    TEST_ASSERT_EQUAL_INT(CLOUD_UNKNOWN, m.state);
    TEST_ASSERT_FALSE(m.hasPrevK);
}

static void
test_an_unknown_state_is_the_empty_string()
{
    // Absent rather than "unknown", the convention moistureState() uses: a
    // badge that renders on every night hour trains the eye to skip the field.
    TEST_ASSERT_EQUAL_STRING("", cloudStateName(CLOUD_UNKNOWN));
    TEST_ASSERT_EQUAL_STRING("clear", cloudStateName(CLOUD_CLEAR));
    TEST_ASSERT_EQUAL_STRING("partly cloudy", cloudStateName(CLOUD_PARTLY));
    TEST_ASSERT_EQUAL_STRING("overcast", cloudStateName(CLOUD_OVERCAST));
}

// ---------------------------------------------------------------------------
// transients
// ---------------------------------------------------------------------------

static void
test_a_steady_sky_opens_no_episode()
{
    const CloudModelParams p = params();
    CloudModel m;
    cloudModelReset(m);

    for (unsigned i = 0; i < 60; ++i) {
        const int events = cloudModelMinute(m, p, 12 * 60 + 30, 90.0f);
        TEST_ASSERT_EQUAL_INT(0, events & CLOUD_EVENT_TRANSIENT_BEGAN);
    }
    TEST_ASSERT_FALSE(m.inTransient);
}

static void
test_a_smooth_ramp_opens_no_episode_even_though_the_level_moves_far()
{
    // The reading climbs the whole morning and the level spread is large; the
    // index barely moves, because the reference climbs with it. This is the
    // case that killed the level-variance version of this detector: measured on
    // this device, a smooth ramp and a flickering sky are indistinguishable by
    // the spread of the LEVEL.
    const CloudModelParams p = params();
    CloudModel m;
    cloudModelReset(m);

    for (int minute = 8 * 60; minute < 12 * 60; minute += 1) {
        const float reference = cloudClearSky(p, minute);
        const int events = cloudModelMinute(m, p, minute, reference * 0.90f);
        TEST_ASSERT_EQUAL_INT(0, events & CLOUD_EVENT_TRANSIENT_BEGAN);
    }
    TEST_ASSERT_FALSE(m.inTransient);
}

static void
test_a_flickering_sky_opens_and_closes_one_episode()
{
    const CloudModelParams p = params();
    CloudModel m;
    cloudModelReset(m);

    feed(m, p, 12 * 60 + 30, 90.0f, 3);

    // Alternating 90 and 60 is |dk| = 0.30 a minute, four times the enter
    // threshold.
    int opened = 0;
    for (unsigned i = 0; i < 10; ++i) {
        opened += (cloudModelMinute(
                     m, p, 12 * 60 + 30, (i % 2) ? 60.0f : 90.0f) &
                   CLOUD_EVENT_TRANSIENT_BEGAN)
                    ? 1
                    : 0;
    }
    TEST_ASSERT_EQUAL_INT(1, opened); // ONE episode, not ten
    TEST_ASSERT_TRUE(m.inTransient);
    TEST_ASSERT_TRUE(m.transientPeak > p.transientEnter);

    // It must not close on the first quiet minute either: a gap between two
    // clouds is not the end of a broken sky. The first steady reading still
    // carries the step DOWN from the last dip, so the exit run only starts on
    // the second — four quiet minutes to close a three-minute dwell, which is
    // the arithmetic and not an off-by-one.
    for (unsigned i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_INT(
          0,
          cloudModelMinute(m, p, 12 * 60 + 30, 90.0f) &
            CLOUD_EVENT_TRANSIENT_ENDED);
        TEST_ASSERT_TRUE(m.inTransient);
    }
    TEST_ASSERT_TRUE((cloudModelMinute(m, p, 12 * 60 + 30, 90.0f) &
                      CLOUD_EVENT_TRANSIENT_ENDED) != 0);
    TEST_ASSERT_FALSE(m.inTransient);
}

static void
test_dusk_closes_an_open_episode()
{
    const CloudModelParams p = params();
    CloudModel m;
    cloudModelReset(m);

    feed(m, p, 12 * 60 + 30, 90.0f, 2);
    for (unsigned i = 0; i < 6; ++i) {
        cloudModelMinute(m, p, 12 * 60 + 30, (i % 2) ? 60.0f : 90.0f);
    }
    TEST_ASSERT_TRUE(m.inTransient);

    const int events = cloudModelMinute(m, p, 20 * 60, 0.0f);
    TEST_ASSERT_TRUE((events & CLOUD_EVENT_TRANSIENT_ENDED) != 0);
    TEST_ASSERT_FALSE(m.inTransient);
}

// ---------------------------------------------------------------------------
// the published variability
// ---------------------------------------------------------------------------

static void
test_variability_is_the_mean_step_and_negative_when_unmeasured()
{
    const CloudModelParams p = params();
    CloudModel m;
    cloudModelReset(m);

    // Nothing folded in: negative, never zero. Zero is a perfectly still sky
    // and the two must not share a value in a stored series.
    TEST_ASSERT_TRUE(cloudModelTakeVariability(m) < 0.0f);

    // The FIRST minute produces no step — there is nothing to subtract from.
    cloudModelMinute(m, p, 12 * 60 + 30, 100.0f);
    TEST_ASSERT_TRUE(cloudModelTakeVariability(m) < 0.0f);

    cloudModelMinute(m, p, 12 * 60 + 30, 90.0f);  // dk = 0.10
    cloudModelMinute(m, p, 12 * 60 + 30, 70.0f);  // dk = 0.20
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.15f, cloudModelTakeVariability(m));

    // Take-and-clear: a second reader would get the same minutes again.
    TEST_ASSERT_TRUE(cloudModelTakeVariability(m) < 0.0f);
}

static void
test_dusk_does_not_discard_minutes_the_payload_has_not_taken()
{
    // The daylight window and the publish period are different clocks. Clearing
    // the accumulator at dusk would silently drop the last minutes of every
    // afternoon from the stored series.
    const CloudModelParams p = params();
    CloudModel m;
    cloudModelReset(m);

    cloudModelMinute(m, p, 12 * 60 + 30, 100.0f);
    cloudModelMinute(m, p, 12 * 60 + 30, 90.0f);
    cloudModelMinute(m, p, 20 * 60, 0.0f); // dusk

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.10f, cloudModelTakeVariability(m));
}

void
run_cloud_cover_tests(void)
{
    RUN_TEST(test_reference_is_interpolated_between_bin_centres);
    RUN_TEST(test_outside_the_window_there_is_no_reference_and_no_index);
    RUN_TEST(test_clearness_is_the_ratio_to_the_reference);
    RUN_TEST(test_thresholds_split_the_index_into_three);
    RUN_TEST(test_leaving_a_state_costs_the_hysteresis_band);
    RUN_TEST(test_the_first_classified_minute_is_taken_immediately);
    RUN_TEST(test_a_state_change_waits_out_its_dwell);
    RUN_TEST(test_a_candidate_that_gives_up_does_not_carry_its_run_forward);
    RUN_TEST(test_the_night_resets_the_state_rather_than_carrying_it_over);
    RUN_TEST(test_an_unknown_state_is_the_empty_string);
    RUN_TEST(test_a_steady_sky_opens_no_episode);
    RUN_TEST(test_a_smooth_ramp_opens_no_episode_even_though_the_level_moves_far);
    RUN_TEST(test_a_flickering_sky_opens_and_closes_one_episode);
    RUN_TEST(test_dusk_closes_an_open_episode);
    RUN_TEST(test_variability_is_the_mean_step_and_negative_when_unmeasured);
    RUN_TEST(test_dusk_does_not_discard_minutes_the_payload_has_not_taken);
}
