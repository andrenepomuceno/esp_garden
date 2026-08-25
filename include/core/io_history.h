#pragma once

#include "BuildConfig.h"
#include "core/filesystem.h"
#include "core/segment_index.h"
#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Append-only history of I/O snapshots, spread across a fixed set of segment
// files on FILESYSTEM.
//
// Records are written to the END of the newest segment and nothing is ever
// rewritten in place — not a record, not a header. When the newest segment is
// full the oldest is recycled: truncated, stamped with a new sequence number,
// and written into. Flash usage is therefore bounded at
// `kSegments * (sizeof(IoSegmentHeader) + recordsPerSegment * sizeof(IoRecord))`
// forever, which is what the old ring bought, without the in-place writes that
// made it fatal on a copy-on-write filesystem. See core/segment_index.h for
// what happened and why the shape had to change.
//
// The segment header deliberately carries NO record count. The count is the
// file length, so an append is one write at the end and a torn append leaves a
// partial record that simply does not divide evenly and is ignored. Storing a
// count would mean rewriting the header on every append — reintroducing the
// exact mid-file write this design exists to remove — and would make a torn
// write a disagreement between two places instead of a short tail.
//
// The record layout is FIXED regardless of build flags: a board with one probe
// still writes four slots, filled with NaN. Making the layout depend on the
// fitted probe count would mean two firmwares disagree about how to read the
// same file, and the reader has no way to tell which wrote it.
//
// Changing the layout DISCARDS the stored history. The header carries
// `recordSize` and begin() drops any segment that disagrees — which is the
// point: reading 40-byte records out of a 48-byte file would silently return
// garbage that looks like data.

#define IO_HISTORY_MAX_MOISTURE 4

// "EGH2". Bumped from EGH1 with the move to segments: an old single-file
// /io_history.bin must not be mistaken for a segment, and the new files have
// new names anyway, so the old one is simply deleted at begin().
#define IO_HISTORY_MAGIC 0x45474832UL
#define IO_HISTORY_LEGACY_FILE "/io_history.bin"

#pragma pack(push, 1)
struct IoRecord
{
    uint32_t timestamp;  ///< epoch seconds; 0 marks an unwritten slot
    uint16_t relayMask;  ///< bit i set while relay i is energised
    uint16_t flags;      ///< see IO_HISTORY_FLAG_*
    float moisture[IO_HISTORY_MAX_MOISTURE];
    float luminosity;
    float temperature;
    float airHumidity;
    float waterLevel;
    float flowRate;      ///< litres/min, mean over the period; NaN when unfitted
    float flowTotal;     ///< cumulative litres since boot; NaN when unfitted
};

// The float switch is one bit, so it rides in `flags` rather than costing a
// whole float. VALID distinguishes "not fitted" from "reads empty" — without
// it every board without the sensor would look like a dry reservoir.
#define IO_HISTORY_FLAG_FLOAT_VALID  0x0001
#define IO_HISTORY_FLAG_FLOAT_RAISED 0x0002

struct IoSegmentHeader
{
    uint32_t magic;
    uint16_t recordSize; ///< sizeof(IoRecord) as written
    uint16_t records;    ///< this segment's capacity, to catch a config change
    uint32_t seq;        ///< monotonic, higher is newer; 0 is never written
};
#pragma pack(pop)

// A probe with no slot in the record would be read, charted live and then
// silently absent from history. MOISTURE_MAX is capacity for the config
// loader; IO_HISTORY_MAX_MOISTURE is capacity for the on-disk record. They are
// the same number, and this is what makes raising one without the other a
// compile error instead of a gap in the data.
static_assert(MOISTURE_MAX == IO_HISTORY_MAX_MOISTURE,
              "MOISTURE_MAX and IO_HISTORY_MAX_MOISTURE must match: a probe "
              "without a record slot vanishes from stored history.");

class IoHistory
{
  public:
    // Adopts whatever segments are already on disk and agree with this build.
    // A segment that disagrees about magic, record size or per-segment capacity
    // is deleted rather than reinterpreted: a half-understood binary log is
    // worse than none.
    //
    // Nothing is preallocated. The old design wrote the whole file at begin()
    // so an append could never fail for space; with append-only segments the
    // space is taken as it is used and released a whole segment at a time, and
    // after the first full cycle no new space is ever needed.
    bool begin(uint16_t capacity, FS& filesystem = FILESYSTEM);

    bool append(const IoRecord& record);

    // Copies up to `limit` records into `out`, oldest first, skipping the
    // `offset` oldest. Returns how many. Pass offset = kNewest for the tail.
    //
    // Without an offset only the newest `limit` records were ever reachable, so
    // a 1440-record buffer exposed its final 14 % and the rest was written and
    // never read.
    static const uint32_t kNewest = 0xFFFFFFFFUL;
    size_t read(IoRecord* out, size_t limit, uint32_t offset = kNewest);

    // Copies at most `maxPoints` records stamped at or after `sinceEpoch`,
    // oldest first, decimating so a 24 h window fits a 200-point response.
    // Reports the stride used and the logical index the window started at.
    //
    // Locating the window and reading it are ONE operation on purpose. They
    // used to be two public calls, each taking the mutex on its own, and an
    // append() landing between them shifted every logical index.
    size_t readWindow(uint32_t sinceEpoch, IoRecord* out, size_t maxPoints,
                      uint32_t* strideOut, uint32_t* fromOut);

    // Streams every stored record, oldest first. Returns how many it visited;
    // the visitor returning false stops the walk early.
    //
    // Exists for the moisture trainer, which needs three passes over the whole
    // buffer and must not hold it in RAM: 1440 records is 69 KB, and the
    // sufficient statistics it is building are twelve doubles.
    using Visitor = bool (*)(const IoRecord& record, uint32_t index, void* ctx);
    size_t forEach(Visitor visit, void* ctx);

    uint32_t capacity() const
    {
        return (uint32_t)segmentRecords * segment::kSegments;
    }
    uint32_t stored() const { return storedTotal; }
    bool ready() const { return initialised; }

    // Records evicted since boot, i.e. how far the logical index origin has
    // moved. forEach() uses it to stay aligned across the lock it releases.
    uint32_t evicted() const { return evictedTotal; }

  private:
    // Holds one segment open across consecutive reads. A logical walk crosses a
    // segment boundary only every `segmentRecords` records, and a binary search
    // touches about eleven, so reopening only when the SLOT changes turns what
    // would be one open per record into a handful.
    struct Reader
    {
        File file;
        int slot = -1;
    };

    bool readAt(Reader& reader, uint32_t index, IoRecord& out);
    static void closeReader(Reader& reader);

    // Both assume the mutex is held. `lowerBoundLocked` reports failure rather
    // than returning a half-converged index: a readAt() error mid-search used
    // to `break` and hand back whatever `lo` had reached, which reads as a
    // valid answer.
    bool lowerBoundLocked(Reader& reader, uint32_t sinceEpoch, uint32_t& out);
    size_t readDecimatedLocked(Reader& reader, IoRecord* out, size_t maxPoints,
                               uint32_t fromIndex, uint32_t* strideOut);

    // Recycles the oldest segment (or claims an unused one) and makes it the
    // newest. Assumes the mutex is held.
    bool rotateLocked(uint8_t& slotOut);
    static String pathFor(uint8_t slot);
    bool adoptSegmentLocked(uint8_t slot);

    FS* fs = nullptr;
    bool initialised = false;

    uint16_t segmentRecords = 0;
    uint16_t segmentCount[segment::kSegments] = {}; ///< records held, by SLOT
    uint32_t segmentSeq[segment::kSegments] = {};   ///< 0 means unused, by SLOT
    uint8_t order[segment::kSegments] = {};         ///< slots, oldest first
    uint8_t orderCount = 0;
    uint32_t storedTotal = 0;
    uint32_t evictedTotal = 0;
    uint32_t nextSeq = 1;

    // append() runs on loop(); read() runs on the async_tcp task. Both touch
    // the same segment table and the same files, so without this a request
    // landing mid-append reads a half-written record and a table that moved
    // under it.
    SemaphoreHandle_t mutex = nullptr;
};

extern IoHistory ioHistory;
