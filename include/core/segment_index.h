#pragma once

#include <stdint.h>

// Where a logical history position lives, once the history is a set of
// append-only segments rather than one file rewritten in place.
//
// Free of Arduino and of the filesystem on purpose, exactly as ring_index.h
// was: a wrong answer here does not fail, it silently reorders history or
// serves one record in place of another, and that is the one part of the
// subsystem that must have unit tests.
//
// WHY SEGMENTS AT ALL
//
// The previous design was a fixed-size ring in ONE file: a slot rewritten in
// place, plus a header rewritten at offset 0, every 60 s. That is correct on
// SPIFFS, which rewrites a single 256-byte page for an in-place overwrite.
//
// It is fatal on LittleFS. LittleFS stores file data as a copy-on-write CTZ
// skip-list, which is built for appending; rewriting bytes in the MIDDLE of a
// file copies the chain forward from that point. With the write near the start
// of a 69 KB file that is the whole file, once a minute — roughly 100 MB of
// flash writes a day on blocks rated for ~100 000 erases. On device 9e7c it did
// not even survive that long: it divided by zero inside lfs_alloc and panicked
// the board every 60 s, from the first history write after every boot.
//
// Appending to the END of a file touches only the last block. So records go to
// the end of a segment, segments are fixed in number and recycled oldest-first,
// and nothing is ever rewritten in place — not even a header, which is why the
// segment header carries no count. The count is the file length.
//
// WHAT IT COSTS
//
// Retention becomes granular. A whole segment is dropped at once, so the number
// of records actually held swings between (segments - 1) * recordsPerSegment
// and segments * recordsPerSegment rather than sitting exactly at capacity.
// Eight segments put that swing at 12.5 %, which is cheaper than the ring by
// every measure that matters here.

namespace segment {

// How many segment files the history is spread across. Eight is a compromise:
// more segments mean finer retention granularity and a smaller loss at each
// rotation, fewer mean less per-file block slack (LittleFS rounds every file up
// to a 4 KB block, so eight segments cost up to 32 KB of slack).
static const uint8_t kSegments = 8;

// Records per segment for a requested total capacity. Rounded UP, so the
// history holds at least what was asked for rather than silently less.
inline uint16_t
recordsPerSegment(uint32_t capacity, uint8_t segments)
{
    if (segments == 0 || capacity == 0) {
        return 0;
    }
    const uint32_t per = (capacity + segments - 1) / segments;
    return (per > 0xFFFFU) ? 0xFFFFU : (uint16_t)per;
}

// Maps a logical index — 0 is the OLDEST record held — onto the segment slot
// holding it and the record's offset inside that segment.
//
// `order` lists slot numbers oldest-first; `counts` is indexed by SLOT, not by
// position in `order`. Keeping those two spaces distinct is the whole reason
// this is a function with a test rather than three lines inlined at the call
// site: indexing `counts` by the loop variable instead of by `order[i]` gives
// answers that are right whenever the slots happen to be in order and wrong
// forever after the first rotation.
inline bool
locate(uint32_t index,
       const uint16_t* counts,
       const uint8_t* order,
       uint8_t orderCount,
       uint8_t& slotOut,
       uint32_t& offsetOut)
{
    if (counts == nullptr || order == nullptr) {
        return false;
    }

    uint32_t seen = 0;
    for (uint8_t i = 0; i < orderCount; ++i) {
        const uint8_t slot = order[i];
        const uint32_t here = counts[slot];
        if (index < seen + here) {
            slotOut = slot;
            offsetOut = index - seen;
            return true;
        }
        seen += here;
    }
    return false; // past the end: the caller is asking for a record not held
}

// The slot the next segment should be written into: an unused one if there is
// any, otherwise the one holding the oldest records.
//
// `seq` is indexed by slot; 0 means the slot holds nothing. Sequence numbers
// only ever increase, so "oldest" is "smallest non-zero seq" and no timestamp
// is involved — a device whose clock jumps backwards after an NTP sync must
// still recycle its segments in the order it wrote them.
inline uint8_t
slotToRecycle(const uint32_t* seq, uint8_t segments)
{
    uint8_t best = 0;
    uint32_t bestSeq = 0xFFFFFFFFUL;
    for (uint8_t i = 0; i < segments; ++i) {
        if (seq[i] == 0) {
            return i; // unused: nothing to throw away
        }
        if (seq[i] < bestSeq) {
            bestSeq = seq[i];
            best = i;
        }
    }
    return best;
}

// Sorts slot numbers oldest-first by sequence, skipping unused slots. Returns
// how many slots are in use. Insertion sort over at most eight entries.
inline uint8_t
buildOrder(const uint32_t* seq, uint8_t segments, uint8_t* orderOut)
{
    uint8_t n = 0;
    for (uint8_t slot = 0; slot < segments; ++slot) {
        if (seq[slot] == 0) {
            continue;
        }
        uint8_t at = n;
        while (at > 0 && seq[orderOut[at - 1]] > seq[slot]) {
            orderOut[at] = orderOut[at - 1];
            --at;
        }
        orderOut[at] = slot;
        ++n;
    }
    return n;
}

} // namespace segment
