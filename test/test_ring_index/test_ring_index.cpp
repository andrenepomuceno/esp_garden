#include "core/ring_index.h"
#include <unity.h>

// Every read of /io_history.bin maps a logical position onto a physical slot
// through ring::slotOf. A wrong answer here does not fail loudly — it reorders
// history and serves it as if nothing happened.

static void
test_before_wrap_is_identity(void)
{
    // 3 of 10 slots used: record 0 is in slot 0, and head == stored.
    for (uint32_t i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_UINT32(i, ring::slotOf(i, 3, 3, 10));
    }
}

static void
test_exactly_full_still_starts_at_zero(void)
{
    // stored == capacity and head wrapped back to 0: the oldest record really
    // is slot 0. This is the boundary the `stored < capacity` test decides.
    TEST_ASSERT_EQUAL_UINT32(0, ring::slotOf(0, 0, 10, 10));
    TEST_ASSERT_EQUAL_UINT32(9, ring::slotOf(9, 0, 10, 10));
}

static void
test_after_wrap_oldest_is_head(void)
{
    // Full buffer, head at 4: slot 4 holds the oldest record, and the newest is
    // the slot just before head.
    TEST_ASSERT_EQUAL_UINT32(4, ring::slotOf(0, 4, 10, 10));
    TEST_ASSERT_EQUAL_UINT32(5, ring::slotOf(1, 4, 10, 10));
    TEST_ASSERT_EQUAL_UINT32(9, ring::slotOf(5, 4, 10, 10));
    TEST_ASSERT_EQUAL_UINT32(0, ring::slotOf(6, 4, 10, 10)); // wraps
    TEST_ASSERT_EQUAL_UINT32(3, ring::slotOf(9, 4, 10, 10)); // newest
}

static void
test_sequence_is_a_permutation(void)
{
    // Reading every logical position must touch every slot exactly once.
    // A subtly wrong modulo passes the spot checks above and still repeats or
    // skips a slot here.
    const uint16_t capacity = 7;
    const uint32_t head = 3;
    bool seen[7] = { false };

    for (uint32_t i = 0; i < capacity; ++i) {
        const uint32_t slot = ring::slotOf(i, head, capacity, capacity);
        TEST_ASSERT_LESS_THAN_UINT32(capacity, slot);
        TEST_ASSERT_FALSE(seen[slot]);
        seen[slot] = true;
    }
    for (uint16_t s = 0; s < capacity; ++s) {
        TEST_ASSERT_TRUE(seen[s]);
    }
}

static void
test_capacity_one(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, ring::slotOf(0, 0, 1, 1));
    TEST_ASSERT_EQUAL_UINT32(0, ring::slotOf(0, 0, 0, 1));
}

static void
test_zero_capacity_does_not_divide_by_zero(void)
{
    // Reached when the buffer is disabled by config (records = 0). Without the
    // guard this is a modulo by zero, which on the device is a reset.
    TEST_ASSERT_EQUAL_UINT32(0, ring::slotOf(5, 0, 0, 0));
}

void
run_ring_index_tests(void)
{
    RUN_TEST(test_before_wrap_is_identity);
    RUN_TEST(test_exactly_full_still_starts_at_zero);
    RUN_TEST(test_after_wrap_oldest_is_head);
    RUN_TEST(test_sequence_is_a_permutation);
    RUN_TEST(test_capacity_one);
    RUN_TEST(test_zero_capacity_does_not_divide_by_zero);
}
