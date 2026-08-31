#include "core/probe_health.h"
#include <unity.h>

// One test: how far the reading jumps between CONSECUTIVE conversions. A
// floating pin does nothing else; soil cannot, however fast it is watered.
//
// Everything else this module once did — naming a shorted pin, a
// dead-but-driving module, a quiet floating one by its source impedance — was
// removed after it accused working hardware. Two of the three probes on the
// bench were healthy and two of the three verdicts were wrong.

static const uint32_t MIN_SAMPLES = 30;
static const double MAX_STEP_SD = 400.0;

static int
verdict(const ProbeHealth& h)
{
    return probeHealthVerdict(h, MIN_SAMPLES, MAX_STEP_SD);
}

static void
test_a_connected_probe_barely_moves_between_readings()
{
    // Level noise of a few counts, which is what the connected probes on this
    // board actually show (level sd 40-80).
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 200; ++i) {
        probeHealthAdd(h, 2000 + (int)(i % 5) * 20);
    }

    TEST_ASSERT_TRUE(probeHealthStepSd(h) < MAX_STEP_SD);
    TEST_ASSERT_EQUAL_INT(PROBE_CONNECTED, verdict(h));
}

static void
test_a_floating_pin_swings_between_consecutive_readings()
{
    // Measured on this board: unplugged probes swinging 0.00 to 100.00 within
    // seconds, against a connected luminosity channel at sd 0.14.
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 200; ++i) {
        probeHealthAdd(h, (i % 2) ? 4000 : 90);
    }

    TEST_ASSERT_TRUE(probeHealthStepSd(h) > MAX_STEP_SD);
    TEST_ASSERT_EQUAL_INT(PROBE_NOISY, verdict(h));
}

static void
test_one_large_real_change_is_not_a_fault()
{
    // The false positive that forced this statistic. A healthy probe lifted
    // out of wet soil into the air went 287 -> 4095 counts, and the spread of
    // the LEVEL called it noisy at sd 1889. One step of the full span among
    // 300 readings contributes S/sqrt(N), which stays well under the line.
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 300; ++i) {
        probeHealthAdd(h, (i < 150) ? 287 : 4095);
    }

    TEST_ASSERT_TRUE(probeHealthStepSd(h) < MAX_STEP_SD);
    TEST_ASSERT_EQUAL_INT(PROBE_CONNECTED, verdict(h));
}

static void
test_a_watering_is_not_a_fault()
{
    // 1200 counts over five minutes at 1 Hz: four counts per reading. If this
    // ever fires, the most important event in the system reads as a fault.
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 300; ++i) {
        probeHealthAdd(h, 1000 + (int)(i * 4));
    }

    TEST_ASSERT_TRUE(probeHealthStepSd(h) < 10.0);
    TEST_ASSERT_EQUAL_INT(PROBE_CONNECTED, verdict(h));
}

static void
test_a_probe_at_a_rail_is_not_accused()
{
    // A capacitive module in air reads full scale and is indistinguishable
    // from a pin shorted to 3V3 — both perfectly still. Calling either one a
    // fault means calling a probe out of its pot a fault, which happened.
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 200; ++i) {
        probeHealthAdd(h, 4095);
    }

    TEST_ASSERT_EQUAL_INT(PROBE_CONNECTED, verdict(h));
}

static void
test_too_few_readings_is_unknown_not_healthy()
{
    // The difference that matters to somebody reading the page: "nobody has
    // looked yet" and "it was looked at and is fine" are not the same claim.
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 10; ++i) {
        probeHealthAdd(h, (i % 2) ? 4000 : 90);
    }

    TEST_ASSERT_EQUAL_INT(PROBE_UNKNOWN, verdict(h));
}

static void
test_decay_ages_the_evidence_without_moving_the_estimate()
{
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 200; ++i) {
        probeHealthAdd(h, (i % 2) ? 4000 : 90);
    }

    const double before = probeHealthStepSd(h);
    const double nBefore = h.n;
    probeHealthDecay(h, 0.5);

    TEST_ASSERT_DOUBLE_WITHIN(1e-6, before, probeHealthStepSd(h));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, nBefore * 0.5, h.n);
}

static void
test_decay_keeps_the_last_reading_as_the_next_baseline()
{
    // Clearing prevRaw on decay would manufacture one enormous step out of
    // nothing every time the evidence ages, which on a probe sitting at a rail
    // is the difference between silence and a fabricated accusation.
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 200; ++i) {
        probeHealthAdd(h, 4095);
    }
    probeHealthDecay(h, 0.5);
    for (unsigned i = 0; i < 200; ++i) {
        probeHealthAdd(h, 4095);
    }

    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, probeHealthStepSd(h));
    TEST_ASSERT_EQUAL_INT(PROBE_CONNECTED, verdict(h));
}

static void
test_a_probe_plugged_back_in_clears_its_own_accusation()
{
    // Decay is what makes the verdict recoverable. Without it a probe that was
    // unplugged for an hour would carry the finding for the life of the boot.
    ProbeHealth h;
    probeHealthReset(h);
    for (unsigned i = 0; i < 200; ++i) {
        probeHealthAdd(h, (i % 2) ? 4000 : 90);
    }
    TEST_ASSERT_EQUAL_INT(PROBE_NOISY, verdict(h));

    for (int run = 0; run < 20; ++run) {
        probeHealthDecay(h, 0.5);
        for (unsigned i = 0; i < 50; ++i) {
            probeHealthAdd(h, 2000 + (int)(i % 5) * 20);
        }
    }

    TEST_ASSERT_TRUE(probeHealthStepSd(h) < MAX_STEP_SD);
    TEST_ASSERT_EQUAL_INT(PROBE_CONNECTED, verdict(h));
}

void
run_probe_health_tests(void)
{
    RUN_TEST(test_a_connected_probe_barely_moves_between_readings);
    RUN_TEST(test_a_floating_pin_swings_between_consecutive_readings);
    RUN_TEST(test_one_large_real_change_is_not_a_fault);
    RUN_TEST(test_a_watering_is_not_a_fault);
    RUN_TEST(test_a_probe_at_a_rail_is_not_accused);
    RUN_TEST(test_too_few_readings_is_unknown_not_healthy);
    RUN_TEST(test_decay_ages_the_evidence_without_moving_the_estimate);
    RUN_TEST(test_decay_keeps_the_last_reading_as_the_next_baseline);
    RUN_TEST(test_a_probe_plugged_back_in_clears_its_own_accusation);
}
