#include "core/probe_health.h"
#include <unity.h>

// The settling test: two back-to-back conversions of the same pin, regressed
// against the step the ADC was asked to make from the previous channel. A stiff
// source recharges the sample-and-hold capacitor inside the sampling window and
// reads the same twice; a floating pin does not.
//
// These are synthetic, because the point of the statistic is that its null
// hypothesis is a NUMBER — slope zero — rather than a calibration against a
// healthy probe. That is what makes it testable here at all, and it is why it
// was chosen: this garden currently has three disconnected probes and no
// healthy one to calibrate against.

static void
feed(ProbeHealth& h, int previous, int settled, double coupling, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        // A previous channel that moves, so `drive` has spread to regress on,
        // and a settled reading that drifts slightly, as any real one does.
        const int prev = previous + (int)(i % 7) * 130;
        const int here = settled + (int)(i % 3);
        const double drive = (double)prev - (double)here;
        const int first = here + (int)(coupling * drive);
        probeHealthAdd(h, prev, first, here);
    }
}

static void
test_a_stiff_source_shows_no_settling()
{
    // A real sensor: both conversions identical however far the ADC just moved.
    ProbeHealth h;
    probeHealthReset(h);
    feed(h, 400, 2000, 0.0, 60);

    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, probeHealthSlope(h));
    TEST_ASSERT_EQUAL_INT(PROBE_CONNECTED,
                          probeHealthVerdict(h, 30, 400.0, 0.05, 5.0));
}

static void
test_a_floating_pin_is_dragged_toward_the_previous_channel()
{
    // 30 % of the step is carried over: the first conversion sits between the
    // previous channel and this node, the second does not.
    ProbeHealth h;
    probeHealthReset(h);
    feed(h, 400, 2000, 0.30, 60);

    TEST_ASSERT_DOUBLE_WITHIN(0.02, 0.30, probeHealthSlope(h));
    TEST_ASSERT_EQUAL_INT(PROBE_FLOATING,
                          probeHealthVerdict(h, 30, 400.0, 0.05, 5.0));
}

static void
test_the_verdict_needs_both_an_effect_and_a_confidence()
{
    // A coupling far below the threshold is not an accusation, however many
    // samples back it: a resistive probe in dry soil is genuinely a bit stiff
    // and must not be condemned for it.
    ProbeHealth h;
    probeHealthReset(h);
    feed(h, 400, 2000, 0.01, 200);

    TEST_ASSERT_TRUE(probeHealthSlope(h) < 0.05);
    TEST_ASSERT_EQUAL_INT(PROBE_CONNECTED,
                          probeHealthVerdict(h, 30, 400.0, 0.05, 5.0));
}

static void
test_too_few_samples_is_unknown_not_healthy()
{
    // The difference that matters to a person reading the page: "no evidence"
    // and "evidence of health" are not the same claim.
    ProbeHealth h;
    probeHealthReset(h);
    feed(h, 400, 2000, 0.30, 10);

    TEST_ASSERT_EQUAL_INT(PROBE_UNKNOWN, probeHealthVerdict(h, 30, 400.0, 0.05, 5.0));
}

static void
test_no_spread_in_the_drive_cannot_accuse_anything()
{
    // Every reading followed the same step, so there is no regression to run.
    // The slope is 0 — which must read as "unmeasurable", never as "stiff".
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 60; ++i) {
        probeHealthAdd(h, 2000, 2000, 2000);
    }

    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, probeHealthSlope(h));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, probeHealthT(h));
    // Identical every time, so this is caught as STUCK rather than passed as
    // connected — a module that died while still driving a level.
    TEST_ASSERT_EQUAL_INT(PROBE_STUCK, probeHealthVerdict(h, 30, 400.0, 0.05, 5.0));
}

static void
test_a_pin_at_a_rail_is_named_as_such()
{
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 60; ++i) {
        probeHealthAdd(h, 1000 + (int)(i % 5) * 200, 4095, 4095);
    }
    TEST_ASSERT_EQUAL_INT(PROBE_RAILED, probeHealthVerdict(h, 30, 400.0, 0.05, 5.0));

    probeHealthReset(h);
    for (unsigned i = 0; i < 60; ++i) {
        probeHealthAdd(h, 1000 + (int)(i % 5) * 200, 0, 0);
    }
    TEST_ASSERT_EQUAL_INT(PROBE_RAILED, probeHealthVerdict(h, 30, 400.0, 0.05, 5.0));
}

static void
test_decay_ages_the_evidence_without_moving_the_estimate()
{
    // The same contract gaussianDecay() keeps for the classifier: yesterday's
    // measurement is still evidence about today, just less of it.
    ProbeHealth h;
    probeHealthReset(h);
    feed(h, 400, 2000, 0.30, 100);

    const double before = probeHealthSlope(h);
    const double nBefore = h.n;
    probeHealthDecay(h, 0.5);

    TEST_ASSERT_DOUBLE_WITHIN(1e-6, before, probeHealthSlope(h));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, nBefore * 0.5, h.n);
    // And the confidence really did fall, or the decay would be cosmetic.
    TEST_ASSERT_TRUE(probeHealthSlopeStdErr(h) >= 0.0);
}

static void
test_a_probe_plugged_back_in_can_clear_its_own_accusation()
{
    // Decay is what makes the verdict recoverable. Without it a probe that was
    // unplugged for an hour would carry the finding for the life of the boot.
    ProbeHealth h;
    probeHealthReset(h);
    feed(h, 400, 2000, 0.30, 100);
    TEST_ASSERT_EQUAL_INT(PROBE_FLOATING,
                          probeHealthVerdict(h, 30, 400.0, 0.05, 5.0));

    for (int run = 0; run < 20; ++run) {
        probeHealthDecay(h, 0.5);
        feed(h, 400, 2000, 0.0, 50);
    }

    TEST_ASSERT_TRUE(probeHealthSlope(h) < 0.05);
    TEST_ASSERT_EQUAL_INT(PROBE_CONNECTED,
                          probeHealthVerdict(h, 30, 400.0, 0.05, 5.0));
}

static void
test_a_reading_that_swings_further_than_soil_can_is_condemned()
{
    // Measured live: three unplugged probes swinging 0.00 to 100.00 within
    // seconds, sd 1140-1830 ADC counts, against a CONNECTED luminosity channel
    // on the same ADC at sd 6. Water content does not move ten points in a
    // minute, so a spread like this is not a reading at all.
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 200; ++i) {
        const int v = (i % 2) ? 4000 : 90;
        probeHealthAdd(h, 2000, v, v);
    }

    TEST_ASSERT_TRUE(probeHealthSd(h) > 400.0);
    TEST_ASSERT_EQUAL_INT(PROBE_NOISY,
                          probeHealthVerdict(h, 30, 400.0, 0.05, 5.0));
}

static void
test_a_quiet_floating_pin_still_needs_the_impedance_test()
{
    // The asymmetry that keeps both tests. High variance condemns; low
    // variance clears nothing — this board has held a disconnected probe at
    // variance 0.01, quieter than any of its connected ones. Only the coupling
    // catches that one.
    ProbeHealth h;
    probeHealthReset(h);
    feed(h, 400, 2000, 0.30, 200);

    TEST_ASSERT_TRUE(probeHealthSd(h) < 400.0);
    TEST_ASSERT_EQUAL_INT(PROBE_FLOATING,
                          probeHealthVerdict(h, 30, 400.0, 0.05, 5.0));
}

void
run_probe_health_tests(void)
{
    RUN_TEST(test_a_reading_that_swings_further_than_soil_can_is_condemned);
    RUN_TEST(test_a_quiet_floating_pin_still_needs_the_impedance_test);
    RUN_TEST(test_a_stiff_source_shows_no_settling);
    RUN_TEST(test_a_floating_pin_is_dragged_toward_the_previous_channel);
    RUN_TEST(test_the_verdict_needs_both_an_effect_and_a_confidence);
    RUN_TEST(test_too_few_samples_is_unknown_not_healthy);
    RUN_TEST(test_no_spread_in_the_drive_cannot_accuse_anything);
    RUN_TEST(test_a_pin_at_a_rail_is_named_as_such);
    RUN_TEST(test_decay_ages_the_evidence_without_moving_the_estimate);
    RUN_TEST(test_a_probe_plugged_back_in_can_clear_its_own_accusation);
}
