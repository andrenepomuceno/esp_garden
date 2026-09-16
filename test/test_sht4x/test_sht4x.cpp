#include "core/sht4x_protocol.h"
#include <unity.h>

// The SHT4x wire protocol's arithmetic: the CRC that guards every frame, the
// two conversions, and the plausibility gate.
//
// WHAT THIS IS EVIDENCE FOR, and what it is not. It is evidence that this
// firmware computes Sensirion's CRC-8 and Sensirion's conversion formulae,
// because the CRC is checked against the vendor's own published check value and
// the conversions against the endpoints their formulae define. It is NOT
// evidence that a real SHT40 answers at 0x44, that it ACKs 0xFD, or that 8.2 ms
// is long enough to wait — those are read off a datasheet and no part has ever
// been on a bus. Nothing in test/ can close that gap; only a board can.

// ---------------------------------------------------------------------------
// The CRC
// ---------------------------------------------------------------------------

// Sensirion publish this pair as the check value for the SHT4x's CRC-8:
// polynomial 0x31, initialisation 0xFF, no reflection, no final XOR. It is the
// one number here that comes from outside this repo, which is what makes it the
// anchor every frame below is built on — the decode tests compute their CRCs
// with the same function, so without this they would only prove the code agrees
// with itself.
static void
test_the_crc_matches_the_datasheets_check_value(void)
{
    const uint8_t data[] = { 0xBE, 0xEF };
    TEST_ASSERT_EQUAL_UINT8(0x92, sht4x::crc8(data, 2));
}

// A different initialisation is the classic way to get this wrong, and it
// happens to agree on some inputs. 0x00 in, 0xFF init gives a value that a
// 0x00-init implementation cannot produce.
static void
test_the_crc_is_seeded_with_ff_and_not_zero(void)
{
    const uint8_t zeros[] = { 0x00, 0x00 };
    TEST_ASSERT_NOT_EQUAL_UINT8(0x00, sht4x::crc8(zeros, 2));
}

// ---------------------------------------------------------------------------
// The conversions
// ---------------------------------------------------------------------------

// Both endpoints are exact by construction, and they are worth pinning because
// they are also the numbers the plausibility gate is reasoned against: the
// conversion can produce -45..130 C and -6..119 %RH and nothing outside that,
// so any bound inside those spans is a real filter and any bound outside one is
// dead code.
static void
test_the_temperature_conversion_hits_both_endpoints(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -45.0f, sht4x::rawToCelsius(0x0000));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 130.0f, sht4x::rawToCelsius(0xFFFF));
}

static void
test_the_humidity_conversion_hits_both_endpoints(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -6.0f, sht4x::rawToHumidityPct(0x0000));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 119.0f, sht4x::rawToHumidityPct(0xFFFF));
}

// 0x6666 is 26214, which is exactly two fifths of 65535 — so both formulae land
// on a round number and a transposed coefficient cannot hide in the rounding.
static void
test_a_midscale_code_converts_to_the_arithmetic_answer(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, sht4x::rawToCelsius(0x6666));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 44.0f, sht4x::rawToHumidityPct(0x6666));
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

static void
buildFrame(uint8_t frame[6], uint16_t rawT, uint16_t rawH)
{
    frame[0] = (uint8_t)(rawT >> 8);
    frame[1] = (uint8_t)(rawT & 0xFF);
    frame[2] = sht4x::crc8(frame, 2);
    frame[3] = (uint8_t)(rawH >> 8);
    frame[4] = (uint8_t)(rawH & 0xFF);
    frame[5] = sht4x::crc8(frame + 3, 2);
}

static void
test_a_good_frame_decodes_both_channels(void)
{
    uint8_t frame[6];
    buildFrame(frame, 0x6666, 0x6666);

    float celsius = -999.0f;
    float humidity = -999.0f;
    TEST_ASSERT_EQUAL_INT(sht4x::kOk,
                          sht4x::decodeFrame(frame, celsius, humidity));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, celsius);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 44.0f, humidity);
}

// The two CRCs are independent, and the verdict has to say WHICH half is
// corrupt. A driver that reported only "bad frame" would leave an operator
// unable to tell a noisy bus from a half-dead part.
static void
test_a_corrupt_temperature_word_is_named_as_such(void)
{
    uint8_t frame[6];
    buildFrame(frame, 0x6666, 0x6666);
    frame[1] ^= 0x01;

    float celsius = 0.0f, humidity = 0.0f;
    TEST_ASSERT_EQUAL_INT(sht4x::kCrcTemperature,
                          sht4x::decodeFrame(frame, celsius, humidity));
}

static void
test_a_corrupt_humidity_word_is_named_as_such(void)
{
    uint8_t frame[6];
    buildFrame(frame, 0x6666, 0x6666);
    frame[4] ^= 0x80;

    float celsius = 0.0f, humidity = 0.0f;
    TEST_ASSERT_EQUAL_INT(sht4x::kCrcHumidity,
                          sht4x::decodeFrame(frame, celsius, humidity));
}

// Neither output may be touched on a failure. A caller that ignored the return
// value would otherwise average half a corrupt frame into the window every
// dashboard and every stored point is drawn from — which is precisely the shape
// of the DHT fault this project spent an archive diagnosing.
static void
test_a_rejected_frame_leaves_both_outputs_alone(void)
{
    uint8_t frame[6];
    buildFrame(frame, 0x6666, 0x6666);
    frame[2] ^= 0xFF;

    float celsius = 12.5f;
    float humidity = 34.5f;
    sht4x::decodeFrame(frame, celsius, humidity);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.5f, celsius);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 34.5f, humidity);
}

// A bus stuck at either rail produces a code at the end of the conversion's
// span. It has to pass CRC to get this far, which is unlikely — but "unlikely"
// is not a gate, and this is the one thing the plausibility bounds can catch
// that the CRC cannot.
static void
test_a_railed_code_is_refused_at_both_ends(void)
{
    uint8_t frame[6];
    float celsius = 0.0f, humidity = 0.0f;

    buildFrame(frame, 0x0000, 0x6666); // -45.00 C, below the -40 floor
    TEST_ASSERT_EQUAL_INT(sht4x::kTemperatureImplausible,
                          sht4x::decodeFrame(frame, celsius, humidity));

    buildFrame(frame, 0xFFFF, 0x6666); // +130.00 C, above the 125 ceiling
    TEST_ASSERT_EQUAL_INT(sht4x::kTemperatureImplausible,
                          sht4x::decodeFrame(frame, celsius, humidity));

    buildFrame(frame, 0x6666, 0x0000); // -6.00 %RH, below the -5 floor
    TEST_ASSERT_EQUAL_INT(sht4x::kHumidityImplausible,
                          sht4x::decodeFrame(frame, celsius, humidity));

    buildFrame(frame, 0x6666, 0xFFFF); // +119.00 %RH, above the 105 ceiling
    TEST_ASSERT_EQUAL_INT(sht4x::kHumidityImplausible,
                          sht4x::decodeFrame(frame, celsius, humidity));
}

// THE DHT LESSON, AS A RED TEST.
//
// This project's DHT gate used the part's RATED range, and this garden is in
// Brasilia: 261 genuine archive readings sit under 20 %RH, the minimum is
// 15.00, and every one of them would have been thrown away on the two driest
// afternoons of the record. A rated range describes ACCURACY, not what a sensor
// can physically report. Anything that later tries to tighten these bounds
// toward the SHT40's own +-1.8 %RH specification has to argue with this.
static void
test_real_brasilia_dry_season_air_is_not_rejected(void)
{
    uint8_t frame[6];
    buildFrame(frame, 29585, 11010); // ~34.0 C, ~15.0 %RH

    float celsius = 0.0f, humidity = 0.0f;
    TEST_ASSERT_EQUAL_INT(sht4x::kOk,
                          sht4x::decodeFrame(frame, celsius, humidity));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 34.0f, celsius);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 15.0f, humidity);
}

// The same argument at the other end. 45.04 C is the hottest reading in this
// project's whole archive — a DHT in 38 minutes of direct sun on 2026-09-03 —
// and it is a real measurement of a real enclosure, kept rather than clamped.
// A sun-baked box goes further than any rated range allows.
static void
test_a_sun_baked_enclosure_reading_is_not_rejected(void)
{
    uint8_t frame[6];
    buildFrame(frame, 33725, 0x6666); // ~45.04 C

    float celsius = 0.0f, humidity = 0.0f;
    TEST_ASSERT_EQUAL_INT(sht4x::kOk,
                          sht4x::decodeFrame(frame, celsius, humidity));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 45.04f, celsius);
}

// A part slightly out of its own calibration reports past 100 %RH. Sensirion
// scale the conversion beyond the physical range on purpose and instruct
// clipping to 0..100, so the reading is ACCEPTED and rounded — not discarded.
//
// That is a different act from the DHT mistake. That one threw away readings
// whose true value was inside the range. This bounds a value at the limit of
// the quantity itself: relative humidity at an instrument cannot exceed 100,
// because the instrument condenses first.
static void
test_humidity_just_over_a_hundred_is_clipped_and_not_refused(void)
{
    uint8_t frame[6];
    buildFrame(frame, 0x6666, 56622); // ~102.0 %RH before the clip

    float celsius = 0.0f, humidity = 0.0f;
    TEST_ASSERT_EQUAL_INT(sht4x::kOk,
                          sht4x::decodeFrame(frame, celsius, humidity));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, humidity);
}

static void
test_humidity_far_over_a_hundred_is_refused_rather_than_clipped(void)
{
    uint8_t frame[6];
    buildFrame(frame, 0x6666, 58720); // ~106.0 %RH, past the +5 margin

    float celsius = 0.0f, humidity = 0.0f;
    TEST_ASSERT_EQUAL_INT(sht4x::kHumidityImplausible,
                          sht4x::decodeFrame(frame, celsius, humidity));
}

// The gate is the part's OPERATING range, so it cannot reject weather or an
// enclosure. Stated as a property rather than left implicit in two constants,
// because the constants are the thing somebody will later be tempted to narrow.
static void
test_the_bounds_cannot_reject_any_terrestrial_air(void)
{
    // Colder than the lowest temperature ever recorded on earth (-89.2 C) is
    // not required; colder than anything a garden sees, comfortably, is.
    TEST_ASSERT_TRUE(sht4x::kMinTempC <= -40.0f);
    // Hotter than any air, and hotter than the 45.04 C enclosure above.
    TEST_ASSERT_TRUE(sht4x::kMaxTempC >= 100.0f);
    // The driest air ever recorded is around 1 %RH; the floor must sit under
    // anything real and above the conversion's own -6.
    TEST_ASSERT_TRUE(sht4x::kMinHumidityPct < 0.0f);
    TEST_ASSERT_TRUE(sht4x::kMinHumidityPct > -6.0f);
    TEST_ASSERT_TRUE(sht4x::kMaxHumidityPct > 100.0f);
    TEST_ASSERT_TRUE(sht4x::kMaxHumidityPct < 119.0f);
}

// ---------------------------------------------------------------------------
// Addressing
// ---------------------------------------------------------------------------

// Bounded rather than free, so a typo lands as a refused config line instead of
// a bus that answers nothing with no clue why. 0x44/0x45/0x46 are the A, B and
// C suffixes of the family; the carrier fits the A.
static void
test_only_the_three_sht4x_addresses_are_accepted(void)
{
    TEST_ASSERT_TRUE(sht4x::addressIsValid(0x44));
    TEST_ASSERT_TRUE(sht4x::addressIsValid(0x45));
    TEST_ASSERT_TRUE(sht4x::addressIsValid(0x46));
    TEST_ASSERT_FALSE(sht4x::addressIsValid(0x43));
    TEST_ASSERT_FALSE(sht4x::addressIsValid(0x47));
    // A decimal 44 typed where 0x44 was meant is the realistic mistake, and it
    // is not a valid 7-bit address for this part.
    TEST_ASSERT_FALSE(sht4x::addressIsValid(44));
    TEST_ASSERT_FALSE(sht4x::addressIsValid(-1));
    TEST_ASSERT_FALSE(sht4x::addressIsValid(256));
    TEST_ASSERT_EQUAL_UINT8(0x44, (uint8_t)sht4x::kDefaultAddress);
}

// The wait is the datasheet's MAXIMUM for high repeatability (8.2 ms) rounded
// up to delay()'s 1 ms resolution. Reading early costs a whole tick to a NACK,
// so the margin is cheaper than the retry — but a value that drifted below 9
// would be reading early every time.
static void
test_the_measurement_wait_covers_the_datasheet_maximum(void)
{
    TEST_ASSERT_TRUE(sht4x::kMeasureMaxMs >= 9);
    TEST_ASSERT_EQUAL_UINT8(6, (uint8_t)sht4x::kFrameBytes);
    TEST_ASSERT_EQUAL_UINT8(0xFD, (uint8_t)sht4x::kCmdMeasureHigh);
}

void
run_sht4x_tests(void)
{
    RUN_TEST(test_the_crc_matches_the_datasheets_check_value);
    RUN_TEST(test_the_crc_is_seeded_with_ff_and_not_zero);
    RUN_TEST(test_the_temperature_conversion_hits_both_endpoints);
    RUN_TEST(test_the_humidity_conversion_hits_both_endpoints);
    RUN_TEST(test_a_midscale_code_converts_to_the_arithmetic_answer);
    RUN_TEST(test_a_good_frame_decodes_both_channels);
    RUN_TEST(test_a_corrupt_temperature_word_is_named_as_such);
    RUN_TEST(test_a_corrupt_humidity_word_is_named_as_such);
    RUN_TEST(test_a_rejected_frame_leaves_both_outputs_alone);
    RUN_TEST(test_a_railed_code_is_refused_at_both_ends);
    RUN_TEST(test_real_brasilia_dry_season_air_is_not_rejected);
    RUN_TEST(test_a_sun_baked_enclosure_reading_is_not_rejected);
    RUN_TEST(test_humidity_just_over_a_hundred_is_clipped_and_not_refused);
    RUN_TEST(test_humidity_far_over_a_hundred_is_refused_rather_than_clipped);
    RUN_TEST(test_the_bounds_cannot_reject_any_terrestrial_air);
    RUN_TEST(test_only_the_three_sht4x_addresses_are_accepted);
    RUN_TEST(test_the_measurement_wait_covers_the_datasheet_maximum);
}
