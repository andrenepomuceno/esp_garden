#include "core/session_slots.h"
#include <unity.h>

// The session table's policy: which slot a login takes, and which slots a
// password change has to end. It is unit-tested for the reason segment_index.h
// is — a wrong answer here does not fail loudly. Allocating the wrong slot
// signs somebody else out; leaving a slot behind leaves a token alive that the
// operator believes they revoked. Both look like a working device.

using namespace session_slots;

namespace {

// The minimum a slot has to expose. The firmware's CustomLogin::Session also
// carries a token, a creation stamp and an IPAddress, none of which this policy
// may know about — that is exactly why it is a template over the slot type.
struct TestSlot
{
    size_t userIndex;
    uint32_t lastSeenMs;
    bool active;
    bool persistent;
};

const size_t kCount = 8;

void
clear(TestSlot* slots)
{
    for (size_t i = 0; i < kCount; ++i) {
        slots[i] = TestSlot{ 0, 0, false, false };
    }
}

void
occupy(TestSlot& slot, size_t user, uint32_t seen, bool persistent)
{
    slot.userIndex = user;
    slot.lastSeenMs = seen;
    slot.active = true;
    slot.persistent = persistent;
}

// Counts calls, so a test can assert the wipe callback ran exactly per drop.
// Reset from setUp(), not from individual tests: resetting in only some of them
// is what makes a suite pass in one order and fail in another.
size_t g_drops = 0;
void
countDrop(TestSlot&)
{
    ++g_drops;
}

} // namespace

void
reset_session_slots_state(void)
{
    g_drops = 0;
}

static void
test_allocate_prefers_a_free_slot_over_the_least_recently_seen()
{
    TestSlot slots[kCount];
    clear(slots);
    occupy(slots[0], 0, 100, false);
    occupy(slots[1], 0, 1, false); // the LRU, but slot 2 is free

    TEST_ASSERT_EQUAL_INT(2, allocate(slots, kCount, 200));
}

static void
test_a_full_table_evicts_the_least_recently_seen()
{
    TestSlot slots[kCount];
    clear(slots);
    for (size_t i = 0; i < kCount; ++i) {
        occupy(slots[i], i, (uint32_t)(1000 + i * 10), false);
    }
    slots[5].lastSeenMs = 7; // oldest

    TEST_ASSERT_EQUAL_INT(5, allocate(slots, kCount, 2000));
}

// millis() wraps every 49.7 days. Ranking slots by comparing lastSeenMs as
// absolute values evicts the slot with the SMALLEST number, which after a wrap
// is the most recently seen session — so a device up for seven weeks starts
// signing out whoever is actively using it and keeps the stale entries. The
// earlier version of this file had exactly that bug, and the first tests could
// not see it because they only ever fed increasing timestamps.
static void
test_allocate_survives_the_millis_wraparound()
{
    TestSlot slots[kCount];
    clear(slots);

    const uint32_t now = 5000; // just after the wrap
    for (size_t i = 0; i < kCount; ++i) {
        // Seen a few seconds ago, i.e. just BEFORE the wrap: a huge number.
        occupy(slots[i], i, (uint32_t)(0xFFFFFF00u + i), false);
    }
    // The genuinely oldest: seen ~60 s before the wrap.
    slots[3].lastSeenMs = 0xFFFFFF00u - 60000u;
    // A slot stamped after the wrap is the NEWEST, and must not be chosen.
    slots[6].lastSeenMs = 4990;

    TEST_ASSERT_EQUAL_INT(3, allocate(slots, kCount, now));
}

static void
test_age_is_computed_across_the_wraparound()
{
    // 100 ms before the wrap to 50 ms after it is 150 ms, not 4.29 billion.
    TEST_ASSERT_EQUAL_UINT32(150, age(50u, 0xFFFFFFFFu - 99u));
}

// A login must never cost somebody their remembered device while an ordinary
// slot is available to take instead.
static void
test_allocate_evicts_an_ephemeral_slot_before_a_persistent_one()
{
    TestSlot slots[kCount];
    clear(slots);
    for (size_t i = 0; i < kCount; ++i) {
        occupy(slots[i], i, (uint32_t)(1000 + i), true);
    }
    // The oldest slot in the table is remembered; slot 7 is ordinary and newer.
    slots[0].lastSeenMs = 1;
    occupy(slots[7], 7, 9000, false);

    TEST_ASSERT_EQUAL_INT(7, allocate(slots, kCount, 10000));
}

// ...but a login is never REFUSED. With every slot remembered, the oldest one
// is given up rather than answering 503.
static void
test_a_table_of_only_persistent_slots_still_yields_one()
{
    TestSlot slots[kCount];
    clear(slots);
    for (size_t i = 0; i < kCount; ++i) {
        occupy(slots[i], i, (uint32_t)(1000 + i * 10), true);
    }
    slots[2].lastSeenMs = 5;

    TEST_ASSERT_EQUAL_INT(2, allocate(slots, kCount, 4000));
}

// The restore path must not evict: overwriting a slot it just filled would
// report a count the table does not hold.
static void
test_allocate_free_refuses_rather_than_evicting()
{
    TestSlot slots[kCount];
    clear(slots);
    TEST_ASSERT_EQUAL_INT(0, allocateFree(slots, kCount));

    for (size_t i = 0; i < kCount; ++i) {
        occupy(slots[i], i, (uint32_t)i, true);
    }
    TEST_ASSERT_EQUAL_INT(kNoSlot, allocateFree(slots, kCount));
}

static void
test_the_persistent_census_counts_only_live_remembered_slots()
{
    TestSlot slots[kCount];
    clear(slots);
    occupy(slots[0], 0, 10, true);
    occupy(slots[1], 1, 20, false);
    occupy(slots[2], 2, 30, true);
    slots[3].persistent = true; // inactive: a stale flag, not a session

    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)countPersistent(slots, kCount));
}

static void
test_the_cap_gives_up_the_oldest_remembered_slot()
{
    TestSlot slots[kCount];
    clear(slots);
    occupy(slots[0], 0, 900, true);
    occupy(slots[1], 1, 100, false); // older, but not remembered
    occupy(slots[2], 2, 300, true);  // the oldest remembered one

    TEST_ASSERT_EQUAL_INT(2, oldestPersistent(slots, kCount, 5000));

    clear(slots);
    TEST_ASSERT_EQUAL_INT(kNoSlot, oldestPersistent(slots, kCount, 5000));
}

static void
test_oldest_persistent_survives_the_millis_wraparound()
{
    TestSlot slots[kCount];
    clear(slots);
    occupy(slots[0], 0, 0xFFFFFF00u, true);          // just before the wrap
    occupy(slots[1], 1, 0xFFFFFF00u - 60000u, true); // a minute earlier
    occupy(slots[2], 2, 300, true);                  // just after it: newest

    TEST_ASSERT_EQUAL_INT(1, oldestPersistent(slots, kCount, 5000));
}

// The feature. Allocation has never cared who owns a slot, so two devices for
// one user simply take two slots — and, with the handleLogin() eviction gone,
// both stay live even when both are persistent.
static void
test_one_user_can_hold_several_sessions_at_once()
{
    TestSlot slots[kCount];
    clear(slots);

    const int first = allocate(slots, kCount, 100);
    occupy(slots[(size_t)first], 3, 10, true);

    const int second = allocate(slots, kCount, 200);
    TEST_ASSERT_NOT_EQUAL(first, second);
    occupy(slots[(size_t)second], 3, 20, true);

    const int third = allocate(slots, kCount, 300);
    TEST_ASSERT_NOT_EQUAL(first, third);
    TEST_ASSERT_NOT_EQUAL(second, third);
    occupy(slots[(size_t)third], 3, 30, false);

    TEST_ASSERT_TRUE(slots[(size_t)first].active);
    TEST_ASSERT_TRUE(slots[(size_t)second].active);
    TEST_ASSERT_TRUE(slots[(size_t)third].active);
}

static void
test_invalidate_ends_every_session_of_one_user_and_no_other()
{
    TestSlot slots[kCount];
    clear(slots);
    occupy(slots[0], 2, 10, false);
    occupy(slots[1], 3, 20, false); // a neighbouring index must survive
    occupy(slots[2], 2, 30, true);
    occupy(slots[3], 1, 40, true);

    const InvalidateResult result =
      invalidateUser(slots, kCount, 2, kNoSlot, countDrop);

    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)result.dropped);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)g_drops);
    TEST_ASSERT_FALSE(slots[0].active);
    TEST_ASSERT_FALSE(slots[2].active);
    TEST_ASSERT_TRUE(slots[1].active);
    TEST_ASSERT_TRUE(slots[3].active);
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)slots[1].userIndex);
}

// The file on flash only has to be rewritten when a REMEMBERED session died.
static void
test_invalidate_reports_whether_a_persistent_session_died()
{
    TestSlot slots[kCount];
    clear(slots);
    occupy(slots[0], 5, 10, false);

    InvalidateResult result = invalidateUser(slots, kCount, 5, kNoSlot, countDrop);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)result.dropped);
    TEST_ASSERT_FALSE(result.persistentDropped);

    clear(slots);
    occupy(slots[0], 5, 10, false);
    occupy(slots[1], 5, 20, true);
    result = invalidateUser(slots, kCount, 5, kNoSlot, countDrop);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)result.dropped);
    TEST_ASSERT_TRUE(result.persistentDropped);
}

// A password change signs the caller out too, and the handler has to be able to
// say so: the page answers {"reauth":true} and redirects to the login screen,
// exactly as the delete path does.
static void
test_invalidate_reports_when_the_caller_signed_itself_out()
{
    TestSlot slots[kCount];
    clear(slots);
    occupy(slots[0], 4, 10, false); // the request's own session
    occupy(slots[1], 4, 20, true);

    InvalidateResult result = invalidateUser(slots, kCount, 4, 0, countDrop);
    TEST_ASSERT_TRUE(result.droppedSlot);

    // An ADMIN changing SOMEBODY ELSE'S password keeps working.
    clear(slots);
    occupy(slots[0], 4, 10, false); // the admin, unaffected
    occupy(slots[1], 6, 20, true);  // the account being changed

    result = invalidateUser(slots, kCount, 6, 0, countDrop);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)result.dropped);
    TEST_ASSERT_FALSE(result.droppedSlot);
    TEST_ASSERT_TRUE(slots[0].active);
}

// An inactive slot still carries the userIndex of whoever last held it. Reading
// that as ownership would end sessions of an unrelated account on every reuse.
static void
test_invalidate_ignores_slots_that_are_already_inactive()
{
    TestSlot slots[kCount];
    clear(slots);
    slots[0].userIndex = 7; // stale, already signed out
    occupy(slots[1], 7, 20, false);

    const InvalidateResult result =
      invalidateUser(slots, kCount, 7, kNoSlot, countDrop);

    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)result.dropped);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)g_drops);
}

static void
test_invalidate_on_a_user_with_no_sessions_changes_nothing()
{
    TestSlot slots[kCount];
    clear(slots);
    occupy(slots[0], 1, 10, true);

    const InvalidateResult result =
      invalidateUser(slots, kCount, 9, kNoSlot, countDrop);

    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)result.dropped);
    TEST_ASSERT_FALSE(result.persistentDropped);
    TEST_ASSERT_FALSE(result.droppedSlot);
    TEST_ASSERT_TRUE(slots[0].active);
}

// A user DELETE shifts every later index, so nothing may be spared.
static void
test_invalidate_all_ends_every_account_and_wipes_the_stale_slots()
{
    TestSlot slots[kCount];
    clear(slots);
    occupy(slots[0], 0, 10, true);
    occupy(slots[1], 1, 20, false);
    occupy(slots[4], 2, 30, true);
    slots[5].persistent = true; // stale flag on an inactive slot

    const InvalidateResult result = invalidateAll(slots, kCount, 1, countDrop);

    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)result.dropped);
    TEST_ASSERT_TRUE(result.persistentDropped);
    TEST_ASSERT_TRUE(result.droppedSlot);
    // The callback runs for every slot, so the token bytes are wiped whether or
    // not the slot was live.
    TEST_ASSERT_EQUAL_UINT32((uint32_t)kCount, (uint32_t)g_drops);
    for (size_t i = 0; i < kCount; ++i) {
        TEST_ASSERT_FALSE(slots[i].active);
        TEST_ASSERT_FALSE(slots[i].persistent);
    }
}

// ---- the purge verdict -----------------------------------------------------
//
// A remembered slot is exempt from the idle TTL, so this verdict is the ONLY
// thing that ever ends one. Getting it wrong either forgets every remembered
// device on a board that boots before NTP answers, or leaves a token immortal.

static const uint32_t kTtlSec = 30UL * 24UL * 60UL * 60UL;

static void
test_a_remembered_slot_is_kept_while_the_clock_is_unusable()
{
    // Dropping on an unsynced clock would forget every remembered device at
    // every boot, since webSetup() runs before the NTP wait.
    TEST_ASSERT_TRUE(PurgeVerdict::Keep ==
                     persistentVerdict(false, 0, 0, kTtlSec));
    TEST_ASSERT_TRUE(PurgeVerdict::Keep ==
                     persistentVerdict(false, 1000, 1000 + kTtlSec * 2,
                                       kTtlSec));
}

static void
test_an_undated_remembered_slot_is_stamped_rather_than_dropped_or_kept()
{
    // Written by 2.11.0, or issued while the clock was down. Keeping it would
    // make it immortal; dropping it would sign out a device for a firmware
    // detail it had no part in.
    TEST_ASSERT_TRUE(PurgeVerdict::Stamp ==
                     persistentVerdict(true, 0, 1'700'000'000UL, kTtlSec));
}

static void
test_a_remembered_slot_dies_only_past_the_absolute_ttl()
{
    const uint32_t created = 1'700'000'000UL;
    TEST_ASSERT_TRUE(PurgeVerdict::Keep ==
                     persistentVerdict(true, created, created, kTtlSec));
    // Exactly at the TTL is still alive; the comparison is strictly greater.
    TEST_ASSERT_TRUE(PurgeVerdict::Keep ==
                     persistentVerdict(true, created, created + kTtlSec,
                                       kTtlSec));
    TEST_ASSERT_TRUE(PurgeVerdict::Drop ==
                     persistentVerdict(true, created, created + kTtlSec + 1,
                                       kTtlSec));
}

static void
test_an_ordinary_slot_idles_out_across_the_millis_wraparound()
{
    const uint32_t ttl = 24UL * 60UL * 60UL * 1000UL;
    // Seen just now.
    TEST_ASSERT_FALSE(idledOut(1000, 1000, ttl));
    TEST_ASSERT_FALSE(idledOut(1000 + ttl, 1000, ttl));
    TEST_ASSERT_TRUE(idledOut(1000 + ttl + 1, 1000, ttl));
    // Straddling the wrap: seen 1 s before it, asked 1 s after. Comparing the
    // timestamps directly would call this session ancient and sign it out.
    const uint32_t beforeWrap = 0xFFFFFFFFUL - 1000;
    TEST_ASSERT_FALSE(idledOut(2000, beforeWrap, ttl));
}

// ---- the deferred /sessions.json write -------------------------------------
//
// The purge runs at the top of the authorization middleware, on the single
// async_tcp task that also carries a 1.2 MB OTA upload, so the write moved to a
// background tick. What this bookkeeping has to guarantee is that a queued
// write is never LOST and never REPEATED, and that a synchronous save — the
// revocation paths, which may not be deferred — supersedes a pending one.

static void
test_a_fresh_queue_asks_for_no_write()
{
    SaveQueue queue;
    TEST_ASSERT_FALSE(queue.takePending());
}

static void
test_a_marked_queue_yields_exactly_one_write()
{
    SaveQueue queue;
    queue.markStale();
    queue.markStale(); // several expiries in one sweep are still one file
    TEST_ASSERT_TRUE(queue.takePending());
    // A flush that failed to clear would write once per background tick for
    // ever — worse than the per-request write this replaces.
    TEST_ASSERT_FALSE(queue.takePending());
}

static void
test_a_synchronous_save_supersedes_a_queued_one()
{
    SaveQueue queue;
    queue.markStale();
    // A login or a revocation rendered the table itself; that snapshot is
    // strictly newer, so the deferred write must not land after it and put a
    // file on flash that predates the token just issued.
    queue.markWritten();
    TEST_ASSERT_FALSE(queue.takePending());
}

static void
test_a_purge_after_a_synchronous_save_is_queued_again()
{
    SaveQueue queue;
    queue.markStale();
    queue.markWritten();
    queue.markStale();
    TEST_ASSERT_TRUE(queue.takePending());
}

void
run_session_slots_tests(void)
{
    RUN_TEST(test_allocate_prefers_a_free_slot_over_the_least_recently_seen);
    RUN_TEST(test_a_full_table_evicts_the_least_recently_seen);
    RUN_TEST(test_allocate_survives_the_millis_wraparound);
    RUN_TEST(test_age_is_computed_across_the_wraparound);
    RUN_TEST(test_allocate_evicts_an_ephemeral_slot_before_a_persistent_one);
    RUN_TEST(test_a_table_of_only_persistent_slots_still_yields_one);
    RUN_TEST(test_allocate_free_refuses_rather_than_evicting);
    RUN_TEST(test_the_persistent_census_counts_only_live_remembered_slots);
    RUN_TEST(test_the_cap_gives_up_the_oldest_remembered_slot);
    RUN_TEST(test_oldest_persistent_survives_the_millis_wraparound);
    RUN_TEST(test_one_user_can_hold_several_sessions_at_once);
    RUN_TEST(test_invalidate_ends_every_session_of_one_user_and_no_other);
    RUN_TEST(test_invalidate_reports_whether_a_persistent_session_died);
    RUN_TEST(test_invalidate_reports_when_the_caller_signed_itself_out);
    RUN_TEST(test_invalidate_ignores_slots_that_are_already_inactive);
    RUN_TEST(test_invalidate_on_a_user_with_no_sessions_changes_nothing);
    RUN_TEST(test_invalidate_all_ends_every_account_and_wipes_the_stale_slots);
    RUN_TEST(test_a_remembered_slot_is_kept_while_the_clock_is_unusable);
    RUN_TEST(test_an_undated_remembered_slot_is_stamped_rather_than_dropped_or_kept);
    RUN_TEST(test_a_remembered_slot_dies_only_past_the_absolute_ttl);
    RUN_TEST(test_an_ordinary_slot_idles_out_across_the_millis_wraparound);
    RUN_TEST(test_a_fresh_queue_asks_for_no_write);
    RUN_TEST(test_a_marked_queue_yields_exactly_one_write);
    RUN_TEST(test_a_synchronous_save_supersedes_a_queued_one);
    RUN_TEST(test_a_purge_after_a_synchronous_save_is_queued_again);
}
