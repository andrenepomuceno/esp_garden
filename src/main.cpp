#include "core/filesystem.h"
#include "BuildConfig.h"
#include "core/config.h"
#include "core/io_history.h"
#include "core/moisture_model.h"
#include "core/logger.h"
#include "core/tasks.h"
#include "core/user_store.h"
#include "network/web.h"
#include <Arduino.h>
#include <esp_system.h>

// esp_reset_reason() returns an enum; the names are worth spelling out because
// the number alone sends the reader to a header. Not static: the ThingsBoard
// connect publishes it too, so the cloud gets a timeline of reboots rather than
// a line in an 8 KB log that rotates.
const char*
resetReasonName()
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_EXT:      return "external reset pin";
        case ESP_RST_SW:       return "software (ESP.restart)";
        case ESP_RST_PANIC:    return "PANIC or exception";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "other watchdog";
        case ESP_RST_DEEPSLEEP:return "deep sleep wake";
        case ESP_RST_BROWNOUT: return "BROWNOUT (supply dipped)";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "unknown";
    }
}

void
setup(void)
{
    bool error = false;

    // First statement on purpose. Until a pin is driven it floats, and an
    // active-low relay board reads that as "energise" — everything below
    // (SPIFFS mount, config load, WiFi association) takes seconds, which is
    // long enough to run a pump dry. Uses the compiled defaults; loadConfigFile
    // parks the pins again once the real assignment is known.
    relayPinsSafeInit();

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    logger.info("");
    logger.info("Initializing ESP Garden " FW_VERSION "...");

    digitalWrite(LED_BUILTIN, 1);

    // Why the last boot happened. A panic, a watchdog and a power cut are
    // three completely different investigations, and without this line all
    // three look identical: "it restarted".
    logger.info(String("Reset reason: ") + resetReasonName());
    logger.info("Free heap at boot: " + String(ESP.getFreeHeap() / 1024) + " KB");

    unsigned id = ESP.getEfuseMac() % 0x10000;
    logger.info("ID: " + String(id, 16));

    // Mount without formatting FIRST, so that a partition which fails to mount
    // is named before it is erased.
    //
    // begin(true) formats on failure, and that is the recovery path for a
    // factory-blank partition — but it is also what happens on the first boot
    // of a LittleFS firmware flashed over the air onto a board whose partition
    // is still SPIFFS. The mount fails, the partition is formatted, and
    // /config.json goes with it: the device comes up on compiled defaults it
    // cannot associate with, has no users, and is unreachable without USB. The
    // hazard is documented in core/filesystem.h; this at least makes the boot
    // that caused it say so, which is the one thing someone recovering over
    // serial actually needs.
    if (!FILESYSTEM.begin(false)) {
        logger.error("FILESYSTEM did not mount. If this firmware was just "
                     "updated over the air onto a board that still had a "
                     "SPIFFS partition, the next line ERASES it — including "
                     "/config.json and every credential. Recover by flashing "
                     "the filesystem image over USB.");
        if (!FILESYSTEM.begin(true)) {
            logger.error("Failed to initialize FILESYSTEM.");
            error = true;
        } else {
            logger.error("FILESYSTEM was reformatted; stored files are gone.");
            error = true;
        }
    }

    if (!loadConfigFile(id)) {
        error = true;
    }

#if USE_CUSTOM_LOGIN
    // Seeded from the legacy ota.{username,password} pair on first boot, so a
    // device that already had a config gets an account without any password
    // being compiled into the firmware.
    if (!userStore.load(FILESYSTEM, config.otaUser, config.otaPassword)) {
        error = true;
    }
#endif

    // Set before tasksSetup(): that call blocks until the device has internet
    // and a valid clock, which never happens when the config failed to load and
    // the compiled WiFi defaults cannot associate. Assigning it afterwards left
    // the only failure indicator unreachable in exactly the case it exists for.
    g_ledBlinkEnabled = error;

    // After the config load so the capacity is the configured one, and before
    // webSetup() so /history.json never sees a half-open buffer.
    ioHistory.begin((uint16_t)config.historyRecords);

    // After the config load, because it reports per configured probe, and
    // before webSetup() so /moisture.json never answers from a zeroed model
    // that has not been read off flash yet.
    moistureModelSetup();

    logger.backupSetup();
    webSetup();
    tasksSetup();

    digitalWrite(LED_BUILTIN, 0);
}

void
loop(void)
{
    tasksLoop();
}
