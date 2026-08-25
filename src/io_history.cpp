#include "core/io_history.h"
#include "core/logger.h"

IoHistory ioHistory;

#define IO_SEGMENT_HEADER_SIZE ((uint32_t)sizeof(IoSegmentHeader))

String
IoHistory::pathFor(uint8_t slot)
{
    // Short on purpose: the upload endpoint refuses paths over
    // FILESYSTEM_MAX_PATH, and these names are also what a person sees when
    // browsing /spiffs/.
    return String("/hist") + String((int)slot) + ".bin";
}

void
IoHistory::closeReader(Reader& reader)
{
    if (reader.slot >= 0) {
        reader.file.close();
        reader.slot = -1;
    }
}

// Reads one segment off disk into the table, or deletes it if this build cannot
// make sense of it. Caller holds the mutex.
bool
IoHistory::adoptSegmentLocked(uint8_t slot)
{
    const String path = pathFor(slot);
    segmentSeq[slot] = 0;
    segmentCount[slot] = 0;

    if (!fs->exists(path)) {
        return false;
    }

    File file = fs->open(path, FILE_READ);
    if (file == false) {
        return false;
    }

    IoSegmentHeader onDisk = {};
    const bool readOk =
      file.read((uint8_t*)&onDisk, sizeof(onDisk)) == (int)sizeof(onDisk);
    const uint32_t size = (uint32_t)file.size();
    file.close();

    // Every field has to agree. A firmware that changed the record layout would
    // otherwise read the old file as garbage and publish it as history.
    if (!readOk || onDisk.magic != IO_HISTORY_MAGIC ||
        onDisk.recordSize != sizeof(IoRecord) ||
        onDisk.records != segmentRecords || onDisk.seq == 0 ||
        size < IO_SEGMENT_HEADER_SIZE) {
        logger.warning("io_history: dropping unusable " + path);
        fs->remove(path);
        return false;
    }

    // The count is the file length, and a torn append leaves a tail that does
    // not divide evenly. Truncating it here — logically, by ignoring it — is
    // the whole benefit of keeping no count in the header: the partial record
    // is simply not one of the records.
    const uint32_t body = size - IO_SEGMENT_HEADER_SIZE;
    uint32_t records = body / sizeof(IoRecord);
    if (records > segmentRecords) {
        records = segmentRecords; // a longer file than this build would write
    }
    if (body % sizeof(IoRecord) != 0) {
        logger.warning("io_history: " + path + " ends in a partial record, " +
                       "ignoring " + String(body % sizeof(IoRecord)) + " bytes");
    }

    segmentSeq[slot] = onDisk.seq;
    segmentCount[slot] = (uint16_t)records;
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

    // The ring this replaced. It is dead weight now — 69 KB of a 512 KB
    // partition — and leaving it would also leave the file whose in-place
    // rewrites panicked the board, one config edit away from being used again.
    if (fs->exists(IO_HISTORY_LEGACY_FILE)) {
        fs->remove(IO_HISTORY_LEGACY_FILE);
        logger.info("io_history: removed the legacy ring " IO_HISTORY_LEGACY_FILE);
    }

    if (capacity == 0) {
        logger.info("io_history: disabled (capacity 0)");
        return false;
    }

    segmentRecords = segment::recordsPerSegment(capacity, segment::kSegments);
    if (segmentRecords == 0) {
        logger.error("io_history: capacity too small to segment");
        return false;
    }

    storedTotal = 0;
    evictedTotal = 0;
    nextSeq = 1;
    for (uint8_t slot = 0; slot < segment::kSegments; ++slot) {
        if (adoptSegmentLocked(slot)) {
            storedTotal += segmentCount[slot];
            if (segmentSeq[slot] >= nextSeq) {
                nextSeq = segmentSeq[slot] + 1;
            }
        }
    }

    orderCount =
      segment::buildOrder(segmentSeq, segment::kSegments, order);

    initialised = true;
    logger.info("io_history: ready, " + String(storedTotal) + "/" +
                String(this->capacity()) + " records across " + String(orderCount) +
                " of " + String((int)segment::kSegments) + " segments (" +
                String(segmentRecords) + " each)");
    return true;
}

bool
IoHistory::rotateLocked(uint8_t& slotOut)
{
    const uint8_t slot =
      segment::slotToRecycle(segmentSeq, segment::kSegments);

    // Whatever this slot held is being thrown away. Accounting for it BEFORE
    // the write means the logical index origin moves exactly once, and
    // forEach() can correct for it with evicted().
    if (segmentSeq[slot] != 0) {
        storedTotal -= segmentCount[slot];
        evictedTotal += segmentCount[slot];
    }
    segmentCount[slot] = 0;
    segmentSeq[slot] = 0;

    const String path = pathFor(slot);
    // FILE_WRITE truncates, which is the point: the segment is reused in place
    // rather than deleted and recreated, so the filesystem never has to find
    // space it did not already have.
    File file = fs->open(path, FILE_WRITE, true);
    if (file == false) {
        logger.error("io_history: cannot open " + path + " for rotation");
        orderCount = segment::buildOrder(segmentSeq, segment::kSegments, order);
        return false;
    }

    IoSegmentHeader head = {};
    head.magic = IO_HISTORY_MAGIC;
    head.recordSize = (uint16_t)sizeof(IoRecord);
    head.records = segmentRecords;
    head.seq = nextSeq;

    const bool ok =
      file.write((const uint8_t*)&head, sizeof(head)) == sizeof(head);
    file.close();

    if (!ok) {
        logger.error("io_history: header write failed for " + path +
                     " — is the filesystem full?");
        fs->remove(path);
        orderCount = segment::buildOrder(segmentSeq, segment::kSegments, order);
        return false;
    }

    segmentSeq[slot] = nextSeq++;
    orderCount = segment::buildOrder(segmentSeq, segment::kSegments, order);
    slotOut = slot;
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

    uint8_t slot = 0;
    bool haveSlot = false;
    if (orderCount > 0) {
        const uint8_t newest = order[orderCount - 1];
        if (segmentCount[newest] < segmentRecords) {
            slot = newest;
            haveSlot = true;
        }
    }
    if (!haveSlot && !rotateLocked(slot)) {
        xSemaphoreGive(mutex);
        return false;
    }

    const String path = pathFor(slot);
    File file = fs->open(path, FILE_APPEND);
    if (file == false) {
        logger.error("io_history: cannot open " + path + " to append");
        xSemaphoreGive(mutex);
        return false;
    }

    // One write, at the end, and the close commits it. Under LittleFS that
    // touches the last block of the file and nothing else — which is the entire
    // reason this subsystem was rewritten.
    const bool ok = file.write((const uint8_t*)&record, sizeof(record)) ==
                    sizeof(record);
    file.close();

    if (!ok) {
        logger.error("io_history: record write failed on " + path);
        // The file may now carry a partial record. It is not corrected here:
        // begin() ignores a tail that does not divide evenly, and a second
        // append landing after a short write would put a record at a
        // misaligned offset. Refuse instead and let the next boot tidy it.
        xSemaphoreGive(mutex);
        return false;
    }

    ++segmentCount[slot];
    ++storedTotal;
    xSemaphoreGive(mutex);
    return true;
}

// Reads one record by logical index. Caller holds the mutex.
bool
IoHistory::readAt(Reader& reader, uint32_t index, IoRecord& out)
{
    uint8_t slot = 0;
    uint32_t offset = 0;
    if (!segment::locate(index, segmentCount, order, orderCount, slot, offset)) {
        return false;
    }

    if (reader.slot != (int)slot) {
        closeReader(reader);
        reader.file = fs->open(pathFor(slot), FILE_READ);
        if (reader.file == false) {
            return false;
        }
        reader.slot = (int)slot;
    }

    if (!reader.file.seek(IO_SEGMENT_HEADER_SIZE +
                          offset * (uint32_t)sizeof(IoRecord))) {
        return false;
    }
    return reader.file.read((uint8_t*)&out, sizeof(IoRecord)) ==
           (int)sizeof(IoRecord);
}

bool
IoHistory::lowerBoundLocked(Reader& reader, uint32_t sinceEpoch, uint32_t& out)
{
    uint32_t lo = 0, hi = storedTotal; // answer lives in [lo, hi]
    IoRecord record;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (!readAt(reader, mid, record)) {
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
IoHistory::readDecimatedLocked(Reader& reader, IoRecord* out, size_t maxPoints,
                               uint32_t fromIndex, uint32_t* strideOut)
{
    if (fromIndex >= storedTotal) {
        return 0;
    }

    const uint32_t span = storedTotal - fromIndex;
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
        if (!readAt(reader, storedTotal - 1 - back, record)) {
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
    if (!initialised || visit == nullptr || storedTotal == 0) {
        return 0;
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    // The lock is RELEASED every chunk, not held for the whole walk.
    //
    // A full walk is thousands of seek+reads — seconds. Holding the mutex
    // across it blocks readWindow(), which runs on the async_tcp task, and
    // blocking that task stalls every HTTP connection on the device rather
    // than just the one asking for history. The daily training run would take
    // the dashboard down with it.
    //
    // What moves under the walk is different now, and worse if ignored. The
    // ring shifted every logical index by ONE on each append once it had
    // wrapped; segments shift by a whole segment at a rotation — 180 records at
    // the default capacity. So the walk is kept in ABSOLUTE coordinates: the
    // ordinal of a record since the first append ever, from which the current
    // logical index is `absolute - evicted()`. A rotation during the walk then
    // costs the records it actually threw away and nothing more.
    static const uint32_t kChunk = 64;

    Reader reader;
    size_t visited = 0;
    IoRecord record;
    bool stop = false;
    uint32_t absolute = evictedTotal;

    for (uint32_t step = 0; !stop; ++step, ++absolute) {
        if (step > 0 && (step % kChunk) == 0) {
            closeReader(reader);
            xSemaphoreGive(mutex);
            // Somebody else gets the lock here, by design.
            taskYIELD();
            if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
                return visited;
            }
        }

        // Records evicted while the lock was down are simply gone; the walk
        // resumes at the oldest record still held rather than skipping ahead.
        if (absolute < evictedTotal) {
            absolute = evictedTotal;
        }
        const uint32_t index = absolute - evictedTotal;

        if (index >= storedTotal || !readAt(reader, index, record)) {
            break;
        }
        ++visited;
        stop = !visit(record, index, ctx);
    }

    closeReader(reader);
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
    if (!initialised || out == nullptr || maxPoints == 0 || storedTotal == 0) {
        return 0;
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    // One lock, both halves of the answer. See the header for what happened
    // when these were two calls.
    Reader reader;
    uint32_t from = 0;
    size_t count = 0;
    if (lowerBoundLocked(reader, sinceEpoch, from)) {
        count = readDecimatedLocked(reader, out, maxPoints, from, strideOut);
        if (fromOut) {
            *fromOut = from;
        }
    }

    closeReader(reader);
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

    const uint32_t available = storedTotal;
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

    Reader reader;
    size_t got = 0;
    for (size_t i = 0; i < wanted; ++i) {
        if (!readAt(reader, skip + (uint32_t)i, out[got])) {
            break;
        }
        ++got;
    }

    closeReader(reader);
    xSemaphoreGive(mutex);
    return got;
}
