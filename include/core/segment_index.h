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


// ---------------------------------------------------------------------------
// DOES THE REQUESTED CAPACITY FIT?
//
// Nothing is preallocated: segments take space as they use it. So a capacity
// the partition cannot hold is accepted at boot, grows for days, and then runs
// the filesystem out from inside append() — long after the change that caused
// it, on a board that is reachable only over the air. That is the worst
// possible shape for this failure, and the static ceiling in
// ConfigFile::loadFile() cannot catch it, because what fits is a property of
// the DEVICE and not of the firmware: it depends on how much of the partition
// the web assets, the log backups and the moisture model already hold, and
// those move with every deploy.
//
// The ceiling says what the RECORD costs. This says what the FILESYSTEM has.
// Both are needed and neither replaces the other.

// LittleFS allocates whole erase blocks, so a segment costs its length rounded
// UP to a block. 4096 is the flash sector size and the ESP32 default; it is a
// constant because no public API reports it back, and a block SMALLER than a
// sector cannot exist on this flash, so an estimate built on it is wrong only
// in the conservative direction unless somebody configures a larger block.
//
// Ignoring this would under-count by up to a block per segment — 32 KB across
// eight, which is a fifth of what is free on the board this was written for.
static const uint32_t kBlockBytes = 4096;

// Space the history is never allowed to take. Two terms answering two
// different questions; the reserve is whichever is larger.
//
// The FLOOR covers consumers that do not scale with the partition:
//   - POST /spiffs/upload writes /upload.tmp and then renames, so both copies
//     exist at once, and it refuses outright when the incoming size exceeds
//     free space. The largest asset shipped is bootstrap.css.gz at 29 899 B.
//     That endpoint is the recovery path that exists so ONE web asset can be
//     replaced without the filesystem OTA that overwrites /config.json --
//     history starving it is history removing the tool used to undo history.
//   - /log0..3.txt are append-only within a boot and nothing truncates them:
//     Logger::backup() opens FILE_APPEND every hour and writes every line
//     since the last run. Their size is bounded by uptime and by nothing else.
//   - LittleFS is copy-on-write. An append allocates a new block before it
//     frees the old, so a partition with no free block cannot write at all --
//     including the writes that would free space.
//
// The FRACTION covers the part that really is proportional: an allocator
// hunting for free blocks in a partition run down to its last one is slow and
// fragile whatever that partition's size.
//
// On the 512 KB partition this firmware ships with, the two terms coincide at
// 64 KB. That is arithmetic and not a measurement: no board has yet been run
// to the point where either term was tested.
static const uint32_t kReserveFloorBytes = 64u * 1024u;
static const uint32_t kReserveDivisor = 8;

inline uint32_t
reserveBytes(uint32_t partitionBytes)
{
    const uint32_t proportional = partitionBytes / kReserveDivisor;
    return (proportional > kReserveFloorBytes) ? proportional : kReserveFloorBytes;
}

// What `capacity` records occupy on flash once every segment has filled --
// which is the number that matters, because that is the state the history
// walks towards and stays in for the life of the device.
inline uint32_t
storageBytes(uint32_t capacity,
             uint32_t headerBytes,
             uint32_t recordBytes,
             uint8_t segments = kSegments,
             uint32_t blockBytes = kBlockBytes)
{
    if (segments == 0 || capacity == 0 || recordBytes == 0 ||
        blockBytes == 0) {
        return 0;
    }
    // Deliberately the same rounding the segments are actually built with, so
    // the cost estimate and the layout cannot drift apart.
    const uint32_t per = recordsPerSegment(capacity, segments);
    const uint32_t raw = headerBytes + per * recordBytes;
    const uint32_t blocks = (raw + blockBytes - 1) / blockBytes;
    return blocks * blockBytes * segments;
}

// The largest capacity whose storageBytes() fits inside `budgetBytes`. The
// inverse of the function above, and it must stay exactly that: a budget is
// split evenly across the segments and then floored to whole blocks, because
// a part-used block is not available to anything else.
inline uint32_t
capacityForBytes(uint32_t budgetBytes,
                 uint32_t headerBytes,
                 uint32_t recordBytes,
                 uint8_t segments = kSegments,
                 uint32_t blockBytes = kBlockBytes)
{
    if (segments == 0 || recordBytes == 0 || blockBytes == 0) {
        return 0;
    }
    const uint32_t blocks = (budgetBytes / segments) / blockBytes;
    const uint32_t usable = blocks * blockBytes;
    if (usable <= headerBytes) {
        return 0;
    }
    return ((usable - headerBytes) / recordBytes) * segments;
}

// The capacity actually granted: never more than was asked for, never more
// than fits.
//
// `historyBytes` is what the segments hold RIGHT NOW and is added to the free
// space, because the new capacity replaces the old rather than sitting beside
// it. Passing the files' logical lengths there under-credits the history by
// its own block slack, which errs towards a smaller budget — the right
// direction for a check whose whole job is to refuse.
//
// A `partitionBytes` of 0 means the filesystem could not be asked, and the
// answer is then to grant what was requested rather than to clamp on a number
// nobody produced: an unmounted filesystem is not evidence of a full one.
inline uint32_t
fitCapacity(uint32_t requested,
            uint32_t partitionBytes,
            uint32_t freeBytes,
            uint32_t historyBytes,
            uint32_t headerBytes,
            uint32_t recordBytes,
            uint8_t segments = kSegments,
            uint32_t blockBytes = kBlockBytes)
{
    if (requested == 0 || partitionBytes == 0) {
        return requested;
    }

    const uint32_t available = freeBytes + historyBytes;
    const uint32_t reserve = reserveBytes(partitionBytes);
    if (available <= reserve) {
        return 0;
    }

    const uint32_t fits = capacityForBytes(
      available - reserve, headerBytes, recordBytes, segments, blockBytes);
    return (fits < requested) ? fits : requested;
}

} // namespace segment
