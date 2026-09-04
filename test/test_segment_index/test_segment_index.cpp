#include "core/segment_index.h"
#include <unity.h>

// The arithmetic that maps a logical history position onto a segment file and
// an offset inside it. It is unit-tested for the same reason ring_index.h was:
// a wrong answer here does not fail loudly, it serves one record in place of
// another and the chart looks plausible.
//
// This replaced the ring because the ring was fatal on LittleFS — see
// core/segment_index.h for the panic and the flash-wear arithmetic.

using namespace segment;

static void
test_capacity_rounds_up_so_the_history_is_never_shorter_than_asked()
{
    // 1440 over 8 is exact.
    TEST_ASSERT_EQUAL_UINT16(180, recordsPerSegment(1440, 8));

    // 1000 over 8 is 125 exactly.
    TEST_ASSERT_EQUAL_UINT16(125, recordsPerSegment(1000, 8));

    // 1001 must round UP: 8 * 125 = 1000 would silently hold one record less
    // than the operator configured, and "history.records" would quietly become
    // a suggestion.
    TEST_ASSERT_EQUAL_UINT16(126, recordsPerSegment(1001, 8));

    // Degenerate inputs return 0 rather than dividing by zero.
    TEST_ASSERT_EQUAL_UINT16(0, recordsPerSegment(1440, 0));
    TEST_ASSERT_EQUAL_UINT16(0, recordsPerSegment(0, 8));
}

static void
test_a_logical_index_lands_in_the_right_segment_and_offset()
{
    // Three segments in use, unequal fills, and deliberately NOT in slot order:
    // slot 2 is the oldest, then slot 0, then slot 1.
    const uint16_t counts[8] = { 10, 4, 7, 0, 0, 0, 0, 0 };
    const uint8_t order[8] = { 2, 0, 1 };

    uint8_t slot = 0xFF;
    uint32_t offset = 0xFFFFFFFF;

    // First record of the oldest segment.
    TEST_ASSERT_TRUE(locate(0, counts, order, 3, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(2, slot);
    TEST_ASSERT_EQUAL_UINT32(0, offset);

    // Last record of the oldest segment.
    TEST_ASSERT_TRUE(locate(6, counts, order, 3, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(2, slot);
    TEST_ASSERT_EQUAL_UINT32(6, offset);

    // First record of the next one — the boundary that an off-by-one gets
    // wrong, and the reason the counts are unequal here.
    TEST_ASSERT_TRUE(locate(7, counts, order, 3, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(0, slot);
    TEST_ASSERT_EQUAL_UINT32(0, offset);

    // Somewhere inside the middle segment.
    TEST_ASSERT_TRUE(locate(16, counts, order, 3, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(0, slot);
    TEST_ASSERT_EQUAL_UINT32(9, offset);

    // The newest record of all: 7 + 10 + 4 = 21 records, so index 20.
    TEST_ASSERT_TRUE(locate(20, counts, order, 3, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(1, slot);
    TEST_ASSERT_EQUAL_UINT32(3, offset);
}

static void
test_counts_are_indexed_by_slot_not_by_position_in_the_order()
{
    // The trap this function exists to make impossible. `order` is a list of
    // SLOT numbers; `counts` is indexed by slot. Walking `counts[i]` with the
    // loop variable gives right answers only while the slots happen to be in
    // sequence order — which they are on a fresh device and never are again
    // after the first rotation.
    //
    // Slot 5 holds 3 records and is the oldest; slot 1 holds 100 and is newer.
    // Reading counts[0] and counts[1] instead would put index 3 inside the
    // first segment rather than at the start of the second.
    const uint16_t counts[8] = { 0, 100, 0, 0, 0, 3, 0, 0 };
    const uint8_t order[8] = { 5, 1 };

    uint8_t slot = 0xFF;
    uint32_t offset = 0xFFFFFFFF;

    TEST_ASSERT_TRUE(locate(2, counts, order, 2, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(5, slot);
    TEST_ASSERT_EQUAL_UINT32(2, offset);

    TEST_ASSERT_TRUE(locate(3, counts, order, 2, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(1, slot);
    TEST_ASSERT_EQUAL_UINT32(0, offset);
}

static void
test_an_index_past_the_end_is_refused_rather_than_clamped()
{
    // Returning the last record for an out-of-range index would make a caller
    // that walks until failure loop forever, and readDecimatedLocked() is
    // exactly such a caller.
    const uint16_t counts[8] = { 5, 0, 0, 0, 0, 0, 0, 0 };
    const uint8_t order[8] = { 0 };

    uint8_t slot = 0xFF;
    uint32_t offset = 0xFFFFFFFF;

    TEST_ASSERT_TRUE(locate(4, counts, order, 1, slot, offset));
    TEST_ASSERT_FALSE(locate(5, counts, order, 1, slot, offset));
    TEST_ASSERT_FALSE(locate(0xFFFFFFFFUL, counts, order, 1, slot, offset));

    // No segments in use at all: every index is out of range.
    TEST_ASSERT_FALSE(locate(0, counts, order, 0, slot, offset));
}

static void
test_an_empty_segment_in_the_middle_is_stepped_over()
{
    // A segment can be in use with zero records: rotateLocked() stamps the
    // header and the first append can then fail. locate() must skip it rather
    // than resolving an index into a file with nothing in it.
    const uint16_t counts[8] = { 4, 0, 6, 0, 0, 0, 0, 0 };
    const uint8_t order[8] = { 0, 1, 2 };

    uint8_t slot = 0xFF;
    uint32_t offset = 0xFFFFFFFF;

    TEST_ASSERT_TRUE(locate(3, counts, order, 3, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(0, slot);

    TEST_ASSERT_TRUE(locate(4, counts, order, 3, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(2, slot);
    TEST_ASSERT_EQUAL_UINT32(0, offset);
}

static void
test_an_unused_slot_is_claimed_before_anything_is_thrown_away()
{
    // While the history is still filling there is always a free slot, and
    // recycling a written one instead would drop records for no reason.
    uint32_t seq[8] = { 3, 1, 2, 0, 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_UINT8(3, slotToRecycle(seq, 8));
}

static void
test_the_oldest_segment_is_the_one_recycled()
{
    // Full: the smallest sequence number goes, wherever it sits.
    uint32_t seq[8] = { 30, 31, 26, 27, 28, 29, 32, 33 };
    TEST_ASSERT_EQUAL_UINT8(2, slotToRecycle(seq, 8));
}

static void
test_recycling_follows_the_sequence_not_the_clock()
{
    // Sequence numbers only ever increase, which is why they and not
    // timestamps decide what is oldest. A device that syncs NTP for the first
    // time jumps from 1970 to now, so its first segments carry timestamps
    // NEWER than the ones written after the jump would suggest — ordering by
    // record time would recycle the wrong file and scramble the history.
    uint32_t seq[8] = { 5, 6, 7, 8, 9, 10, 11, 12 };
    TEST_ASSERT_EQUAL_UINT8(0, slotToRecycle(seq, 8));

    // And the wrap of the slot numbering is not the wrap of the sequence.
    uint32_t later[8] = { 13, 6, 7, 8, 9, 10, 11, 12 };
    TEST_ASSERT_EQUAL_UINT8(1, slotToRecycle(later, 8));
}

static void
test_the_order_is_rebuilt_oldest_first_skipping_unused_slots()
{
    uint32_t seq[8] = { 30, 0, 26, 0, 28, 0, 0, 33 };
    uint8_t order[8] = {};

    const uint8_t n = buildOrder(seq, 8, order);
    TEST_ASSERT_EQUAL_UINT8(4, n);
    TEST_ASSERT_EQUAL_UINT8(2, order[0]); // seq 26
    TEST_ASSERT_EQUAL_UINT8(4, order[1]); // seq 28
    TEST_ASSERT_EQUAL_UINT8(0, order[2]); // seq 30
    TEST_ASSERT_EQUAL_UINT8(7, order[3]); // seq 33
}

static void
test_a_fresh_device_has_no_order_at_all()
{
    uint32_t seq[8] = {};
    uint8_t order[8] = {};
    TEST_ASSERT_EQUAL_UINT8(0, buildOrder(seq, 8, order));
}

static void
test_a_full_rotation_keeps_the_history_contiguous()
{
    // The property that matters end to end: fill every segment, recycle the
    // oldest, and the logical sequence must still run oldest-to-newest with no
    // gap and no repeat. Simulated over several rotations against a plain
    // model of what the records should be.
    uint32_t seq[8] = {};
    uint16_t counts[8] = {};
    uint8_t order[8] = {};
    uint8_t orderCount = 0;
    uint32_t nextSeq = 1;
    const uint16_t per = 5;

    uint32_t written = 0;  // records ever appended
    uint32_t evicted = 0;  // records dropped by rotation

    for (uint32_t i = 0; i < 200; ++i) {
        bool haveSlot = false;
        uint8_t slot = 0;
        if (orderCount > 0 && counts[order[orderCount - 1]] < per) {
            slot = order[orderCount - 1];
            haveSlot = true;
        }
        if (!haveSlot) {
            slot = slotToRecycle(seq, 8);
            if (seq[slot] != 0) {
                evicted += counts[slot];
            }
            counts[slot] = 0;
            seq[slot] = nextSeq++;
            orderCount = buildOrder(seq, 8, order);
        }
        ++counts[slot];
        ++written;

        // Every logical index resolves, and resolves to the record it should.
        const uint32_t held = written - evicted;
        uint32_t seen = 0;
        for (uint8_t k = 0; k < orderCount; ++k) {
            seen += counts[order[k]];
        }
        TEST_ASSERT_EQUAL_UINT32(held, seen);

        uint8_t s = 0;
        uint32_t off = 0;
        TEST_ASSERT_TRUE(locate(0, counts, order, orderCount, s, off));
        TEST_ASSERT_TRUE(locate(held - 1, counts, order, orderCount, s, off));
        TEST_ASSERT_FALSE(locate(held, counts, order, orderCount, s, off));

        // Retention never falls below (segments - 1) * per once full, which is
        // the bound the header promises.
        if (written >= 8 * per) {
            TEST_ASSERT_TRUE(held >= (uint32_t)(7 * per));
            TEST_ASSERT_TRUE(held <= (uint32_t)(8 * per));
        }
    }
}


// ---------------------------------------------------------------------------
// Does the requested capacity fit? Nothing is preallocated, so a capacity the
// partition cannot hold is accepted at boot, grows for days and only then runs
// the filesystem out from inside append(). These are the checks that stop that
// being decided by a constant somebody re-derived by hand.

// sizeof(IoSegmentHeader) and sizeof(IoRecord). Spelled out rather than
// included, because io_history.h reaches Arduino, FreeRTOS and LittleFS. A
// change on either side moves the numbers below, which is the point: they are
// asserted here so a layout change cannot quietly shrink the history.
static const uint32_t kHeader = 12;
static const uint32_t kRecord = 48;

// Device 6224 on 2026-09-04, from /data.json's "344 / 512 KB" plus the segment
// files 1298 records occupy at 180 records per segment.
static const uint32_t kPartition = 512u * 1024u;
static const uint32_t kFree = 168u * 1024u;
static const uint32_t kOnDisk = 7 * (kHeader + 180 * kRecord) +
                                (kHeader + 38 * kRecord);

static void
test_littlefs_charges_for_whole_blocks_and_the_estimate_says_so()
{
    // 5000 records is 8 * (12 + 625 * 48) = 240 096 bytes of DATA, and that is
    // the number this repo used to reason with. LittleFS allocates whole 4 KB
    // blocks, so each of the eight segments really costs ceil(30012 / 4096) =
    // 8 blocks: 256 KB, not 235. Under-counting by 32 KB is under-counting by
    // a fifth of what was free on the board this was written for.
    TEST_ASSERT_EQUAL_UINT32(262144, storageBytes(5000, kHeader, kRecord));
    TEST_ASSERT_EQUAL_UINT32(98304, storageBytes(1440, kHeader, kRecord));
    TEST_ASSERT_EQUAL_UINT32(0, storageBytes(0, kHeader, kRecord));
}

static void
test_a_capacity_that_fits_is_granted_unchanged()
{
    // The clamp is not allowed to be a quiet reduction of everybody's history:
    // what has been running here for weeks must come back untouched.
    TEST_ASSERT_EQUAL_UINT32(
      1440, fitCapacity(1440, kPartition, kFree, kOnDisk, kHeader, kRecord));
    TEST_ASSERT_EQUAL_UINT32(
      2500, fitCapacity(2500, kPartition, kFree, kOnDisk, kHeader, kRecord));
}

static void
test_the_ceiling_does_not_fit_this_device_and_is_reduced_not_accepted()
{
    // The whole reason the static ceiling cannot be the only check. 5000 is
    // allowed by loadFile() and does not fit here; accepting it would grow the
    // history for about two and a half days and then fill the partition from
    // inside append(), on a board reachable only over the air.
    const uint32_t granted =
      fitCapacity(5000, kPartition, kFree, kOnDisk, kHeader, kRecord);
    TEST_ASSERT_EQUAL_UINT32(3408, granted);
    TEST_ASSERT_TRUE(granted < 5000);
}

static void
test_what_is_granted_actually_fits_with_the_reserve_still_intact()
{
    // The property the whole exercise exists for, checked across the range
    // rather than at one point: whatever comes back must still leave the
    // reserve untouched once every segment has filled.
    const uint32_t available = kFree + kOnDisk;
    const uint32_t reserve = reserveBytes(kPartition);
    for (uint32_t asked = 0; asked <= 5000; asked += 137) {
        const uint32_t granted =
          fitCapacity(asked, kPartition, kFree, kOnDisk, kHeader, kRecord);
        TEST_ASSERT_TRUE(granted <= asked);
        TEST_ASSERT_TRUE(storageBytes(granted, kHeader, kRecord) + reserve <=
                         available);
    }
}

static void
test_cost_and_capacity_are_inverses_of_each_other()
{
    // capacityForBytes() must be the exact inverse of storageBytes(), or the
    // clamp either wastes blocks or hands back a number that does not fit. One
    // more record per segment has to overflow the budget it was derived from.
    for (uint32_t budget = 0; budget <= 512u * 1024u; budget += 4096) {
        const uint32_t fits = capacityForBytes(budget, kHeader, kRecord);
        TEST_ASSERT_TRUE(storageBytes(fits, kHeader, kRecord) <= budget);
        TEST_ASSERT_TRUE(storageBytes(fits + kSegments, kHeader, kRecord) >
                         budget);
    }
}

static void
test_the_grant_is_a_whole_number_of_segments()
{
    // Segments are equal by construction, so a capacity that is not a multiple
    // of eight is a capacity one segment cannot deliver.
    for (uint32_t asked = 1; asked <= 5000; asked += 91) {
        const uint32_t granted =
          fitCapacity(asked, kPartition, kFree, kOnDisk, kHeader, kRecord);
        if (granted != asked) {
            TEST_ASSERT_EQUAL_UINT32(0, granted % kSegments);
        }
    }
}

static void
test_the_history_already_on_disk_is_credited_because_it_is_replaced()
{
    // A device holding 60 KB of segments is not choosing between its old
    // history and a new one; the new capacity takes that space over. Charging
    // for both would shrink the history a little further at every boot.
    const uint32_t withOld =
      fitCapacity(5000, kPartition, kFree, kOnDisk, kHeader, kRecord);
    const uint32_t withoutOld =
      fitCapacity(5000, kPartition, kFree, 0, kHeader, kRecord);
    TEST_ASSERT_TRUE(withOld > withoutOld);
}

static void
test_a_disabled_history_stays_disabled_and_is_never_clamped_up()
{
    // records = 0 disables the feature, and this check must not turn a
    // deliberate 0 into "as much as fits".
    TEST_ASSERT_EQUAL_UINT32(
      0, fitCapacity(0, kPartition, kFree, kOnDisk, kHeader, kRecord));
    TEST_ASSERT_EQUAL_UINT32(
      0, fitCapacity(0, kPartition, kPartition, 0, kHeader, kRecord));
}

static void
test_a_filesystem_that_cannot_be_asked_grants_what_was_configured()
{
    // totalBytes() == 0 is an unmounted or unreadable partition, which is not
    // evidence of a full one. Clamping on it would silently shrink the history
    // of every board whose mount failed, and the static ceiling still applies.
    TEST_ASSERT_EQUAL_UINT32(5000,
                             fitCapacity(5000, 0, 0, 0, kHeader, kRecord));
}

static void
test_a_partition_with_no_room_disables_the_history_rather_than_half_filling_it()
{
    // Below the reserve there is no honest answer but zero: a history that
    // takes the last blocks stops LittleFS being able to write at all,
    // including the writes that would free space.
    TEST_ASSERT_EQUAL_UINT32(
      0, fitCapacity(5000, kPartition, 16u * 1024u, 0, kHeader, kRecord));
    TEST_ASSERT_EQUAL_UINT32(
      0, fitCapacity(5000, kPartition, 0, 0, kHeader, kRecord));
}

static void
test_the_reserve_has_a_floor_and_a_fraction_and_takes_the_larger()
{
    // The floor covers what does not scale with the partition: a 30 KB asset
    // upload plus the /upload.tmp twin it renames from, log backups nothing
    // truncates, and the free blocks a copy-on-write filesystem needs in order
    // to write at all. The fraction covers what does scale: an allocator
    // hunting for free blocks in a partition run down to its last one.
    TEST_ASSERT_EQUAL_UINT32(64u * 1024u, reserveBytes(256u * 1024u));
    TEST_ASSERT_EQUAL_UINT32(64u * 1024u, reserveBytes(512u * 1024u));
    TEST_ASSERT_EQUAL_UINT32(256u * 1024u, reserveBytes(2048u * 1024u));
}

void
run_segment_index_tests(void)
{
    RUN_TEST(test_capacity_rounds_up_so_the_history_is_never_shorter_than_asked);
    RUN_TEST(test_a_logical_index_lands_in_the_right_segment_and_offset);
    RUN_TEST(test_counts_are_indexed_by_slot_not_by_position_in_the_order);
    RUN_TEST(test_an_index_past_the_end_is_refused_rather_than_clamped);
    RUN_TEST(test_an_empty_segment_in_the_middle_is_stepped_over);
    RUN_TEST(test_an_unused_slot_is_claimed_before_anything_is_thrown_away);
    RUN_TEST(test_the_oldest_segment_is_the_one_recycled);
    RUN_TEST(test_recycling_follows_the_sequence_not_the_clock);
    RUN_TEST(test_the_order_is_rebuilt_oldest_first_skipping_unused_slots);
    RUN_TEST(test_a_fresh_device_has_no_order_at_all);
    RUN_TEST(test_a_full_rotation_keeps_the_history_contiguous);
    RUN_TEST(test_littlefs_charges_for_whole_blocks_and_the_estimate_says_so);
    RUN_TEST(test_a_capacity_that_fits_is_granted_unchanged);
    RUN_TEST(test_the_ceiling_does_not_fit_this_device_and_is_reduced_not_accepted);
    RUN_TEST(test_what_is_granted_actually_fits_with_the_reserve_still_intact);
    RUN_TEST(test_cost_and_capacity_are_inverses_of_each_other);
    RUN_TEST(test_the_grant_is_a_whole_number_of_segments);
    RUN_TEST(test_the_history_already_on_disk_is_credited_because_it_is_replaced);
    RUN_TEST(test_a_disabled_history_stays_disabled_and_is_never_clamped_up);
    RUN_TEST(test_a_filesystem_that_cannot_be_asked_grants_what_was_configured);
    RUN_TEST(test_a_partition_with_no_room_disables_the_history_rather_than_half_filling_it);
    RUN_TEST(test_the_reserve_has_a_floor_and_a_fraction_and_takes_the_larger);
}
