#include "core/moisture_classifier.h"
#include <math.h>
#include <unity.h>

void
setUp()
{
}
void
tearDown()
{
}

// Thresholds the device uses, so the tests exercise the real gates.
static const double MIN_WEIGHT = 20.0;
static const double MIN_SEPARATION = 4.0;

static void
fill(GaussianStats& stats, double mean, double spread, int samples)
{
    gaussianReset(stats);
    for (int i = 0; i < samples; ++i) {
        // Symmetric around the mean so the fitted mean is exactly `mean`.
        const double offset = spread * ((i % 2 == 0) ? 1.0 : -1.0);
        gaussianAdd(stats, mean + offset);
    }
}

// A model that looks like a real probe: dry near 30, humid near 45, wet near 60.
static void
buildGoodModel(GaussianStats classes[MOISTURE_CLASS_COUNT])
{
    fill(classes[MOISTURE_DRY], 30.0, 1.5, 40);
    fill(classes[MOISTURE_HUMID], 45.0, 2.0, 200);
    fill(classes[MOISTURE_WET], 60.0, 1.5, 40);
}

static void
test_mean_and_variance_come_out_of_the_sufficient_statistics()
{
    GaussianStats stats;
    fill(stats, 50.0, 2.0, 100);

    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 50.0, gaussianMean(stats));
    // Every sample sits exactly 2.0 from the mean, so the variance is 4.
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 4.0, gaussianVariance(stats));
}

static void
test_variance_is_floored_rather_than_zero_or_negative()
{
    GaussianStats stats;
    gaussianReset(stats);
    for (int i = 0; i < 50; ++i) {
        gaussianAdd(stats, 42.0); // identical samples: true variance is 0
    }

    // Zero variance would make this class's log-likelihood infinite and win
    // every comparison regardless of the reading.
    TEST_ASSERT_TRUE(gaussianVariance(stats) > 0.0);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, g_gaussianVarianceFloor,
                              gaussianVariance(stats));

    GaussianStats empty;
    gaussianReset(empty);
    TEST_ASSERT_TRUE(gaussianVariance(empty) > 0.0);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, gaussianMean(empty));
}

static void
test_decay_ages_confidence_without_moving_the_estimate()
{
    GaussianStats stats;
    fill(stats, 40.0, 3.0, 100);

    const double meanBefore = gaussianMean(stats);
    const double varianceBefore = gaussianVariance(stats);

    gaussianDecay(stats, 0.5);

    // This is the whole point of decaying all three moments together: the soil
    // model is unchanged, there is simply half as much evidence for it.
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, meanBefore, gaussianMean(stats));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, varianceBefore, gaussianVariance(stats));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 50.0, stats.weight);
}

static void
test_a_well_separated_model_classifies_each_band()
{
    GaussianStats classes[MOISTURE_CLASS_COUNT];
    buildGoodModel(classes);

    TEST_ASSERT_TRUE(
      moistureModelIsUsable(classes, MIN_WEIGHT, MIN_SEPARATION));

    double confidence = 0.0;
    TEST_ASSERT_EQUAL_INT(
      MOISTURE_DRY,
      moistureClassify(classes, 30.0, MIN_WEIGHT, MIN_SEPARATION, &confidence));
    TEST_ASSERT_TRUE(confidence > 0.9);

    TEST_ASSERT_EQUAL_INT(MOISTURE_HUMID,
                          moistureClassify(classes, 45.0, MIN_WEIGHT,
                                           MIN_SEPARATION, &confidence));
    TEST_ASSERT_TRUE(confidence > 0.9);

    TEST_ASSERT_EQUAL_INT(
      MOISTURE_WET,
      moistureClassify(classes, 60.0, MIN_WEIGHT, MIN_SEPARATION, &confidence));
    TEST_ASSERT_TRUE(confidence > 0.9);
}

static void
test_confidence_tracks_how_much_the_classes_overlap()
{
    // Two models that both pass the gate, one tight and one overlapping. The
    // number attached to a badge has to mean something: on a probe whose bands
    // genuinely overlap, a reading near the boundary must not be reported with
    // the same confidence as one sitting on a mean.
    GaussianStats tight[MOISTURE_CLASS_COUNT];
    buildGoodModel(tight); // means 15 apart, spreads 1.5-2.0

    GaussianStats loose[MOISTURE_CLASS_COUNT];
    fill(loose[MOISTURE_DRY], 35.0, 5.0, 60);
    fill(loose[MOISTURE_HUMID], 45.0, 5.0, 200);
    fill(loose[MOISTURE_WET], 55.0, 5.0, 60);

    TEST_ASSERT_TRUE(moistureModelIsUsable(loose, MIN_WEIGHT, MIN_SEPARATION));
    TEST_ASSERT_TRUE(moistureSeparation(tight) > moistureSeparation(loose));

    double tightAtMean = 0.0, looseAtMean = 0.0, looseAtBoundary = 0.0;
    moistureClassify(tight, 45.0, MIN_WEIGHT, MIN_SEPARATION, &tightAtMean);
    moistureClassify(loose, 45.0, MIN_WEIGHT, MIN_SEPARATION, &looseAtMean);
    moistureClassify(loose, 40.0, MIN_WEIGHT, MIN_SEPARATION, &looseAtBoundary);

    // Better separation, more confidence, at the same reading.
    TEST_ASSERT_TRUE(tightAtMean > looseAtMean);
    // And within one model, the boundary is less certain than the mean.
    TEST_ASSERT_TRUE(looseAtBoundary < looseAtMean);

    // A posterior is a probability over three classes: never above 1, and
    // never below 1/3, which is what a perfect three-way tie would give.
    TEST_ASSERT_TRUE(looseAtBoundary > 1.0 / 3.0);
    TEST_ASSERT_TRUE(tightAtMean <= 1.0);
}

static void
test_an_unseparated_model_is_refused_rather_than_guessed()
{
    // A probe watered so often it never dries: all three classes land on top
    // of each other. This is the case the whole gate exists for.
    GaussianStats classes[MOISTURE_CLASS_COUNT];
    fill(classes[MOISTURE_DRY], 44.0, 2.0, 100);
    fill(classes[MOISTURE_HUMID], 45.0, 2.0, 100);
    fill(classes[MOISTURE_WET], 46.0, 2.0, 100);

    TEST_ASSERT_TRUE(moistureSeparation(classes) < MIN_SEPARATION);
    TEST_ASSERT_FALSE(
      moistureModelIsUsable(classes, MIN_WEIGHT, MIN_SEPARATION));
    TEST_ASSERT_EQUAL_INT(MOISTURE_UNKNOWN,
                          moistureClassify(classes, 45.0, MIN_WEIGHT,
                                           MIN_SEPARATION, nullptr));
}

static void
test_out_of_order_means_are_refused()
{
    // Humid outside the dry..wet span means the labelling contradicts the
    // physics that produced it. Separation alone would happily pass this.
    GaussianStats classes[MOISTURE_CLASS_COUNT];
    fill(classes[MOISTURE_DRY], 30.0, 1.0, 100);
    fill(classes[MOISTURE_HUMID], 80.0, 1.0, 100); // above wet
    fill(classes[MOISTURE_WET], 60.0, 1.0, 100);

    TEST_ASSERT_TRUE(moistureSeparation(classes) > MIN_SEPARATION);
    TEST_ASSERT_FALSE(
      moistureModelIsUsable(classes, MIN_WEIGHT, MIN_SEPARATION));
}

static void
test_inverted_polarity_is_accepted()
{
    // A probe or conversion where a wetter soil reads LOWER. Nothing in the
    // model may assume the sign, the same way the two-point calibration does
    // not assume it.
    GaussianStats classes[MOISTURE_CLASS_COUNT];
    fill(classes[MOISTURE_DRY], 60.0, 1.5, 40);
    fill(classes[MOISTURE_HUMID], 45.0, 2.0, 200);
    fill(classes[MOISTURE_WET], 30.0, 1.5, 40);

    TEST_ASSERT_TRUE(
      moistureModelIsUsable(classes, MIN_WEIGHT, MIN_SEPARATION));
    TEST_ASSERT_EQUAL_INT(
      MOISTURE_WET,
      moistureClassify(classes, 30.0, MIN_WEIGHT, MIN_SEPARATION, nullptr));
    TEST_ASSERT_EQUAL_INT(
      MOISTURE_DRY,
      moistureClassify(classes, 60.0, MIN_WEIGHT, MIN_SEPARATION, nullptr));
}

static void
test_too_little_evidence_is_refused_however_clean_it_looks()
{
    // Perfectly separated, but built from three samples per class. A model
    // fitted to one watering cycle is a description of that cycle.
    GaussianStats classes[MOISTURE_CLASS_COUNT];
    fill(classes[MOISTURE_DRY], 30.0, 1.0, 3);
    fill(classes[MOISTURE_HUMID], 45.0, 1.0, 3);
    fill(classes[MOISTURE_WET], 60.0, 1.0, 3);

    TEST_ASSERT_TRUE(moistureSeparation(classes) > MIN_SEPARATION);
    TEST_ASSERT_FALSE(
      moistureModelIsUsable(classes, MIN_WEIGHT, MIN_SEPARATION));
    TEST_ASSERT_EQUAL_INT(MOISTURE_UNKNOWN,
                          moistureClassify(classes, 45.0, MIN_WEIGHT,
                                           MIN_SEPARATION, nullptr));
}

static void
test_the_disconnected_probe_reading_is_a_visible_outlier()
{
    // A floating input reads at a rail. The z-score is how training rejects
    // those before they drag a class mean to a value the soil never had — the
    // exact component BIC found when the history was clustered blind.
    GaussianStats stats;
    fill(stats, 45.0, 2.0, 200);

    TEST_ASSERT_TRUE(moistureZScore(stats, 45.5) < 3.0);
    TEST_ASSERT_TRUE(moistureZScore(stats, 94.0) > 3.0);
    TEST_ASSERT_TRUE(moistureZScore(stats, 0.0) > 3.0);
}

static void
test_non_finite_samples_are_dropped_instead_of_poisoning_the_fit()
{
    GaussianStats stats;
    fill(stats, 40.0, 1.0, 100);
    const double weightBefore = stats.weight;

    gaussianAdd(stats, NAN);
    gaussianAdd(stats, INFINITY);
    gaussianAdd(stats, 40.0, 0.0);  // zero weight
    gaussianAdd(stats, 40.0, -1.0); // negative weight

    TEST_ASSERT_DOUBLE_WITHIN(1e-9, weightBefore, stats.weight);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 40.0, gaussianMean(stats));

    GaussianStats classes[MOISTURE_CLASS_COUNT];
    buildGoodModel(classes);
    TEST_ASSERT_EQUAL_INT(MOISTURE_UNKNOWN,
                          moistureClassify(classes, NAN, MIN_WEIGHT,
                                           MIN_SEPARATION, nullptr));
}

static void
test_the_prior_decides_a_reading_the_likelihoods_tie_on()
{
    // Identical spreads, equidistant reading: only the priors differ. Humid
    // has far more accumulated weight because soil spends most of its time
    // there, and that is a real prior rather than an artefact to apologise for.
    GaussianStats classes[MOISTURE_CLASS_COUNT];
    fill(classes[MOISTURE_DRY], 30.0, 2.0, 40);
    fill(classes[MOISTURE_HUMID], 45.0, 2.0, 400);
    fill(classes[MOISTURE_WET], 60.0, 2.0, 40);

    TEST_ASSERT_EQUAL_INT(MOISTURE_HUMID,
                          moistureClassify(classes, 37.5, MIN_WEIGHT,
                                           MIN_SEPARATION, nullptr));
}

int
main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_mean_and_variance_come_out_of_the_sufficient_statistics);
    RUN_TEST(test_variance_is_floored_rather_than_zero_or_negative);
    RUN_TEST(test_decay_ages_confidence_without_moving_the_estimate);
    RUN_TEST(test_a_well_separated_model_classifies_each_band);
    RUN_TEST(test_confidence_tracks_how_much_the_classes_overlap);
    RUN_TEST(test_an_unseparated_model_is_refused_rather_than_guessed);
    RUN_TEST(test_out_of_order_means_are_refused);
    RUN_TEST(test_inverted_polarity_is_accepted);
    RUN_TEST(test_too_little_evidence_is_refused_however_clean_it_looks);
    RUN_TEST(test_the_disconnected_probe_reading_is_a_visible_outlier);
    RUN_TEST(test_non_finite_samples_are_dropped_instead_of_poisoning_the_fit);
    RUN_TEST(test_the_prior_decides_a_reading_the_likelihoods_tie_on);
    return UNITY_END();
}
