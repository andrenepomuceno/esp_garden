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

    // Preallocate every slot so the file never grows later and a append can
    // never fail for lack of space halfway through a season.
    const IoRecord blank = {};
    for (uint16_t i = 0; i < capacity; ++i) {
        if (file.write((const uint8_t*)&blank, sizeof(blank)) != sizeof(blank)) {
            logger.error("io_history: preallocation failed at slot " +
                         String(i) + " — is SPIFFS full?");
            file.close();
            return false;
        }
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
IoHistory::writeHeader()
{
    File file = fs->open(IO_HISTORY_FILE, "r+");
    if (file == false) {
        return false;
    }
    file.seek(0);
    const bool ok =
      file.write((const uint8_t*)&header, sizeof(header)) == sizeof(header);
    file.close();
    return ok;
}

bool
IoHistory::append(const IoRecord& record)
{
    if (!initialised) {
        return false;
    }

    File file = fs->open(IO_HISTORY_FILE, "r+");
    if (file == false) {
        logger.error("io_history: cannot open for append");
        return false;
    }

    const uint32_t offset =
      IO_HISTORY_HEADER_SIZE + header.head * (uint32_t)sizeof(IoRecord);
    file.seek(offset);
    const bool ok =
      file.write((const uint8_t*)&record, sizeof(record)) == sizeof(record);

    if (ok) {
        header.head = (header.head + 1) % header.capacity;
        if (header.stored < header.capacity) {
            ++header.stored;
        }
        file.seek(0);
        file.write((const uint8_t*)&header, sizeof(header));
    }

    file.close();
    if (!ok) {
        logger.error("io_history: record write failed");
    }
    return ok;
}

size_t
IoHistory::read(IoRecord* out, size_t limit) const
{
    if (!initialised || out == nullptr || limit == 0) {
        return 0;
    }

    const uint32_t available = header.stored;
    const size_t wanted = (limit < available) ? limit : available;
    const uint32_t skip = available - (uint32_t)wanted; // keep the newest

    File file = fs->open(IO_HISTORY_FILE, FILE_READ);
    if (file == false) {
        return 0;
    }

    size_t got = 0;
    for (size_t i = 0; i < wanted; ++i) {
        const uint32_t slot = ring::slotOf(
          skip + (uint32_t)i, header.head, header.stored, header.capacity);
        file.seek(IO_HISTORY_HEADER_SIZE + slot * (uint32_t)sizeof(IoRecord));
        if (file.read((uint8_t*)&out[got], sizeof(IoRecord)) !=
            (int)sizeof(IoRecord)) {
            break;
        }
        ++got;
    }

    file.close();
    return got;
}
