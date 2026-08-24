#pragma once

#include "BuildConfig.h"
#include "core/ring_index.h"
#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <SPIFFS.h>

// Fixed-size ring buffer of I/O snapshots on SPIFFS.
//
// The file is preallocated to `capacity` records and never grows: the oldest
// record is overwritten in place. That bounds flash usage exactly — a full
// buffer costs `IO_HISTORY_HEADER_SIZE + capacity * sizeof(IoRecord)` bytes,
// forever — and removes the rotation logic the logger needs.
//
// The record layout is FIXED regardless of build flags: a board with one probe
// still writes four slots, filled with NaN. Making the layout depend on
// MOISTURE_SENSOR_COUNT would mean two firmwares disagree about how to read the
// same file, and the reader has no way to tell which wrote it.

#define IO_HISTORY_MAX_MOISTURE 4
#define IO_HISTORY_MAGIC 0x45474831UL // "EGH1"
#define IO_HISTORY_FILE "/io_history.bin"

#pragma pack(push, 1)
struct IoRecord
{
    uint32_t timestamp;  ///< epoch seconds; 0 marks an unwritten slot
    uint16_t relayMask;  ///< bit i set while relay i is energised
    uint16_t reserved;   ///< keeps the struct 4-byte aligned and leaves room
    float moisture[IO_HISTORY_MAX_MOISTURE];
    float luminosity;
    float temperature;
    float airHumidity;
    float waterLevel;
};

struct IoHistoryHeader
{
    uint32_t magic;
    uint16_t recordSize; ///< sizeof(IoRecord) as written
    uint16_t capacity;   ///< records the file holds
    uint32_t head;       ///< slot the next append writes to
    uint32_t stored;     ///< records ever written, saturating at capacity
};
#pragma pack(pop)

class IoHistory
{
  public:
    // Opens the file, or creates it when missing. Any disagreement about
    // magic, record size or capacity discards the file and starts over: a
    // half-understood binary log is worse than none.
    bool begin(uint16_t capacity, FS& filesystem = SPIFFS);

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
    // append() landing between them shifted every logical index: once the ring
    // has wrapped, `stored` stays at capacity while `head` advances, so index
    // i names a different record after each append. A request arriving on the
    // history task's append boundary got a window starting one record late,
    // and in the limit `from` could equal `stored` — zero records returned for
    // a window that plainly has data, with no error to explain it.
    size_t readWindow(uint32_t sinceEpoch, IoRecord* out, size_t maxPoints,
                      uint32_t* strideOut, uint32_t* fromOut);

    uint16_t capacity() const { return header.capacity; }
    uint32_t stored() const { return header.stored; }
    bool ready() const { return initialised; }

  private:
    // Both assume the mutex is held and the file is open. `lowerBoundLocked`
    // reports failure rather than returning a half-converged index: a readAt()
    // error mid-search used to `break` and hand back whatever `lo` had reached,
    // which reads as a valid answer.
    bool lowerBoundLocked(File& file, uint32_t sinceEpoch, uint32_t& out);
    size_t readDecimatedLocked(File& file, IoRecord* out, size_t maxPoints,
                               uint32_t fromIndex, uint32_t* strideOut);

    IoHistoryHeader header = {};
    FS* fs = nullptr;
    bool initialised = false;

    bool format(uint16_t capacity);
    bool readAt(File& file, uint32_t index, IoRecord& out) const;

    // append() runs on loop(); read() runs on the async_tcp task. Both touch
    // the same header and the same file, so without this a request landing
    // mid-append reads a half-written record and a head that moved under it.
    SemaphoreHandle_t mutex = nullptr;
};

extern IoHistory ioHistory;
