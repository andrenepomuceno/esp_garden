#pragma once

#include <stddef.h>
#include <stdint.h>

// The session table's POLICY — which slot a new login takes, and which slots a
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
// That eviction is gone. Several remembered devices per user is the feature;
// the only bound is the size of the table.
//
// WHAT THAT COSTS, AND WHY IT IS PAID HERE
//
// More live tokens at once is a wider surface, and it removes a side effect
// that used to hide a hole: while only one persistent session per user could
// exist, changing a password and logging in again revoked the old token as a
// consequence of the login, not as a consequence of the password change.
// Nothing invalidated a session when a password changed. So invalidateUser()
// exists alongside the relaxation and is called from BOTH password-writing
// paths — POST /users and the ota.password push in POST /config.json. A
// person changing a compromised password expects every token issued under the
// old one to die, and that expectation is now met explicitly rather than as a
// by-product of a restriction.
//
// A slot identifies its owner by INDEX into the user store, never by name.
// UserStore::remove() erases from a vector and shifts every later entry, which
// is why a delete still drops EVERY session wholesale; upsert() replaces in
// place or appends, so it never reorders and a per-user invalidation is exact.

namespace session_slots {

// Index of the slot a new session should take: the first inactive one, and
// otherwise the least-recently-seen. A full table therefore evicts rather than
// refusing a login, which is deliberate — the alternative is a device that
// locks its operator out because four stale tabs are holding the slots.
//
// Returns -1 only for an empty table, which cannot happen with a fixed array.
template<typename Slot>
int
allocate(const Slot* slots, size_t count)
{
    int lru = -1;
    for (size_t i = 0; i < count; ++i) {
        if (!slots[i].active) {
            return (int)i;
        }
        if (lru < 0 || slots[i].lastSeenMs < slots[(size_t)lru].lastSeenMs) {
            lru = (int)i;
        }
    }
    return lru;
}

struct InvalidateResult
{
    size_t dropped;         // how many live sessions were ended
    bool persistentDropped; // at least one was remembered, so the file is stale
    bool droppedSlot;       // `slot` was among them: the caller signed itself out
};

// Deactivates every active slot owned by `userIndex`. `slot` names one slot to
// report on — the session making the request — so the caller can tell a client
// that it has just signed ITSELF out; pass `count` for none.
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
invalidateUser(Slot* slots, size_t count, size_t userIndex, size_t slot,
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
        if (i == slot) {
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
