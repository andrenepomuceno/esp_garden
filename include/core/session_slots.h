#pragma once

#include <stddef.h>
#include <stdint.h>

// The session table's POLICY — which slot a new login takes, which slots a
// credential change has to drop — free of Arduino, of mbedtls and of the web
// server so it can be tested on the host.
//
// It is separated for the same reason segment_index.h is: a wrong answer here
// does not fail loudly. Allocating the wrong slot silently signs somebody else
// out; dropping the wrong slots silently leaves a token alive that the operator
// believes they have just revoked. Both look exactly like a working device.
//
// WHY SEVERAL SESSIONS PER USER
//
// Sessions were already multi-device by accident — allocate() has always
// returned any free slot without caring who owned it — but handleLogin() then
// deactivated every other PERSISTENT session belonging to the same user, so a
// person who ticked "remember me" on a phone lost the tick on their laptop.
// That eviction is gone. Several remembered devices per user is the feature.
//
// WHAT REPLACED THE BOUND THAT EVICTION WAS ACCIDENTALLY PROVIDING
//
// The per-user eviction was the only thing keeping remembered sessions from
// filling the table, because a persistent slot is exempt from the idle TTL.
// Removing it without a replacement means eight remembered logins hold every
// slot forever, and from then on every ordinary login evicts somebody's
// remembered device — which is the failure raising kMaxSessions was meant to
// relieve, merely relocated. Two deliberate bounds replace it:
//
//   1. A CAP on how many slots may be persistent at once, enforced by the
//      caller (CustomLogin::kMaxPersistentSessions), so free or ephemeral
//      slots always exist.
//   2. TIERED eviction here: a free slot, else the oldest NON-persistent slot,
//      and only as a last resort the oldest persistent one. With the cap in
//      force the last tier is unreachable, so a script authenticating in a
//      loop churns the ephemeral slots and never touches a remembered device.
//      It stays as a tier anyway, because refusing a login outright is worse
//      than forgetting one remembered browser.
//
// A slot identifies its owner by INDEX into the user store, never by name.
// UserStore::remove() erases from a vector and shifts every later entry, which
// is why a delete still drops EVERY session wholesale; upsert() replaces in
// place or appends, so it never reorders and a per-user invalidation is exact.

namespace session_slots {

// One sentinel for "no slot", used by every function here and by the caller.
// Two spellings of the same idea in one header is how an off-by-one becomes a
// security bug: `count` also reads as a valid loop bound.
static const int kNoSlot = -1;

// Age of a slot, in the same units as lastSeenMs. Written as a subtraction of
// unsigned values so it stays correct across the millis() wrap at 49.7 days —
// comparing the timestamps directly would rank a slot stamped just after the
// wrap as the OLDEST in the table and evict the session in active use. Every
// other time comparison in this firmware is written this way; this one was not,
// and the first host tests could not see it because they only fed increasing
// values.
inline uint32_t
age(uint32_t now, uint32_t lastSeenMs)
{
    return now - lastSeenMs;
}

// What a purge should do with ONE slot. Split out for the reason the rest of
// this header is: the verdict decides whether a token that the operator
// believes has expired is still accepted, and getting it wrong looks exactly
// like a working device.
enum class PurgeVerdict
{
    Keep,  // still valid, or not yet decidable
    Stamp, // remembered but of unknown age: date it now, and persist the date
    Drop   // past its lifetime
};

// A REMEMBERED slot. It deliberately does not idle out — that is what the tick
// means — so it expires on ABSOLUTE age, in wall-clock seconds, which is the
// only clock that survives the reboots that reset millis().
//
// `Keep` while the clock is not yet usable: dropping on an unsynced clock would
// forget every remembered device on a board that boots before NTP answers, and
// asking again on the next request costs nothing.
inline PurgeVerdict
persistentVerdict(bool clockUsable,
                  uint32_t createdAtEpoch,
                  uint32_t wallClock,
                  uint32_t ttlSec)
{
    if (!clockUsable) {
        return PurgeVerdict::Keep;
    }
    // Age unknown: the clock was down when it was issued, or a firmware that
    // stored no timestamp wrote it. Stamping makes it age from here instead of
    // being immortal, and the stamp has to reach flash or the next boot asks
    // the same question again.
    if (createdAtEpoch == 0) {
        return PurgeVerdict::Stamp;
    }
    return ((wallClock - createdAtEpoch) > ttlSec) ? PurgeVerdict::Drop
                                                   : PurgeVerdict::Keep;
}

// An ORDINARY slot expires on disuse. Unsigned subtraction, for the wrap the
// note on age() describes.
inline bool
idledOut(uint32_t now, uint32_t lastSeenMs, uint32_t ttlMs)
{
    return age(now, lastSeenMs) > ttlMs;
}

// Bookkeeping for a /sessions.json write that has been moved OFF the request
// path. purgeExpiredSessions() runs at the top of the authorization middleware,
// on the single async_tcp task that also serves a 1.2 MB OTA upload, so it may
// not touch flash; it marks instead, and a background task writes.
//
// `markWritten()` is the half that matters. Any SYNCHRONOUS save renders a
// strictly newer view of the table, so it supersedes whatever was queued —
// without that, a deferred write could land after a login and put a file on
// flash that predates it.
//
// `takePending()` clears as it reads: a flush that forgot to clear would write
// once per background tick for ever, which is worse than the per-request write
// this replaces.
struct SaveQueue
{
    // Written on the request path, read on a background task.
    volatile bool stale = false;

    void markStale() { stale = true; }
    void markWritten() { stale = false; }
    bool takePending()
    {
        const bool pending = stale;
        stale = false;
        return pending;
    }
};

// The first inactive slot, or kNoSlot. Never evicts — this is what a restore
// from /sessions.json wants, where evicting means overwriting an entry that was
// just restored and reporting a count the table does not hold.
template<typename Slot>
int
allocateFree(const Slot* slots, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (!slots[i].active) {
            return (int)i;
        }
    }
    return kNoSlot;
}

// The slot a new login should take: a free one, else the oldest non-persistent
// one, else the oldest persistent one. See the tiering note above.
//
// Returns kNoSlot only for an empty table, which a fixed array cannot be.
template<typename Slot>
int
allocate(const Slot* slots, size_t count, uint32_t now)
{
    const int free = allocateFree(slots, count);
    if (free != kNoSlot) {
        return free;
    }

    // Every slot is active here — allocateFree() already returned above if any
    // were not — so the two tiers below cover the whole table.
    int ephemeral = kNoSlot;
    int persistent = kNoSlot;
    for (size_t i = 0; i < count; ++i) {
        int& tier = slots[i].persistent ? persistent : ephemeral;
        if (tier == kNoSlot || age(now, slots[i].lastSeenMs) >
                                 age(now, slots[(size_t)tier].lastSeenMs)) {
            tier = (int)i;
        }
    }
    return (ephemeral != kNoSlot) ? ephemeral : persistent;
}

// How many slots are remembered right now — what the cap is checked against.
template<typename Slot>
size_t
countPersistent(const Slot* slots, size_t count)
{
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (slots[i].active && slots[i].persistent) {
            ++total;
        }
    }
    return total;
}

// The remembered slot to give up when the cap is already met: the oldest one.
// The seventh remembered device forgets the first, which is explicable to an
// operator in a way that "your login was refused" is not.
template<typename Slot>
int
oldestPersistent(const Slot* slots, size_t count, uint32_t now)
{
    int oldest = kNoSlot;
    for (size_t i = 0; i < count; ++i) {
        if (!slots[i].active || !slots[i].persistent) {
            continue;
        }
        if (oldest == kNoSlot || age(now, slots[i].lastSeenMs) >
                                   age(now, slots[(size_t)oldest].lastSeenMs)) {
            oldest = (int)i;
        }
    }
    return oldest;
}

struct InvalidateResult
{
    size_t dropped;         // how many live sessions were ended
    bool persistentDropped; // at least one was remembered, so the file is stale
    bool droppedSlot;       // `slot` was among them: the caller signed itself out
};

// Deactivates every active slot owned by `userIndex`. `callerSlot` names the
// slot serving the request, so the caller can tell a client that it has just
// signed ITSELF out; pass kNoSlot for none.
//
// The deactivation is unconditional, the request's own session included. The
// exemption an "except the caller" variant would grant goes to whoever makes
// the request, and an attacker holding a stolen ADMIN token makes that request
// just as well as its owner — so exempting the caller hands the surviving token
// to the wrong party in exactly the scenario the invalidation exists for. One
// re-login with the password that was just typed is the whole cost.
//
// onDrop is called for each ended slot, which is where the token bytes are
// wiped: this header deliberately knows nothing about tokens.
template<typename Slot, typename OnDrop>
InvalidateResult
invalidateUser(Slot* slots, size_t count, size_t userIndex, int callerSlot,
               OnDrop onDrop)
{
    InvalidateResult result = { 0, false, false };
    for (size_t i = 0; i < count; ++i) {
        if (!slots[i].active || slots[i].userIndex != userIndex) {
            continue;
        }
        if (slots[i].persistent) {
            result.persistentDropped = true;
        }
        if ((int)i == callerSlot) {
            result.droppedSlot = true;
        }
        slots[i].active = false;
        slots[i].persistent = false;
        onDrop(slots[i]);
        ++result.dropped;
    }
    return result;
}

// Every slot, every account — what a user DELETE owes, because removal shifts
// every later index and a live session holds an index, not a name.
template<typename Slot, typename OnDrop>
InvalidateResult
invalidateAll(Slot* slots, size_t count, int callerSlot, OnDrop onDrop)
{
    InvalidateResult result = { 0, false, false };
    for (size_t i = 0; i < count; ++i) {
        if (!slots[i].active) {
            // An inactive slot still carries stale fields; clearing it costs
            // nothing and keeps "not active" and "holds no token" the same
            // statement.
            slots[i].persistent = false;
            onDrop(slots[i]);
            continue;
        }
        if (slots[i].persistent) {
            result.persistentDropped = true;
        }
        if ((int)i == callerSlot) {
            result.droppedSlot = true;
        }
        slots[i].active = false;
        slots[i].persistent = false;
        onDrop(slots[i]);
        ++result.dropped;
    }
    return result;
}

} // namespace session_slots
