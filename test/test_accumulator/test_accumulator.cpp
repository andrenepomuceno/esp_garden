#include "core/accumulator_v2.h"
#include <math.h>
#include <cmath>
#include <unity.h>

// AccumulatorV2 backs every sensor channel: the value, the rolling average and
// the variance shown on the dashboard and published to ThingSpeak. It is also
// the only production class that compiles without the Arduino core, so it is
// where the host suite starts.

static void
test_empty_accumulator_is_safe(void)
{
    // Regression: getLast() used to be a bare sampleList.back(). The web server
    // answers before the first io task runs, so /data.json dereferenced an
    // empty std::list on every boot.
    AccumulatorV2 acc(10);

    TEST_ASSERT_EQUAL_UINT(0, acc.getSamples());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, acc.getLast());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, acc.getAverage());
}

static void
test_single_sample(void)
{
    AccumulatorV2 acc(10);
    acc.add(42.0f);

    TEST_ASSERT_EQUAL_UINT(1, acc.getSamples());
    TEST_ASSERT_EQUAL_FLOAT(42.0f, acc.getLast());
    TEST_ASSERT_EQUAL_FLOAT(42.0f, acc.getAverage());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, acc.variance);
}

static void
test_average_and_variance(void)
{
    AccumulatorV2 acc(10);
    const float samples[] = { 2.0f, 4.0f, 4.0f, 4.0f, 5.0f, 5.0f, 7.0f, 9.0f };
    for (float s : samples) {
        acc.add(s);
    }

    // Population variance of that set is exactly 4.
    TEST_ASSERT_EQUAL_UINT(8, acc.getSamples());
    TEST_ASSERT_EQUAL_FLOAT(5.0f, acc.getAverage());
    TEST_ASSERT_EQUAL_FLOAT(4.0f, acc.variance);
    TEST_ASSERT_EQUAL_FLOAT(9.0f, acc.getLast());
}

static void
test_variance_only_valid_after_getAverage(void)
{
    // variance is a public field recomputed as a side effect of getAverage().
    // web.cpp relies on that ordering when it builds the Inputs payload.
    AccumulatorV2 acc(10);
    acc.add(1.0f);
    acc.add(3.0f);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, acc.variance); // not computed yet
    acc.getAverage();
    TEST_ASSERT_EQUAL_FLOAT(1.0f, acc.variance);
}

static void
test_window_drops_oldest(void)
{
    AccumulatorV2 acc(3);
    acc.add(1.0f);
    acc.add(2.0f);
    acc.add(3.0f);
    acc.add(4.0f); // pushes 1.0 out

    TEST_ASSERT_EQUAL_UINT(3, acc.getSamples());
    TEST_ASSERT_EQUAL_FLOAT(3.0f, acc.getAverage()); // (2+3+4)/3
    TEST_ASSERT_EQUAL_FLOAT(4.0f, acc.getLast());
}

static void
test_setMaxLen_trims_immediately(void)
{
    // Regression: g_soilMoisture is an array, so it cannot pass a constructor
    // argument and kept the 120-sample default while every scalar accumulator
    // tracked the MQTT period. Its averages silently spanned a different
    // interval from every other channel.
    AccumulatorV2 acc(10);
    for (int i = 1; i <= 10; ++i) {
        acc.add((float)i);
    }
    TEST_ASSERT_EQUAL_UINT(10, acc.getSamples());

    acc.setMaxLen(3);

    TEST_ASSERT_EQUAL_UINT(3, acc.getSamples());
    TEST_ASSERT_EQUAL_FLOAT(9.0f, acc.getAverage()); // (8+9+10)/3
    TEST_ASSERT_EQUAL_FLOAT(10.0f, acc.getLast());
}

static void
test_setMaxLen_zero_is_ignored(void)
{
    // A zero window would make add() pop what it just pushed, leaving getLast()
    // permanently on the empty path.
    AccumulatorV2 acc(5);
    acc.add(7.0f);

    acc.setMaxLen(0);
    acc.add(8.0f);

    TEST_ASSERT_EQUAL_UINT(2, acc.getSamples());
    TEST_ASSERT_EQUAL_FLOAT(8.0f, acc.getLast());
}

static void
test_setMaxLen_grows_without_loss(void)
{
    AccumulatorV2 acc(2);
    acc.add(1.0f);
    acc.add(2.0f);

    acc.setMaxLen(4);
    acc.add(3.0f);
    acc.add(4.0f);

    TEST_ASSERT_EQUAL_UINT(4, acc.getSamples());
    TEST_ASSERT_EQUAL_FLOAT(2.5f, acc.getAverage());
}


// ---------------------------------------------------------------------------
// Allocation accounting
// ---------------------------------------------------------------------------
//
// CLAUDE.md carried "AccumulatorV2 allocates on every sample" as an open
// contradiction for as long as nothing measured it. A note cannot fail; this
// can. The counters are defined in runner.cpp, which overrides global new and
// delete for the whole test binary.

extern unsigned long g_allocCount;
extern unsigned long g_freeCount;

static void
test_steady_state_does_not_allocate()
{
    AccumulatorV2 acc(16);
    for (int i = 0; i < 64; ++i) {
        acc.add((float)i); // fill and wrap several times
    }

    // Everything from here is the steady state the device lives in: a sample a
    // second, forever.
    const unsigned long allocsBefore = g_allocCount;
    const unsigned long freesBefore = g_freeCount;

    for (int i = 0; i < 5000; ++i) {
        acc.add((float)(i % 97));
        (void)acc.getAverage();
        (void)acc.getSamples();
        (void)acc.getLast();
    }

    TEST_ASSERT_EQUAL_UINT32(0, (unsigned)(g_allocCount - allocsBefore));
    TEST_ASSERT_EQUAL_UINT32(0, (unsigned)(g_freeCount - freesBefore));
}

static void
test_sizing_the_window_allocates_once_and_only_then()
{
    const unsigned long before = g_allocCount;

    AccumulatorV2 acc(8);
    const unsigned long afterConstruct = g_allocCount;
    TEST_ASSERT_EQUAL_UINT32(1, (unsigned)(afterConstruct - before));

    acc.setMaxLen(32);
    TEST_ASSERT_EQUAL_UINT32(2, (unsigned)(g_allocCount - before));

    // Setting the same length again must not churn the buffer.
    acc.setMaxLen(32);
    TEST_ASSERT_EQUAL_UINT32(2, (unsigned)(g_allocCount - before));
}

static void
test_a_non_finite_sample_is_dropped_not_absorbed()
{
    // One NaN into a running sum poisons every later mean and variance, with
    // no way back short of a reboot. The list version recomputed from scratch
    // and so recovered; this one cannot, which is why it refuses the sample.
    AccumulatorV2 acc(8);
    for (int i = 0; i < 4; ++i) {
        acc.add(10.0f);
    }

    acc.add(NAN);
    acc.add(INFINITY);

    TEST_ASSERT_EQUAL_UINT32(4, acc.getSamples());
    TEST_ASSERT_FLOAT_WITHIN(1e-4, 10.0f, acc.getAverage());
    TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.0f, acc.variance);
}

static void
test_the_mean_does_not_drift_over_a_long_run()
{
    // The running sums are maintained incrementally, which accumulates rounding
    // error; resync() bounds it at one recompute per window. A channel at 1 Hz
    // sees three million samples a month, so this is the property that matters
    // most and the one a naive incremental implementation quietly loses.
    AccumulatorV2 acc(60);
    for (int i = 0; i < 200000; ++i) {
        acc.add(1000.0f + (float)(i % 3)); // mean of the window is 1001
    }

    TEST_ASSERT_FLOAT_WITHIN(1e-2, 1001.0f, acc.getAverage());
    TEST_ASSERT_FLOAT_WITHIN(1e-2, 0.6666f, acc.variance);
}

void
run_accumulator_tests(void)
{
    RUN_TEST(test_empty_accumulator_is_safe);
    RUN_TEST(test_single_sample);
    RUN_TEST(test_average_and_variance);
    RUN_TEST(test_variance_only_valid_after_getAverage);
    RUN_TEST(test_window_drops_oldest);
    RUN_TEST(test_setMaxLen_trims_immediately);
    RUN_TEST(test_setMaxLen_zero_is_ignored);
    RUN_TEST(test_setMaxLen_grows_without_loss);
    RUN_TEST(test_steady_state_does_not_allocate);
    RUN_TEST(test_sizing_the_window_allocates_once_and_only_then);
    RUN_TEST(test_a_non_finite_sample_is_dropped_not_absorbed);
    RUN_TEST(test_the_mean_does_not_drift_over_a_long_run);
}
