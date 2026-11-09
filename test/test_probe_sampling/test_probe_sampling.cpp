#include "core/probe_sampling.h"
#include <unity.h>

using namespace probe_sampling;

static const uint32_t kFloor = 50;

struct Run
{
    unsigned samples;
    uint32_t at[512];
};

static Run
simulate(uint32_t settleMs,
         uint32_t intervalMs,
         uint32_t processingMs,
         uint32_t lateAtMs,
         uint32_t lateByMs)
{
    Run run = {};
    Cycle cycle;
    begin(cycle, settleMs, intervalMs);
    uint32_t now = 0;
    for (;;) {
        const Action action = step(cycle, now);
        if (action != kWait) {
            run.at[run.samples++] = now;
        }
        if (action == kSampleAndFinish) {
            return run;
        }
        const uint32_t after = now + (action == kWait ? 0 : processingMs);
        uint32_t wake = after + waitMs(cycle, after, kFloor);
        if (lateByMs > 0 && wake >= lateAtMs && now < lateAtMs) {
            wake += lateByMs;
        }
        now = wake;
    }
}

static Window
emptyWindow()
{
    Window window;
    reset(window);
    return window;
}

static void
test_defaults_are_one_sample_at_the_settle(void)
{
    const Run run = simulate(2000, 0, 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT(1, run.samples);
    TEST_ASSERT_EQUAL_UINT32(2000, run.at[0]);
}

static void
test_defaults_keep_the_floor_rounding_a_short_settle_up(void)
{
    const Run run = simulate(10, 0, 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT(1, run.samples);
    TEST_ASSERT_EQUAL_UINT32(kFloor, run.at[0]);
}

static void
test_no_settle_is_one_immediate_sample_whatever_the_interval(void)
{
    const Run run = simulate(0, 100, 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT(1, run.samples);
    TEST_ASSERT_EQUAL_UINT32(0, run.at[0]);
}

static void
test_two_seconds_every_100_ms_is_twenty_samples_ending_on_the_settle(void)
{
    const Run run = simulate(2000, 100, 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT(20, run.samples);
    TEST_ASSERT_EQUAL_UINT32(100, run.at[0]);
    TEST_ASSERT_EQUAL_UINT32(1000, run.at[9]);
    TEST_ASSERT_EQUAL_UINT32(2000, run.at[19]);
    TEST_ASSERT_EQUAL_UINT(20, samplesPerWindow(2000, 100, kFloor));
}

static void
test_a_last_gap_under_the_floor_does_not_delay_the_final_sample(void)
{
    const Run run = simulate(2000, 199, 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(1990, run.at[run.samples - 2]);
    TEST_ASSERT_EQUAL_UINT32(2000, run.at[run.samples - 1]);
}

static void
test_an_interval_under_the_floor_is_measured_at_the_floor(void)
{
    const Run run = simulate(2000, 10, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(50, run.at[0]);
    TEST_ASSERT_EQUAL_UINT32(100, run.at[1]);
    TEST_ASSERT_EQUAL_UINT32(2000, run.at[run.samples - 1]);
    TEST_ASSERT_EQUAL_UINT(40, run.samples);
    TEST_ASSERT_EQUAL_UINT(40, samplesPerWindow(2000, 10, kFloor));
}

static void
test_an_interval_that_does_not_divide_the_settle_still_ends_on_it(void)
{
    const Run run = simulate(2000, 300, 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT(7, run.samples);
    TEST_ASSERT_EQUAL_UINT32(1800, run.at[5]);
    TEST_ASSERT_EQUAL_UINT32(2000, run.at[6]);
}

static void
test_an_interval_longer_than_the_settle_is_one_sample(void)
{
    const Run run = simulate(500, 1000, 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT(1, run.samples);
    TEST_ASSERT_EQUAL_UINT32(500, run.at[0]);
}

static void
test_a_late_wake_skips_the_missed_points_instead_of_stretching_the_window(void)
{
    const Run run = simulate(2000, 100, 0, 300, 550);
    TEST_ASSERT_EQUAL_UINT32(200, run.at[1]);
    TEST_ASSERT_EQUAL_UINT32(850, run.at[2]);
    TEST_ASSERT_EQUAL_UINT32(900, run.at[3]);
    TEST_ASSERT_EQUAL_UINT32(2000, run.at[run.samples - 1]);
    TEST_ASSERT_EQUAL_UINT(15, run.samples);
}

static void
test_a_wake_past_the_settle_is_the_final_sample(void)
{
    const Run run = simulate(2000, 100, 0, 1900, 600);
    TEST_ASSERT_EQUAL_UINT(19, run.samples);
    TEST_ASSERT_EQUAL_UINT32(2500, run.at[run.samples - 1]);
}

static void
test_an_early_wake_takes_nothing(void)
{
    Cycle cycle;
    begin(cycle, 2000, 100);
    TEST_ASSERT_EQUAL_INT(kWait, step(cycle, 40));
    TEST_ASSERT_EQUAL_UINT16(0, cycle.taken);
    TEST_ASSERT_EQUAL_UINT32(60, waitMs(cycle, 40, kFloor));
    TEST_ASSERT_EQUAL_INT(kSample, step(cycle, 100));
}

static void
test_a_window_past_capacity_keeps_the_newest_and_counts_the_rest(void)
{
    Window window = emptyWindow();
    for (uint32_t k = 1; k <= kCapacity + 10; ++k) {
        add(window, k * 10, (uint16_t)k);
    }
    TEST_ASSERT_EQUAL_UINT16(kCapacity, window.count);
    TEST_ASSERT_EQUAL_UINT(10, dropped(window));
    TEST_ASSERT_EQUAL_UINT16(11, window.adc[0]);
    TEST_ASSERT_EQUAL_UINT16(110, window.elapsedMs[0]);
    TEST_ASSERT_EQUAL_UINT16(kCapacity + 10, window.adc[kCapacity - 1]);
}

static void
test_elapsed_past_the_storage_saturates_rather_than_wrapping(void)
{
    Window window = emptyWindow();
    add(window, 70000, 5);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, window.elapsedMs[0]);
}

static void
test_a_tail_of_one_is_the_last_sample(void)
{
    Window window = emptyWindow();
    add(window, 100, 900);
    add(window, 200, 950);
    add(window, 300, 1000);
    unsigned used = 0;
    TEST_ASSERT_EQUAL_DOUBLE(1000.0, tailMedian(window, 1, &used));
    TEST_ASSERT_EQUAL_UINT(1, used);
}

static void
test_the_tail_median_rejects_a_spike(void)
{
    Window window = emptyWindow();
    const uint16_t values[] = { 100, 800, 802, 4095, 801, 799, 803 };
    for (unsigned k = 0; k < 7; ++k) {
        add(window, (k + 1) * 100, values[k]);
    }
    unsigned used = 0;
    TEST_ASSERT_EQUAL_DOUBLE(802.0, tailMedian(window, 5, &used));
    TEST_ASSERT_EQUAL_UINT(5, used);
}

static void
test_an_even_tail_averages_the_two_middle_samples(void)
{
    Window window = emptyWindow();
    const uint16_t values[] = { 10, 20, 30, 41 };
    for (unsigned k = 0; k < 4; ++k) {
        add(window, (k + 1) * 100, values[k]);
    }
    TEST_ASSERT_EQUAL_DOUBLE(25.0, tailMedian(window, 4, nullptr));
    TEST_ASSERT_EQUAL_DOUBLE(35.5, tailMedian(window, 2, nullptr));
}

static void
test_a_tail_longer_than_the_window_uses_every_sample_and_says_so(void)
{
    Window window = emptyWindow();
    add(window, 100, 7);
    add(window, 200, 3);
    add(window, 300, 5);
    unsigned used = 0;
    TEST_ASSERT_EQUAL_DOUBLE(5.0, tailMedian(window, 10, &used));
    TEST_ASSERT_EQUAL_UINT(3, used);
}

static void
test_an_empty_window_reports_no_samples_used(void)
{
    Window window = emptyWindow();
    unsigned used = 99;
    TEST_ASSERT_EQUAL_DOUBLE(0.0, tailMedian(window, 1, &used));
    TEST_ASSERT_EQUAL_UINT(0, used);
}

static Window
lateWindow(void)
{
    Window window = emptyWindow();
    add(window, 1800, 400);
    add(window, 1900, 416);
    add(window, 2300, 480);
    return window;
}

static void
test_an_on_time_final_sample_is_published_unchanged(void)
{
    Window window = emptyWindow();
    add(window, 1900, 416);
    add(window, 2000, 431);
    TEST_ASSERT_EQUAL_DOUBLE(431.0, valueAt(window, 2000));
    TEST_ASSERT_EQUAL_DOUBLE(431.0, settledValue(window, 1, 2000, nullptr));
    TEST_ASSERT_EQUAL_UINT32(0, lateMs(window, 2000));
}

static void
test_a_late_final_sample_is_interpolated_back_to_the_settle(void)
{
    const Window window = lateWindow();
    TEST_ASSERT_EQUAL_DOUBLE(432.0, valueAt(window, 2000));
    TEST_ASSERT_EQUAL_DOUBLE(432.0, settledValue(window, 1, 2000, nullptr));
    TEST_ASSERT_EQUAL_UINT32(300, lateMs(window, 2000));
}

static void
test_the_tail_median_sees_the_interpolated_final_not_the_late_one(void)
{
    const Window window = lateWindow();
    unsigned used = 0;
    TEST_ASSERT_EQUAL_DOUBLE(416.0, settledValue(window, 3, 2000, &used));
    TEST_ASSERT_EQUAL_UINT(3, used);
    TEST_ASSERT_EQUAL_DOUBLE(424.0, settledValue(window, 2, 2000, nullptr));
}

static void
test_a_late_single_sample_has_nothing_to_interpolate_from(void)
{
    Window window = emptyWindow();
    add(window, 2212, 467);
    TEST_ASSERT_EQUAL_DOUBLE(467.0, valueAt(window, 2000));
    TEST_ASSERT_EQUAL_UINT32(212, lateMs(window, 2000));
}

static void
test_the_raw_tail_median_ignores_the_settle(void)
{
    const Window window = lateWindow();
    TEST_ASSERT_EQUAL_DOUBLE(480.0, tailMedian(window, 1, nullptr));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, valueAt(emptyWindow(), 2000));
    TEST_ASSERT_EQUAL_UINT32(0, lateMs(emptyWindow(), 2000));
}

void
run_probe_sampling_tests(void)
{
    RUN_TEST(test_defaults_are_one_sample_at_the_settle);
    RUN_TEST(test_defaults_keep_the_floor_rounding_a_short_settle_up);
    RUN_TEST(test_no_settle_is_one_immediate_sample_whatever_the_interval);
    RUN_TEST(test_two_seconds_every_100_ms_is_twenty_samples_ending_on_the_settle);
    RUN_TEST(test_a_last_gap_under_the_floor_does_not_delay_the_final_sample);
    RUN_TEST(test_an_interval_under_the_floor_is_measured_at_the_floor);
    RUN_TEST(test_an_interval_that_does_not_divide_the_settle_still_ends_on_it);
    RUN_TEST(test_an_interval_longer_than_the_settle_is_one_sample);
    RUN_TEST(test_a_late_wake_skips_the_missed_points_instead_of_stretching_the_window);
    RUN_TEST(test_a_wake_past_the_settle_is_the_final_sample);
    RUN_TEST(test_an_early_wake_takes_nothing);
    RUN_TEST(test_a_window_past_capacity_keeps_the_newest_and_counts_the_rest);
    RUN_TEST(test_elapsed_past_the_storage_saturates_rather_than_wrapping);
    RUN_TEST(test_a_tail_of_one_is_the_last_sample);
    RUN_TEST(test_the_tail_median_rejects_a_spike);
    RUN_TEST(test_an_even_tail_averages_the_two_middle_samples);
    RUN_TEST(test_a_tail_longer_than_the_window_uses_every_sample_and_says_so);
    RUN_TEST(test_an_empty_window_reports_no_samples_used);
    RUN_TEST(test_an_on_time_final_sample_is_published_unchanged);
    RUN_TEST(test_a_late_final_sample_is_interpolated_back_to_the_settle);
    RUN_TEST(test_the_tail_median_sees_the_interpolated_final_not_the_late_one);
    RUN_TEST(test_a_late_single_sample_has_nothing_to_interpolate_from);
    RUN_TEST(test_the_raw_tail_median_ignores_the_settle);
}
