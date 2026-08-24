#include "core/fw_version.h"
#include <unity.h>

void
setUp()
{
}
void
tearDown()
{
}

static void
test_equal_versions_do_not_trigger_an_update()
{
    TEST_ASSERT_EQUAL_INT(0, fwVersionCompare("2.0.0", "2.0.0"));
    TEST_ASSERT_FALSE(fwVersionDiffers("2.0.0", "2.0.0"));
}

static void
test_missing_components_read_as_zero()
{
    // The device compiles FW_VERSION as "2.0.0"; an operator typing "2" into
    // ThingsBoard must not look like a different image.
    TEST_ASSERT_EQUAL_INT(0, fwVersionCompare("2", "2.0.0"));
    TEST_ASSERT_EQUAL_INT(0, fwVersionCompare("2.0", "2.0.0"));
    TEST_ASSERT_FALSE(fwVersionDiffers("2.0", "2.0.0"));
}

static void
test_ordering_is_by_component_not_lexicographic()
{
    // "2.10.0" < "2.9.0" as strings, which is the classic way this goes wrong.
    TEST_ASSERT_EQUAL_INT(1, fwVersionCompare("2.10.0", "2.9.0"));
    TEST_ASSERT_EQUAL_INT(-1, fwVersionCompare("2.9.0", "2.10.0"));
    TEST_ASSERT_EQUAL_INT(1, fwVersionCompare("3.0.0", "2.99.99"));
    TEST_ASSERT_EQUAL_INT(1, fwVersionCompare("2.0.1", "2.0.0"));
}

static void
test_a_downgrade_still_counts_as_different()
{
    // Rolling back is an operator decision made in ThingsBoard, not something
    // the device gets to veto.
    TEST_ASSERT_TRUE(fwVersionDiffers("1.9.0", "2.0.0"));
    TEST_ASSERT_EQUAL_INT(-1, fwVersionCompare("1.9.0", "2.0.0"));
}

static void
test_a_v_prefix_is_accepted()
{
    TEST_ASSERT_EQUAL_INT(0, fwVersionCompare("v2.0.0", "2.0.0"));
    TEST_ASSERT_FALSE(fwVersionDiffers("V2.0.0", "2.0.0"));
}

static void
test_a_suffix_ends_the_version_rather_than_corrupting_it()
{
    int parts[3];
    fwVersionParse("2.1.0-rc1", parts);
    TEST_ASSERT_EQUAL_INT(2, parts[0]);
    TEST_ASSERT_EQUAL_INT(1, parts[1]);
    TEST_ASSERT_EQUAL_INT(0, parts[2]);
}

static void
test_garbage_parses_to_zero_instead_of_reading_off_the_end()
{
    int parts[3];

    fwVersionParse("", parts);
    TEST_ASSERT_EQUAL_INT(0, parts[0]);

    fwVersionParse(nullptr, parts);
    TEST_ASSERT_EQUAL_INT(0, parts[0]);
    TEST_ASSERT_EQUAL_INT(0, parts[1]);
    TEST_ASSERT_EQUAL_INT(0, parts[2]);

    fwVersionParse("not-a-version", parts);
    TEST_ASSERT_EQUAL_INT(0, parts[0]);

    // An empty fw_version attribute must not read as "different from 2.0.0"
    // in a way that flashes something, but it also must not equal it.
    TEST_ASSERT_TRUE(fwVersionDiffers("", "2.0.0"));
}

static void
test_a_negative_component_is_clamped()
{
    int parts[3];
    fwVersionParse("1.-1.0", parts);
    TEST_ASSERT_EQUAL_INT(1, parts[0]);
    TEST_ASSERT_EQUAL_INT(0, parts[1]);
}

int
main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_equal_versions_do_not_trigger_an_update);
    RUN_TEST(test_missing_components_read_as_zero);
    RUN_TEST(test_ordering_is_by_component_not_lexicographic);
    RUN_TEST(test_a_downgrade_still_counts_as_different);
    RUN_TEST(test_a_v_prefix_is_accepted);
    RUN_TEST(test_a_suffix_ends_the_version_rather_than_corrupting_it);
    RUN_TEST(test_garbage_parses_to_zero_instead_of_reading_off_the_end);
    RUN_TEST(test_a_negative_component_is_clamped);
    return UNITY_END();
}
