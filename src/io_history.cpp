#include "core/io_history.h"
#include "core/logger.h"

IoHistory ioHistory;

#define IO_HISTORY_HEADER_SIZE ((uint32_t)sizeof(IoHistoryHeader))

bool
IoHistory::format(uint16_t capacity)
{
    File file = fs->open(IO_HISTORY_FILE, FILE_WRITE, true);
    if (file == false) {
        logger.error("io_history: cannot create " IO_HISTORY_FILE);
        return false;
    }

    header.magic = IO_HISTORY_MAGIC;
    header.recordSize = (uint16_t)sizeof(IoRecord);
    header.capacity = capacity;
    header.head = 0;
    header.stored = 0;

    if (file.write((const uint8_t*)&header, sizeof(header)) != sizeof(header)) {
        logger.error("io_history: header write failed");
        file.close();
        return false;
    }

    // Preallocate every slot so the file never grows later and an append can
    // never fail for lack of space halfway through a season. Written in blocks:
    // one VFS call per record meant 1440 round-trips with the web server not
    // yet up, and 6000 at the configured ceiling.
    static const size_t kBlockRecords = 16;
    uint8_t block[kBlockRecords * sizeof(IoRecord)] = {};
    uint16_t remaining = capacity;
    while (remaining > 0) {
        const size_t batch =
          (remaining < kBlockRecords) ? remaining : kBlockRecords;
        const size_t bytes = batch * sizeof(IoRecord);
        if (file.write(block, bytes) != bytes) {
            logger.error("io_history: preallocation failed with " +
                         String(remaining) + " records left — is SPIFFS full?");
            file.close();
            // Leaving the partial file behind would keep the space it already
            // took and the next boot would try again on a fuller filesystem.
            fs->remove(IO_HISTORY_FILE);
            return false;
        }
        remaining -= batch;
    }

    file.close();
    logger.info("io_history: formatted " IO_HISTORY_FILE " for " +
                String(capacity) + " records (" +
                String(IO_HISTORY_HEADER_SIZE + (uint32_t)capacity * sizeof(IoRecord)) +
                " bytes)");
    return true;
}

bool
IoHistory::begin(uint16_t capacity, FS& filesystem)
{
    fs = &filesystem;
    initialised = false;

    if (mutex == nullptr) {
        mutex = xSemaphoreCreateMutex();
        if (mutex == nullptr) {
            // Every other allocation here is checked; leaving this one meant
            // append() would call xSemaphoreTake(nullptr, portMAX_DELAY).
            logger.error("io_history: cannot create mutex; history disabled");
            return false;
        }
    }

    if (capacity == 0) {
        logger.info("io_history: disabled (capacity 0)");
        return false;
    }

    bool needsFormat = true;
    if (fs->exists(IO_HISTORY_FILE)) {
        File file = fs->open(IO_HISTORY_FILE, FILE_READ);
        if (file != false) {
            IoHistoryHeader onDisk = {};
            if (file.read((uint8_t*)&onDisk, sizeof(onDisk)) == sizeof(onDisk)) {
                const uint32_t expected =
                  IO_HISTORY_HEADER_SIZE +
                  (uint32_t)onDisk.capacity * onDisk.recordSize;
                // Every field has to agree. A firmware that changed the record
                // layout would otherwise read the old file as garbage and
                // publish it as history.
                if (onDisk.magic == IO_HISTORY_MAGIC &&
                    onDisk.recordSize == sizeof(IoRecord) &&
                    onDisk.capacity == capacity &&
                    onDisk.head < onDisk.capacity &&
                    // Without this a corrupt `stored` (a torn header write, a
                    // flipped bit) is accepted: read() computes
                    // skip = stored - wanted and serves arbitrary slots in
                    // scrambled order, and append() stops counting for good.
                    onDisk.stored <= onDisk.capacity &&
                    // Before the buffer wraps the two are the same number.
                    // Checking them only separately lets head=5/stored=3 pass,
                    // and read() then serves preallocated zeros while the real
                    // records in slots 3 and 4 are unreachable.
                    (onDisk.stored == onDisk.capacity ||
                     onDisk.head == onDisk.stored) &&
                    (uint32_t)file.size() == expected) {
                    header = onDisk;
                    needsFormat = false;
                }
            }
            file.close();
        }
        if (needsFormat) {
            logger.warning("io_history: incompatible or corrupt file, recreating");
        }
    }

    if (needsFormat && !format(capacity)) {
        return false;
    }

    initialised = true;
    logger.info("io_history: ready, " + String(header.stored) + "/" +
                String(header.capacity) + " records");
    return true;
}

bool
IoHistory::append(const IoRecord& record)
{
    if (!initialised) {
        return false;
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    File file = fs->open(IO_HISTORY_FILE, "r+");
    if (file == false) {
        logger.error("io_history: cannot open for append");
        xSemaphoreGive(mutex);
        return false;
    }

    const uint32_t offset =
      IO_HISTORY_HEADER_SIZE + header.head * (uint32_t)sizeof(IoRecord);

    // An unchecked seek writes the record wherever the handle happened to sit —
    // offset 0 means straight over the header.
    bool ok = file.seek(offset);
    if (ok) {
        ok = file.write((const uint8_t*)&record, sizeof(record)) == sizeof(record);
    }

    if (ok) {
        const IoHistoryHeader previous = header;
        header.head = (header.head + 1) % header.capacity;
        if (header.stored < header.capacity) {
            ++header.stored;
        }

        // The header write must be checked too. Dropping it leaves RAM ahead of
        // disk, and the next boot rewinds `head` and overwrites live records
        // while claiming they are the newest.
        const bool headerOk =
          file.seek(0) &&
          file.write((const uint8_t*)&header, sizeof(header)) == sizeof(header);
        if (!headerOk) {
            header = previous;
            logger.error("io_history: header update failed; record dropped");
            ok = false;
        }
    }

    file.close();
    xSemaphoreGive(mutex);
    if (!ok) {
        logger.error("io_history: append failed");
    }
    return ok;
}

// Reads one record by logical index. Caller holds the mutex.
bool
IoHistory::readAt(File& file, uint32_t index, IoRecord& out) const
{
    const uint32_t slot =
      ring::slotOf(index, header.head, header.stored, header.capacity);
    if (!file.seek(IO_HISTORY_HEADER_SIZE + slot * (uint32_t)sizeof(IoRecord))) {
        return false;
    }
    return file.read((uint8_t*)&out, sizeof(IoRecord)) == (int)sizeof(IoRecord);
}

bool
IoHistory::lowerBoundLocked(File& file, uint32_t sinceEpoch, uint32_t& out)
{
    uint32_t lo = 0, hi = header.stored; // answer lives in [lo, hi]
    IoRecord record;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (!readAt(file, mid, record)) {
            return false;
        }
        if (record.timestamp < sinceEpoch) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    out = lo;
    return true;
}

size_t
IoHistory::readDecimatedLocked(File& file, IoRecord* out, size_t maxPoints,
                               uint32_t fromIndex, uint32_t* strideOut)
{
    if (fromIndex >= header.stored) {
        return 0;
    }

    const uint32_t span = header.stored - fromIndex;
    // Ceiling division: with 1440 records and 200 points the stride is 8, and
    // the newest record must still be the last one returned.
    uint32_t stride = (span + (uint32_t)maxPoints - 1) / (uint32_t)maxPoints;
    if (stride == 0) {
        stride = 1;
    }
    if (strideOut) {
        *strideOut = stride;
    }

    size_t got = 0;
    uint16_t bucketRelays = 0;
    IoRecord bucket;
    bool haveBucket = false;

    // Walk backwards from the newest so the last sample is always present —
    // striding forwards would drop it whenever span is not a multiple.
    //
    // Every record in the bucket is read, not just the one that represents it.
    // The analog channels can be sampled: they move slowly and one record in
    // `stride` is a fair stand-in. The relay mask cannot. It is deliberately
    // STICKY — set for the whole period if the relay was on at any point in it,
    // because a watering lasts seconds and a 60 s record would otherwise miss
    // nine activations in ten. Sampling it re-introduces exactly that bug one
    // level up: at the default one-day window the stride is 8, so seven of
    // every eight waterings would vanish from the chart and the moisture rise
    // would again appear with no cause. So the mask is OR-ed across the bucket
    // while the rest of the record comes from the bucket's newest sample.
    for (uint32_t back = 0; back < span && got < maxPoints; ++back) {
        IoRecord record;
        if (!readAt(file, header.stored - 1 - back, record)) {
            break;
        }

        if (!haveBucket) {
            bucket = record;
            haveBucket = true;
        }
        bucketRelays |= record.relayMask;

        const bool bucketFull = ((back % stride) == stride - 1);
        if (bucketFull || back == span - 1) {
            bucket.relayMask = bucketRelays;
            out[got] = bucket;
            ++got;
            haveBucket = false;
            bucketRelays = 0;
        }
    }

    // Collected newest-first; the callers all want oldest-first.
    for (size_t i = 0; i < got / 2; ++i) {
        IoRecord tmp = out[i];
        out[i] = out[got - 1 - i];
        out[got - 1 - i] = tmp;
    }
    return got;
}


size_t
IoHistory::forEach(Visitor visit, void* ctx)
{
    if (!initialised || visit == nullptr || header.stored == 0) {
        return 0;
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    File file = fs->open(IO_HISTORY_FILE, FILE_READ);
    if (file == false) {
        xSemaphoreGive(mutex);
        return 0;
    }

    size_t visited = 0;
    IoRecord record;
    for (uint32_t i = 0; i < header.stored; ++i) {
        if (!readAt(file, i, record)) {
            break;
        }
        ++visited;
        if (!visit(record, i, ctx)) {
            break;
        }
    }

    file.close();
    xSemaphoreGive(mutex);
    return visited;
}

size_t
IoHistory::readWindow(uint32_t sinceEpoch, IoRecord* out, size_t maxPoints,
                      uint32_t* strideOut, uint32_t* fromOut)
{
    if (strideOut) {
        *strideOut = 1;
    }
    if (fromOut) {
        *fromOut = 0;
    }
    if (!initialised || out == nullptr || maxPoints == 0 ||
        header.stored == 0) {
        return 0;
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    File file = fs->open(IO_HISTORY_FILE, FILE_READ);
    if (file == false) {
        xSemaphoreGive(mutex);
        return 0;
    }

    // One lock, one file handle, both halves of the answer. See the header for
    // what happened when these were two calls.
    uint32_t from = 0;
    size_t count = 0;
    if (lowerBoundLocked(file, sinceEpoch, from)) {
        count = readDecimatedLocked(file, out, maxPoints, from, strideOut);
        if (fromOut) {
            *fromOut = from;
        }
    }

    file.close();
    xSemaphoreGive(mutex);
    return count;
}

size_t
IoHistory::read(IoRecord* out, size_t limit, uint32_t offset)
{
    if (!initialised || out == nullptr || limit == 0) {
        return 0;
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    const uint32_t available = header.stored;
    uint32_t skip;
    if (offset == kNewest) {
        const uint32_t wanted = (limit < available) ? (uint32_t)limit : available;
        skip = available - wanted;
    } else {
        skip = offset;
    }

    if (skip >= available) {
        xSemaphoreGive(mutex);
        return 0;
    }

    const uint32_t remaining = available - skip;
    const size_t wanted = (limit < remaining) ? limit : remaining;

    File file = fs->open(IO_HISTORY_FILE, FILE_READ);
    if (file == false) {
        xSemaphoreGive(mutex);
        return 0;
    }

    size_t got = 0;
    for (size_t i = 0; i < wanted; ++i) {
        if (!readAt(file, skip + (uint32_t)i, out[got])) {
            break;
        }
        ++got;
    }

    file.close();
    xSemaphoreGive(mutex);
    return got;
}
