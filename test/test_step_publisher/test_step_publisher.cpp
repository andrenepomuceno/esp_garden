#include "core/step_publisher.h"
#include <unity.h>

// Change-based publishing for step telemetry keys. Everything here is a case
// that would otherwise fail silently: a key that publishes on every tick still
// looks correct on a dashboard, and so does one that never publishes at all.

static const uint32_t kHeartbeat = 900u * 1000u; // mqtt.heartbeatSec default

static StepValue
fresh()
{
    StepValue step = { false, 0.0, 0 };
    return step;
}

// A key nobody has published yet has to go out whatever its value is: absent
// from the cloud and "unchanged since boot" look identical from outside.
static void
test_the_first_value_is_always_published(void)
{
    StepValue step = fresh();
    TEST_ASSERT_TRUE(stepValueDue(step, 0.0, 0.0, 1000, kHeartbeat));
    TEST_ASSERT_TRUE(step.valid);
}

static void
test_an_unchanged_value_is_not_republished(void)
{
    StepValue step = fresh();
    stepValueDue(step, 1.0, 0.0, 1000, kHeartbeat);
    TEST_ASSERT_FALSE(stepValueDue(step, 1.0, 0.0, 2000, kHeartbeat));
    TEST_ASSERT_FALSE(stepValueDue(step, 1.0, 0.0, 60000, kHeartbeat));
}

static void
test_a_changed_value_is_published(void)
{
    StepValue step = fresh();
    stepValueDue(step, 0.0, 0.0, 1000, kHeartbeat);
    TEST_ASSERT_TRUE(stepValueDue(step, 1.0, 0.0, 2000, kHeartbeat));
    // ...and then stays quiet at its new value.
    TEST_ASSERT_FALSE(stepValueDue(step, 1.0, 0.0, 3000, kHeartbeat));
}

// The whole point of the deadband. Two doubles that represent the same
// measurement differ in their last bits for ever, so with an exact comparison
// a float key would publish on every single tick and the mechanism would buy
// nothing at all.
static void
test_a_float_jittering_inside_the_deadband_is_not_republished(void)
{
    StepValue step = fresh();
    stepValueDue(step, 12.34, 0.05, 1000, kHeartbeat);

    TEST_ASSERT_FALSE(stepValueDue(step, 12.340000000000001, 0.05, 2000, kHeartbeat));
    TEST_ASSERT_FALSE(stepValueDue(step, 12.36, 0.05, 3000, kHeartbeat));
    TEST_ASSERT_FALSE(stepValueDue(step, 12.31, 0.05, 4000, kHeartbeat));
}

static void
test_a_float_moving_past_the_deadband_is_published(void)
{
    StepValue step = fresh();
    stepValueDue(step, 12.34, 0.05, 1000, kHeartbeat);
    TEST_ASSERT_TRUE(stepValueDue(step, 12.50, 0.05, 2000, kHeartbeat));
}

// The deadband must not become a ratchet: a channel drifting by less than the
// deadband per tick would never publish, however far it travelled, if the
// comparison were against the last SEEN value rather than the last PUBLISHED
// one. It is against the last published one, so the drift accumulates.
static void
test_slow_drift_still_crosses_the_deadband_eventually(void)
{
    StepValue step = fresh();
    stepValueDue(step, 0.0, 0.05, 1000, kHeartbeat);

    bool published = false;
    for (unsigned i = 1; i <= 10 && !published; ++i) {
        published = stepValueDue(step, i * 0.02, 0.05, 1000 + i * 1000, kHeartbeat);
    }
    TEST_ASSERT_TRUE(published);
}

// Exact keys — a relay contact, a reboot counter — pass a zero deadband, and
// that is correct rather than sloppy: their values are whole numbers a double
// represents exactly.
static void
test_a_zero_deadband_still_detects_a_single_count(void)
{
    StepValue step = fresh();
    stepValueDue(step, 41.0, 0.0, 1000, kHeartbeat);
    TEST_ASSERT_TRUE(stepValueDue(step, 42.0, 0.0, 2000, kHeartbeat));
}

// Without this an operator reading "latest" cannot tell a value that has sat
// still for a week from a device that died a week ago.
static void
test_the_heartbeat_republishes_an_unchanged_value(void)
{
    StepValue step = fresh();
    stepValueDue(step, 7.0, 0.0, 1000, kHeartbeat);

    TEST_ASSERT_FALSE(stepValueDue(step, 7.0, 0.0, 1000 + kHeartbeat - 1, kHeartbeat));
    TEST_ASSERT_TRUE(stepValueDue(step, 7.0, 0.0, 1000 + kHeartbeat, kHeartbeat));
}

// The heartbeat runs from the last PUBLISH, not from boot, so a key that
// changes often is not also heartbeating on top of its own changes.
static void
test_a_change_resets_the_heartbeat_clock(void)
{
    StepValue step = fresh();
    stepValueDue(step, 0.0, 0.0, 0, kHeartbeat);

    const uint32_t changedAt = kHeartbeat / 2;
    TEST_ASSERT_TRUE(stepValueDue(step, 1.0, 0.0, changedAt, kHeartbeat));

    // The original heartbeat deadline passes with nothing published...
    TEST_ASSERT_FALSE(stepValueDue(step, 1.0, 0.0, kHeartbeat + 1, kHeartbeat));
    // ...and the new one lands a full interval after the change.
    TEST_ASSERT_TRUE(
      stepValueDue(step, 1.0, 0.0, changedAt + kHeartbeat, kHeartbeat));
}

// millis() wraps at 49 days. A naive comparison would decide the heartbeat
// deadline is still in the future and stall every step key for another 49 days
// - at the one moment a long-running device most needs to prove it is alive.
static void
test_the_heartbeat_survives_a_millis_rollover(void)
{
    StepValue step = fresh();

    const uint32_t beforeWrap = 0xFFFFFFFFu - (kHeartbeat / 2);
    stepValueDue(step, 3.0, 0.0, beforeWrap, kHeartbeat);

    // Half an interval later, which is past zero. Not due yet.
    const uint32_t justAfterWrap = beforeWrap + (kHeartbeat / 4);
    TEST_ASSERT_FALSE(stepValueDue(step, 3.0, 0.0, justAfterWrap, kHeartbeat));

    // A full interval after the publish, still past zero. Due.
    const uint32_t aFullInterval = beforeWrap + kHeartbeat;
    TEST_ASSERT_TRUE(stepValueDue(step, 3.0, 0.0, aFullInterval, kHeartbeat));
}

// A relay that starts and stops between two looks is seen through the sticky
// mask, which presents as 0 -> 1 -> 0. Both edges have to publish, or the
// activation is either invisible or stuck on for ever.
static void
test_both_edges_of_a_sticky_flag_are_published(void)
{
    StepValue step = fresh();
    stepValueDue(step, 0.0, 0.0, 1000, kHeartbeat);

    TEST_ASSERT_TRUE(stepValueDue(step, 1.0, 0.0, 2000, kHeartbeat));
    TEST_ASSERT_TRUE(stepValueDue(step, 0.0, 0.0, 3000, kHeartbeat));
    TEST_ASSERT_FALSE(stepValueDue(step, 0.0, 0.0, 4000, kHeartbeat));
}

void
run_step_publisher_tests(void)
{
    RUN_TEST(test_the_first_value_is_always_published);
    RUN_TEST(test_an_unchanged_value_is_not_republished);
    RUN_TEST(test_a_changed_value_is_published);
    RUN_TEST(test_a_float_jittering_inside_the_deadband_is_not_republished);
    RUN_TEST(test_a_float_moving_past_the_deadband_is_published);
    RUN_TEST(test_slow_drift_still_crosses_the_deadband_eventually);
    RUN_TEST(test_a_zero_deadband_still_detects_a_single_count);
    RUN_TEST(test_the_heartbeat_republishes_an_unchanged_value);
    RUN_TEST(test_a_change_resets_the_heartbeat_clock);
    RUN_TEST(test_the_heartbeat_survives_a_millis_rollover);
    RUN_TEST(test_both_edges_of_a_sticky_flag_are_published);
}
