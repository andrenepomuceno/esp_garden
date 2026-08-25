#pragma once

#include <LittleFS.h>

// The one place that names the filesystem driver.
//
// Everything else says `FILESYSTEM`, so changing it again is this line rather
// than sixty substitutions across nineteen files — which is what it took to
// leave SPIFFS.
//
// WHY LITTLEFS
//
// **Power-loss resilience, on a board that needs it.** LittleFS is
// copy-on-write with atomic metadata: a cut mid-write leaves the previous
// version intact. SPIFFS has no such guarantee, and a corrupted SPIFFS meets
// `begin(true)`, which FORMATS — taking /config.json, and with it the WiFi and
// broker credentials, on a device that then cannot be reached to fix.
//
// This board prints `flash read err, 1000` on the first flash read of EVERY
// boot and restarts on the RTC watchdog. It writes a history record every 60 s
// and rotates a log every hour. That is exactly the combination SPIFFS loses.
//
// **Occupancy — and it costs space rather than saving it.** Measured on device
// 9e7c: SPIFFS reported 333 of 463 usable KB (72 %); LittleFS reports 388 of
// 512 KB (76 %). LittleFS allocates in 4 KB blocks where SPIFFS used 256 B
// pages, so thirty small web assets pay roughly 55 KB of internal slack. Free
// space went 130 KB -> 124 KB.
//
// The move is still right, because the number that matters is not occupancy but
// what occupancy DOES: SPIFFS mount and open times degrade sharply past ~75 %
// full and this partition was already at 72 % and growing. LittleFS has no such
// cliff. Trading 6 KB of free space for the removal of a cliff we were about to
// walk off is the trade; do not read the 76 % as a regression.
//
// **Maintenance.** SPIFFS has been in maintenance mode for years; LittleFS is
// where the ESP-IDF ecosystem went.
//
// WHAT IT COST
//
// The partition cannot be converted in place — it is reformatted, so the
// history ring and the moisture model start over. Both are disposable by
// design; /config.json and /users.json are not, which is why the migration was
// done over USB with a verified `?secrets=1` backup in hand rather than as an
// OTA. Over the air both orderings brick the device: firmware-first mounts a
// SPIFFS partition as LittleFS, fails, formats, and loses the credentials;
// filesystem-first is erased by the old firmware's own format on the next boot.
//
// `board_build.filesystem = littlefs` in platformio.ini is the matching half —
// without it the image is still built with mkspiffs and the device formats on
// first mount.
#define FILESYSTEM LittleFS

// LittleFS allows names up to 255 bytes where SPIFFS allowed 31. The upload
// endpoint still enforces 31, deliberately: nothing here needs a longer path,
// and a limit that was once a silent truncation is worth keeping as an explicit
// refusal.
#define FILESYSTEM_MAX_PATH 31
