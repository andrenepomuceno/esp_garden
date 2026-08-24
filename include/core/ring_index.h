#pragma once

#include <stdint.h>

// Ring-buffer index arithmetic, kept free of Arduino, SPIFFS and logging so the
// host tests can reach it. This is the only part of IoHistory with no I/O in
// it, and it is where an off-by-one hides: every read of the file goes through
// it, so a wrong answer here silently reorders history rather than failing.
namespace ring {

/// Slot holding the `index`-th oldest record.
///
/// Before the buffer wraps, record 0 lives in slot 0 and `head == stored`.
/// Once it has wrapped, the oldest record is the one `head` is about to
/// overwrite, so the sequence starts there and runs modulo `capacity`.
inline uint32_t
slotOf(uint32_t index, uint32_t head, uint32_t stored, uint16_t capacity)
{
    if (capacity == 0) {
        return 0;
    }

    const uint32_t oldest = (stored < capacity) ? 0u : head;
    return (oldest + index) % capacity;
}

} // namespace ring
