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
// CHANGING history.records MUST NOT DESTROY THE HISTORY
//
// A segment's header carries its own capacity, and begin() used to DELETE any
// segment whose capacity disagreed with the one derived from history.records.
// So editing the buffer size wiped every stored record at the next boot.
//
// `recordSize` guards a FORMAT and stays absolute; capacity guards an EVICTION
// POLICY, and a 48-byte record written into a 5-per-segment file is
// byte-identical to one written into a 12-per-segment file. Segments of
// different sizes now coexist and are recycled to the new size as they age
// out.
//
// WHAT THESE TESTS ARE NOT. src/io_history.cpp reaches Arduino, FreeRTOS and
// LittleFS and cannot be built for the host at all, so nothing here executes
// one line of it. The model below is a SECOND implementation of its append and
// rotate decisions, written against the same core/segment_index.h functions
// the firmware calls. What it pins is the arithmetic and the interaction
// between the three functions — which is the half where a wrong answer
// reorders history instead of failing, and the half that gives right answers
// until the first rotation and wrong ones forever after.

namespace {

static const uint8_t kSeg = 8;
static const uint16_t kMaxPer = 32; // biggest per-segment capacity used below

struct History
{
    uint16_t target = 0;                ///< recordsPerSegment() right now
    uint16_t count[kSeg] = {};          ///< records held, by SLOT
    uint16_t cap[kSeg] = {};            ///< each slot's own header, by SLOT
    uint32_t seq[kSeg] = {};
    uint8_t order[kSeg] = {};
    uint8_t orderCount = 0;
    uint32_t nextSeq = 1;
    uint32_t stored = 0;
    uint32_t evicted = 0;
    uint32_t written = 0;               ///< ordinals ever appended
    uint32_t cell[kSeg][kMaxPer] = {};  ///< which ordinal lives where

    // IoHistory::append() plus rotateLocked(), with the filesystem replaced by
    // a count. The fill test is the one line that had to change for mixed
    // sizes: against segmentFillLimit(), not against `target`.
    void append()
    {
        uint8_t slot = 0;
        bool haveSlot = false;
        if (orderCount > 0) {
            const uint8_t newest = order[orderCount - 1];
            if (count[newest] < segmentFillLimit(cap[newest], target)) {
                slot = newest;
                haveSlot = true;
            }
        }
        if (!haveSlot) {
            slot = slotToRecycle(seq, kSeg);
            if (seq[slot] != 0) {
                stored -= count[slot];
                evicted += count[slot];
            }
            count[slot] = 0;
            cap[slot] = target; // a recycled slot is stamped with the new size
            seq[slot] = nextSeq++;
            orderCount = buildOrder(seq, kSeg, order);
        }
        cell[slot][count[slot]] = written;
        ++count[slot];
        ++stored;
        ++written;
    }

    // A reboot: history.records changed, begin() re-adopts the files. `cap`
    // comes from each header and `count` from each file's length, so both
    // survive; only the target moves. evicted/written are per-boot ordinals
    // and are NOT reset here, so the invariant below keeps checking across it.
    void resizeAndReboot(uint16_t newTarget)
    {
        target = newTarget;
        orderCount = buildOrder(seq, kSeg, order);
    }

    uint32_t liveCap() const
    {
        return liveCapacity(cap, count, seq, kSeg, target);
    }
};

// Everything that must be true after every single append. One helper rather
// than scattered assertions, because the failure this guards against is not a
// crash: it is one record served in place of another.
static void
assertConsistent(const History& h)
{
    uint32_t seen = 0;
    for (uint8_t k = 0; k < h.orderCount; ++k) {
        seen += h.count[h.order[k]];
    }
    TEST_ASSERT_EQUAL_UINT32(h.stored, seen);
    TEST_ASSERT_EQUAL_UINT32(h.stored, h.written - h.evicted);

    // The held records are exactly the contiguous run of ordinals ending at
    // the newest, in order. That single property rules out a reorder, a
    // duplicate, a hole and a silent loss at once.
    uint8_t slot = 0xFF;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < h.stored; ++i) {
        TEST_ASSERT_TRUE(locate(i, h.count, h.order, h.orderCount, slot, offset));
        TEST_ASSERT_EQUAL_UINT32(h.written - h.stored + i, h.cell[slot][offset]);
    }
    TEST_ASSERT_FALSE(locate(h.stored, h.count, h.order, h.orderCount, slot,
                             offset));

    // No segment is ever written past its OWN header. A file longer than its
    // header allows loses the tail at the next boot, silently — that is what
    // adoptSegmentLocked() clamps, and it must never have anything to clamp.
    for (uint8_t s = 0; s < kSeg; ++s) {
        if (h.seq[s] != 0) {
            TEST_ASSERT_TRUE(h.count[s] <= h.cap[s]);
        }
    }

    // The /data.json row is "stored / capacity". It may never read as a number
    // over a smaller one.
    TEST_ASSERT_TRUE(h.liveCap() >= h.stored);
}

} // namespace

static void
test_a_segment_is_full_at_the_smaller_of_its_own_header_and_the_new_geometry()
{
    // Growing: the inherited segment stops at its own 5 and is recycled to 12
    // afterwards. Writing 12 into a file whose header says 5 would lose 7
    // records at the next boot.
    TEST_ASSERT_EQUAL_UINT16(5, segmentFillLimit(5, 12));

    // Shrinking: the inherited segment stops at the new 3 even though its own
    // header allows 12. Letting it run to 12 would take space
    // ioHistoryFitCapacity() never credited — it measured the file as it is
    // today — and would delay any reduction in flash use by a whole cycle.
    TEST_ASSERT_EQUAL_UINT16(3, segmentFillLimit(12, 3));

    // Unchanged geometry is the ordinary case and must be exactly what it was.
    TEST_ASSERT_EQUAL_UINT16(180, segmentFillLimit(180, 180));

    // A header claiming a capacity of zero can never be appended to, which is
    // why adoptSegmentLocked() refuses that one value outright rather than
    // keeping a segment nothing can ever write into. (The refusal itself is in
    // src/io_history.cpp and is not reachable from here.)
    TEST_ASSERT_EQUAL_UINT16(0, segmentFillLimit(0, 180));
}

static void
test_growing_the_capacity_keeps_every_record_and_converges_by_rotation()
{
    History h;
    h.target = 5;
    for (uint32_t i = 0; i < 100; ++i) { // well past a full cycle
        h.append();
        assertConsistent(h);
    }
    const uint32_t before = h.stored;
    const uint32_t writtenBefore = h.written;
    TEST_ASSERT_TRUE(before >= 35 && before <= 40);

    // The change. Under the old rule this was the moment every segment was
    // deleted and `stored` went to 0.
    h.resizeAndReboot(12);
    TEST_ASSERT_EQUAL_UINT32(before, h.stored);
    TEST_ASSERT_EQUAL_UINT32(writtenBefore, h.written);
    assertConsistent(h);

    // The first append after the change finds the newest inherited segment
    // already at its own ceiling, so it rotates — dropping the oldest 5, which
    // is exactly what an ordinary rotation would have dropped.
    h.append();
    assertConsistent(h);
    TEST_ASSERT_EQUAL_UINT32(before - 5 + 1, h.stored);

    // Eight rotations and the geometry is uniform again.
    for (uint32_t i = 0; i < 8 * 12; ++i) {
        h.append();
        assertConsistent(h);
    }
    for (uint8_t s = 0; s < kSeg; ++s) {
        TEST_ASSERT_EQUAL_UINT16(12, h.cap[s]);
    }
    TEST_ASSERT_EQUAL_UINT32(96, h.liveCap());
    TEST_ASSERT_TRUE(h.stored >= 7 * 12 && h.stored <= 8 * 12);
}

static void
test_shrinking_the_capacity_evicts_rather_than_deletes()
{
    History h;
    h.target = 12;
    for (uint32_t i = 0; i < 200; ++i) {
        h.append();
        assertConsistent(h);
    }
    const uint32_t before = h.stored;
    TEST_ASSERT_TRUE(before >= 84 && before <= 96);

    // Shrinking is the hard direction: every stored segment can hold four
    // times what the new geometry allows. Nothing is dropped AT THE CHANGE.
    h.resizeAndReboot(3);
    TEST_ASSERT_EQUAL_UINT32(before, h.stored);
    assertConsistent(h);

    // It converges downwards, monotonically, one whole old segment per
    // rotation — which is the cost worth knowing: a rotation during the
    // convergence drops up to 12 records at once, not 3.
    uint32_t previous = h.stored;
    uint32_t biggestDrop = 0;
    for (uint32_t i = 0; i < 8 * 3 + 40; ++i) {
        h.append();
        assertConsistent(h);
        if (h.stored < previous + 1) {
            // An append that rotated: it added one and evicted a whole
            // segment, so the segment it dropped was (previous + 1) - stored.
            const uint32_t drop = (previous + 1) - h.stored;
            if (drop > biggestDrop) {
                biggestDrop = drop;
            }
        }
        TEST_ASSERT_TRUE(h.stored <= previous + 1);
        previous = h.stored;
    }
    TEST_ASSERT_EQUAL_UINT32(12, biggestDrop);

    for (uint8_t s = 0; s < kSeg; ++s) {
        TEST_ASSERT_EQUAL_UINT16(3, h.cap[s]);
    }
    TEST_ASSERT_EQUAL_UINT32(24, h.liveCap());
    TEST_ASSERT_TRUE(h.stored >= 7 * 3 && h.stored <= 8 * 3);
}

static void
test_a_resize_that_is_undone_before_it_converges_still_orders_correctly()
{
    // The case that is easy to get right for one resize and wrong for two:
    // slots then carry three different capacities at once, and the oldest is
    // not slot 0. An operator trying values in /devices.html does exactly this.
    History h;
    h.target = 4;
    for (uint32_t i = 0; i < 60; ++i) {
        h.append();
    }
    h.resizeAndReboot(11);
    for (uint32_t i = 0; i < 25; ++i) {
        h.append();
        assertConsistent(h);
    }
    h.resizeAndReboot(6);
    for (uint32_t i = 0; i < 25; ++i) {
        h.append();
        assertConsistent(h);
    }

    // Three capacities really are live at once, or this test is checking the
    // uniform case with extra steps.
    bool seen4 = false, seen11 = false, seen6 = false;
    for (uint8_t s = 0; s < kSeg; ++s) {
        if (h.seq[s] == 0) {
            continue;
        }
        seen4 = seen4 || h.cap[s] == 4;
        seen11 = seen11 || h.cap[s] == 11;
        seen6 = seen6 || h.cap[s] == 6;
    }
    TEST_ASSERT_TRUE(seen4);
    TEST_ASSERT_TRUE(seen11);
    TEST_ASSERT_TRUE(seen6);

    for (uint32_t i = 0; i < 120; ++i) {
        h.append();
        assertConsistent(h);
    }
    for (uint8_t s = 0; s < kSeg; ++s) {
        TEST_ASSERT_EQUAL_UINT16(6, h.cap[s]);
    }
}

static void
test_the_absolute_walk_survives_a_rotation_between_mixed_size_segments()
{
    // IoHistory::forEach() releases the mutex every 64 records and tracks an
    // ABSOLUTE ordinal, converting with `absolute - evicted()`. With uniform
    // segments a rotation shifted every logical index by one segment; with
    // mixed sizes it shifts by whatever the recycled segment held, which is
    // the number the walk must not assume.
    History h;
    h.target = 11;
    for (uint32_t i = 0; i < 90; ++i) {
        h.append();
    }
    h.resizeAndReboot(4);
    const uint32_t base = h.evicted; // begin() would restart the ordinals here

    // Walk the first few records, then let appends rotate an eleven-record
    // segment out from under it.
    uint32_t absolute = base;
    for (int i = 0; i < 3; ++i) {
        const uint32_t index = absolute - h.evicted;
        uint8_t slot = 0xFF;
        uint32_t offset = 0;
        TEST_ASSERT_TRUE(locate(index, h.count, h.order, h.orderCount, slot,
                                offset));
        TEST_ASSERT_EQUAL_UINT32(absolute, h.cell[slot][offset]);
        ++absolute;
    }

    const uint32_t evictedBefore = h.evicted;
    while (h.evicted == evictedBefore) {
        h.append();
    }
    TEST_ASSERT_EQUAL_UINT32(11, h.evicted - evictedBefore);

    // The three records already visited are gone, and so are eight the walk
    // had not reached. The correction is the one forEach() makes: clamp the
    // absolute ordinal up to evicted() rather than carrying on from where the
    // logical index used to point.
    TEST_ASSERT_TRUE(absolute < h.evicted);
    if (absolute < h.evicted) {
        absolute = h.evicted;
    }
    const uint32_t index = absolute - h.evicted;
    uint8_t slot = 0xFF;
    uint32_t offset = 0;
    TEST_ASSERT_TRUE(locate(index, h.count, h.order, h.orderCount, slot, offset));
    TEST_ASSERT_EQUAL_UINT32(absolute, h.cell[slot][offset]);

    // And the rest of the walk runs to the end with no repeat and no skip.
    uint32_t expected = absolute;
    while (absolute - h.evicted < h.stored) {
        const uint32_t at = absolute - h.evicted;
        TEST_ASSERT_TRUE(locate(at, h.count, h.order, h.orderCount, slot, offset));
        TEST_ASSERT_EQUAL_UINT32(expected, h.cell[slot][offset]);
        ++absolute;
        ++expected;
    }
    TEST_ASSERT_EQUAL_UINT32(h.written, expected);
}

static void
test_locate_crosses_segments_that_hold_different_numbers_of_records()
{
    // The existing coverage uses unequal FILLS, which under one capacity only
    // ever happens to the newest segment. Unequal CAPACITIES put a short
    // segment in the middle of the order, with the slots out of sequence.
    //
    // Slot 6 holds 11 (inherited), slot 3 holds 4, slot 0 holds 2 and is the
    // newest: 17 records.
    const uint16_t counts[8] = { 2, 0, 0, 4, 0, 0, 11, 0 };
    const uint8_t order[8] = { 6, 3, 0 };

    uint8_t slot = 0xFF;
    uint32_t offset = 0xFFFFFFFF;

    TEST_ASSERT_TRUE(locate(10, counts, order, 3, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(6, slot);
    TEST_ASSERT_EQUAL_UINT32(10, offset);

    TEST_ASSERT_TRUE(locate(11, counts, order, 3, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(3, slot);
    TEST_ASSERT_EQUAL_UINT32(0, offset);

    TEST_ASSERT_TRUE(locate(15, counts, order, 3, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(0, slot);
    TEST_ASSERT_EQUAL_UINT32(0, offset);

    TEST_ASSERT_TRUE(locate(16, counts, order, 3, slot, offset));
    TEST_ASSERT_EQUAL_UINT8(0, slot);
    TEST_ASSERT_EQUAL_UINT32(1, offset);

    TEST_ASSERT_FALSE(locate(17, counts, order, 3, slot, offset));
}

static void
test_the_reported_capacity_is_the_product_again_once_the_slots_agree()
{
    // The row /data.json renders. With one capacity everywhere it has to be
    // exactly what `segmentRecords * kSegments` always returned, or this change
    // has quietly redefined a number operators read.
    uint16_t cap[8], counts[8];
    uint32_t seq[8];
    for (uint8_t i = 0; i < 8; ++i) {
        cap[i] = 180;
        counts[i] = (i == 3) ? 42 : 180;
        seq[i] = i + 1;
    }
    TEST_ASSERT_EQUAL_UINT32(1440, liveCapacity(cap, counts, seq, 8, 180));

    // A fresh device: every slot unused, so every slot contributes the size it
    // will be stamped with.
    uint32_t none[8] = {};
    uint16_t zero[8] = {};
    TEST_ASSERT_EQUAL_UINT32(1440, liveCapacity(zero, zero, none, 8, 180));

    // Mid-shrink: four slots still hold 1250 each, four have been recycled to
    // 180. The ceiling is the real one and not the target, so the row never
    // reads as more stored than the buffer can hold.
    for (uint8_t i = 0; i < 4; ++i) {
        cap[i] = 1250;
        counts[i] = 1250;
    }
    for (uint8_t i = 4; i < 8; ++i) {
        cap[i] = 180;
        counts[i] = 180;
    }
    TEST_ASSERT_EQUAL_UINT32(4 * 1250 + 4 * 180,
                             liveCapacity(cap, counts, seq, 8, 180));
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

    // The ceiling ConfigFile::loadFile() carries since 2.20.0. 1250 records per
    // segment is 60 012 B of data, which LittleFS charges 15 blocks for: 480 KB
    // against the 468.8 KB the unrounded arithmetic would claim. The comment on
    // that constant quotes this number and the test is what keeps them equal.
    TEST_ASSERT_EQUAL_UINT32(491520, storageBytes(10000, kHeader, kRecord));
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

    // And raising the ceiling to 10000 changes nothing here, which is the
    // point of having two checks: on a WROOM-32 the DEVICE refuses, not the
    // constant, so the same 3408 comes back for either request.
    TEST_ASSERT_EQUAL_UINT32(
      3408, fitCapacity(10000, kPartition, kFree, kOnDisk, kHeader, kRecord));
}

static void
test_what_is_granted_actually_fits_with_the_reserve_still_intact()
{
    // The property the whole exercise exists for, checked across the range
    // rather than at one point: whatever comes back must still leave the
    // reserve untouched once every segment has filled.
    const uint32_t available = kFree + kOnDisk;
    const uint32_t reserve = reserveBytes(kPartition);
    for (uint32_t asked = 0; asked <= 10000; asked += 137) {
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
    // Up to the whole S3 filesystem, not just the WROOM-32's partition: with
    // the ceiling at 10000 the budgets the clamp has to invert on that family
    // are four times larger than anything this loop used to reach.
    for (uint32_t budget = 0; budget <= 2432u * 1024u; budget += 4096) {
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
    for (uint32_t asked = 1; asked <= 10000; asked += 91) {
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

// The three figures the header of partitions/esp_garden_8mb.csv argues from.
// That file is a DURABLE record — a partition table cannot be delivered over
// OTA, so the layout a board is first flashed with is the one it keeps — and
// its previous version reasoned from a cap that had already been raised and a
// cost that ignored LittleFS's blocks. Pinned here so the paragraph and the
// arithmetic cannot drift apart again.
static void
test_the_8mb_filesystem_holds_the_record_ceiling_with_room_to_spare()
{
    const uint32_t kFs = 0x260000u;   // 2 490 368 B = 2432 KB, the spiffs row

    // The shipped ConfigFile ceiling, in whole blocks. 480 KB since 2.20.0
    // raised it from 5000 to 10000; 256 KB is what the previous one cost, and
    // neither is the 120 KB the header once quoted for a 2500 that no longer
    // applies.
    TEST_ASSERT_EQUAL_UINT32(491520u, storageBytes(10000, kHeader, kRecord));

    // The first partition on which max(64 KB, partition / 8) picks the
    // proportional term. On every 4 MB board both terms are 64 KB, so the
    // "larger of" has never had to choose.
    TEST_ASSERT_EQUAL_UINT32(311296u, reserveBytes(kFs));
    TEST_ASSERT_TRUE(reserveBytes(kFs) > 64u * 1024u);

    // 212 KB of block-rounded web assets is measured, not asserted here; what
    // the header needs is that the other two plus that figure still leave the
    // filesystem comfortably larger than the worst case it must hold. Raising
    // the ceiling moved that margin from 3.1x to 2.4x, which is the honest
    // number and the reason this assertion says 2 and not 3.
    const uint32_t assets = 212u * 1024u;
    const uint32_t worst = assets + storageBytes(10000, kHeader, kRecord) +
                           reserveBytes(kFs);
    TEST_ASSERT_EQUAL_UINT32(1019904u, worst);   // 996 KB
    TEST_ASSERT_TRUE(worst * 2u < kFs);

    // And the whole ceiling is granted on this partition rather than clamped,
    // which is what "the cap can be raised later without a serial reflash"
    // means. Free space here is the partition minus those assets.
    TEST_ASSERT_EQUAL_UINT32(
      10000, fitCapacity(10000, kFs, kFs - assets, 0, kHeader, kRecord));
}

static void
test_what_each_family_actually_resolves_to_at_the_raised_ceiling()
{
    // The two answers the 2.20.0 change has to be able to state. Both are
    // arithmetic on numbers read off the two boards, not measurements taken
    // after the change: nothing here has run on hardware.
    //
    // WROOM-32, device 6224: 512 KB partition, the free space and segment
    // bytes at the top of this section. The ceiling is irrelevant there — 5000
    // and 10000 both come back as 3408, because what refuses is the partition.
    TEST_ASSERT_EQUAL_UINT32(
      3408, fitCapacity(10000, kPartition, kFree, kOnDisk, kHeader, kRecord));

    // ESP32-S3, device b580: 2432 KB filesystem reporting 248 KB used, with
    // 1440 records already stored at 180 per segment. 10000 is granted whole,
    // at 480 KB of a partition that would in fact hold 40 952 — so on this
    // family the CEILING is the binding limit and the device is not.
    const uint32_t s3Fs = 0x260000u;
    const uint32_t s3Free = (2432u - 248u) * 1024u;
    const uint32_t s3OnDisk = 8u * (kHeader + 180u * kRecord);
    TEST_ASSERT_EQUAL_UINT32(
      10000, fitCapacity(10000, s3Fs, s3Free, s3OnDisk, kHeader, kRecord));
    TEST_ASSERT_EQUAL_UINT32(
      40952, capacityForBytes(s3Free + s3OnDisk - reserveBytes(s3Fs), kHeader,
                              kRecord));
    TEST_ASSERT_TRUE(storageBytes(10000, kHeader, kRecord) +
                       reserveBytes(s3Fs) <=
                     s3Free + s3OnDisk);
}

static void
test_the_compiled_default_capacity_is_not_exempt_from_the_fit()
{
    // 1440 is what ConfigFile's constructor holds, and it is what survives
    // every early return in loadFile() — a missing /config.json, an unopenable
    // one, an unparseable one, a foreign id. That is exactly the state a
    // filesystem deploy which dropped /config.json leaves behind: the
    // partition is at its fullest and the configured number nobody could read
    // is replaced by a compiled one nothing had checked. main.cpp calls
    // ioHistory.begin() with it regardless, so the fit has to run on that path
    // too — which is why the call moved out of loadFile() and into
    // loadConfigFile(), above the early returns rather than below them.
    //
    // Nothing about 1440 makes it fit. Here it is a 96 KB request against a
    // freshly reflashed partition with 130 KB free and no history yet on disk
    // to credit back, and the arithmetic reduces it whether a config was read
    // or not.
    static const uint32_t kDefaultRecords = 1440;
    static const uint32_t kFreeAfterDeploy = 130u * 1024u;

    const uint32_t granted = fitCapacity(kDefaultRecords, kPartition,
                                         kFreeAfterDeploy, 0, kHeader, kRecord);
    TEST_ASSERT_EQUAL_UINT32(1360, granted);
    TEST_ASSERT_TRUE(granted < kDefaultRecords);
    TEST_ASSERT_TRUE(storageBytes(granted, kHeader, kRecord) +
                       reserveBytes(kPartition) <=
                     kFreeAfterDeploy);
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
    RUN_TEST(
      test_a_segment_is_full_at_the_smaller_of_its_own_header_and_the_new_geometry);
    RUN_TEST(test_growing_the_capacity_keeps_every_record_and_converges_by_rotation);
    RUN_TEST(test_shrinking_the_capacity_evicts_rather_than_deletes);
    RUN_TEST(test_a_resize_that_is_undone_before_it_converges_still_orders_correctly);
    RUN_TEST(test_the_absolute_walk_survives_a_rotation_between_mixed_size_segments);
    RUN_TEST(test_locate_crosses_segments_that_hold_different_numbers_of_records);
    RUN_TEST(test_the_reported_capacity_is_the_product_again_once_the_slots_agree);
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
    RUN_TEST(test_the_compiled_default_capacity_is_not_exempt_from_the_fit);
    RUN_TEST(test_the_8mb_filesystem_holds_the_record_ceiling_with_room_to_spare);
    RUN_TEST(test_what_each_family_actually_resolves_to_at_the_raised_ceiling);
}
