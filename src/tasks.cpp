#include "core/tasks.h"
#include "BuildConfig.h"
#include "core/io_history.h"
#include "core/logger.h"
#include "core/relays.h"
#include "core/sensors.h"
#include "core/telemetry.h"
#include "network/mqtt.h"
#include "network/talkback.h"
#include "network/web.h"
#include <CriticalTaskScheduler.h>
#include <ESP32Ping.h>
#include <WiFi.h>

// The period constants the macros mint are read by src/sensors.cpp and
// src/telemetry.cpp, which size their accumulator windows and their publish
// queue from them, so they carry external linkage.
#define DECLARE_TASK(name, period)                                             \
    static void name##TaskHandler();                                           \
    extern const unsigned g_##name##TaskPeriod = period;                       \
    static TSTask g_##name##Task(#name, g_##name##TaskPeriod,                  \
                                 &name##TaskHandler)

// Critical tasks run on a dedicated FreeRTOS task instead of the cooperative
// loop() pump, so a blocking background handler cannot delay them.
#define DECLARE_CRITICAL_TASK(name, period)                                    \
    static void name##TaskHandler();                                           \
    extern const unsigned g_##name##TaskPeriod = period;                       \
    static TSTask g_##name##Task(#name, g_##name##TaskPeriod,                  \
                                 &name##TaskHandler, true)

static TSScheduler g_taskScheduler;
static TSFreeRTOSCriticalRunner g_criticalRunner(g_taskScheduler);

DECLARE_TASK(io, 1000);                         // 1 s
DECLARE_TASK(clockUpdate, 24 * 60 * 60 * 1000); // 24 h
DECLARE_TASK(checkInternet, 15 * 1000);         // 15 s
DECLARE_TASK(logBackup, 60 * 60 * 1000);        // 1 h
// Period comes from config.historyPeriodSec; this is only the fallback used
// until tasksSetup() calls setPeriod().
DECLARE_TASK(history, 60 * 1000);               // 1 min
// Runs at 20 s so a schedule cannot be missed inside its minute, and fires at
// most once per minute per schedule.
DECLARE_TASK(schedules, 20 * 1000);             // 20 s
DECLARE_TASK(mqtt, 1 * 60 * 1000);              // 1 min
DECLARE_TASK(talkBack, 1 * 60 * 1000);          // 1 min
#ifdef HAS_MOISTURE_SENSOR
DECLARE_TASK(checkMoisture, 4 * 60 * 60 * 1000); // 4 h
#endif
#ifdef HAS_DHT_SENSOR
// At the DHT11's sampling floor: the Adafruit driver returns its cached
// reading rather than an error when polled faster than once per second.
DECLARE_TASK(dht, 1 * 1000); // 1 s
#endif

// Switching a pump off on time is the one deadline in this firmware that has a
// physical cost when missed, so it does not share the cooperative pump with
// Ping, TalkBack and the MQTT drain — any of which blocks for seconds.
DECLARE_CRITICAL_TASK(relays, CRITICAL_TASKS_PERIOD_MS);

// Critical for a different reason: it is the only indication that the config
// failed to load, and that failure makes tasksSetup() block forever waiting for
// an internet connection the device cannot get. On the cooperative pump the
// error blink would never run in precisely the case it exists to report.
DECLARE_CRITICAL_TASK(ledBlink, 1000);

// Ceiling on each blocking wait in tasksSetup(). Long enough for a normal
// association and NTP round trip, short enough that an outage costs a minute
// rather than the whole session.
static const unsigned long g_bootWaitMaxMs = 60UL * 1000UL;

AccumulatorV2 g_pingTime(g_mqttTaskPeriod / g_checkInternetTaskPeriod);

#ifdef HAS_MOISTURE_SENSOR
static float g_moistureBeforeWatering[MOISTURE_SENSOR_COUNT] = { 0.0 };
#endif

static WiFiClient g_wifiClient;
static TalkBack talkBack;

bool g_hasInternet = false;
time_t g_bootTime = 0;
bool g_mqttEnabled = true;
bool g_ledBlinkEnabled = false;
unsigned g_connectionLossCount = 0;

// Seam with src/relays.cpp: startRelay() calls this once the relay is
// energised, so relay switching itself stays free of the watering bookkeeping.
void
relayStartedHook(unsigned index, unsigned int duration)
{
    if (index == 0) {
        ++g_wateringCycles;
        g_pendingWateringMs = duration;
        mqttAddField(g_wateringField, String(duration));

#ifdef HAS_MOISTURE_SENSOR
        for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
            g_moistureBeforeWatering[i] = g_soilMoisture[i].getAverage();
        }
        g_checkMoistureTask.enableDelayed(g_checkMoistureTaskPeriod);
#endif
    }
}

static void
relaysTaskHandler()
{
    relaysTick();
}

static void
ioTaskHandler()
{
    sensorsReadIo();

    // Same task, same thread as the accumulator writes above — the request
    // handler must never walk these lists itself.
    webUpdateDataCache();
}

#ifdef HAS_DHT_SENSOR
static void
dhtTaskHandler()
{
    sensorsReadDht();
}
#endif

void
mqttTaskHandler()
{
    telemetryPublish();
}

void
clockUpdateTaskHandler()
{
    logger.info("Syncing clock...");

    if (!g_hasInternet) {
        logger.warning("Syncing skipped, no internet connection.");
        return;
    }

    configTime(
      0, 0, "0.br.pool.ntp.org", "1.br.pool.ntp.org", "2.br.pool.ntp.org");

    setenv("TZ", g_timezone.c_str(), 1); // America/Sao Paulo
    tzset();

    // While the clock is still unset, come back in a minute instead of a day —
    // otherwise a boot that missed NTP stays undatable until tomorrow.
    if (time(NULL) < g_safeTimestamp) {
        g_clockUpdateTask.setPeriod(60UL * 1000UL);
    } else {
        g_bootTime = (g_bootTime < g_safeTimestamp) ? time(NULL) : g_bootTime;
        g_clockUpdateTask.setPeriod(g_clockUpdateTaskPeriod);
    }
}

static void
talkBackTaskHandler()
{
    if (!g_mqttEnabled || !g_hasInternet) {
        return;
    }

    String response;

    digitalWrite(LED_BUILTIN, 1);
    if (talkBack.execute(response) == false) {
        logger.error("TalkBack failure.");
        return;
    }
    digitalWrite(LED_BUILTIN, 0);

    // watering:<ms>        -> relay 0, kept for existing TalkBack queues
    // relay:<index>:<ms>   -> any relay
    if (response.indexOf("relay:") != -1) {
        int first = response.indexOf("relay:") + 6;
        int second = response.indexOf(":", first);
        if (second != -1) {
            unsigned index = response.substring(first, second).toInt();
            unsigned time = response.substring(second + 1).toInt();
            logger.info("Executing TalkBack relay task.");
            startRelay(index, time);
        }
    } else if (response.indexOf("watering:") != -1) {
        int index = response.indexOf(":");
        String timeStr = response.substring(index + 1);
        if (timeStr.length() > 0) {
            logger.info("Executing TalkBack watering task.");
            startWatering(timeStr.toInt());
        }
    }
}

static void
checkInternetTaskHandler()
{
    if (!g_wifiConnected || !g_hasNetwork) {
        g_hasInternet = false;
        return;
    }

    static const size_t addresListLen = 3;
    static const IPAddress addressList[addresListLen] = {
        IPAddress(8, 8, 8, 8), IPAddress(8, 8, 4, 4), IPAddress(1, 1, 1, 1)
    };
    static time_t connectionLostTime = 0;

    for (int i = 0; i < addresListLen; ++i) {
        bool success = Ping.ping(addressList[i], 2); // retry at least one time
        if (success) {
            if (!g_hasInternet) {
                logger.info("Internet connection detected!");

                if (connectionLostTime != 0) {
                    time_t downTime = time(NULL) - connectionLostTime;
                    logger.info("Down time: " + String(downTime) + " s");

                    mqttAddStatus("Im back online! Downtime: " +
                                  String(downTime));
                }
            }
            float avgTime = Ping.averageTime();
            if (!isnan(avgTime) && avgTime > 0.0 && avgTime < 1e6) {
                g_pingTime.add(avgTime);
            } else {
                logger.warning("Invalid avgTime " + String(avgTime));
            }
            g_hasInternet = true;
            return;
        }
    }

    if (g_hasInternet) {
        logger.warning("Internet connection lost.");
        g_hasInternet = false;
        connectionLostTime = time(NULL);
        ++g_connectionLossCount;

        mqttAddStatus("Internet connection lost.");
    }
}

#ifdef HAS_MOISTURE_SENSOR
static void
checkMoistureTaskHandler()
{
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
        float moistureDelta =
          g_soilMoisture[i].getAverage() - g_moistureBeforeWatering[i];

        if (moistureDelta < 0.5) {
            logger.warning("Probe " + String(i) +
                           ": no moisture gain after watering. Delta: " +
                           FLOAT_TO_STRING(moistureDelta));
        }
    }

    g_checkMoistureTask.disable();
}
#endif

static void
ledBlinkTaskHandler()
{
    static bool on = false;
    if (g_ledBlinkEnabled) {
        on = !on;
        digitalWrite(LED_BUILTIN, on);
    } else {
        digitalWrite(LED_BUILTIN, 0);
    }
}

// One snapshot of every input and output, written to the ring buffer. Fields
// the board does not have stay NaN, so a reader can tell "not fitted" from
// "read zero".
static void
historyTaskHandler()
{
    // A record stamped before the clock synced is undatable, and it would sit
    // in the ring pretending to be from 1970.
    const time_t now = time(NULL);
    if (now < g_safeTimestamp) {
        return;
    }

    IoRecord record = {};
    record.timestamp = (uint32_t)now;

    // Everything that fired during the period, not just what happens to be on
    // at this instant.
    portENTER_CRITICAL(&g_relayMux);
    uint16_t mask = g_relaySticky;
    g_relaySticky = 0;
    portEXIT_CRITICAL(&g_relayMux);

    for (unsigned i = 0; i < RELAY_COUNT && i < 16; ++i) {
        if (relayIsOn(i)) {
            mask |= (uint16_t)(1u << i);
        }
    }
    record.relayMask = mask;

    for (unsigned i = 0; i < IO_HISTORY_MAX_MOISTURE; ++i) {
        record.moisture[i] = NAN;
    }
    record.luminosity = NAN;
    record.temperature = NAN;
    record.airHumidity = NAN;
    record.waterLevel = NAN;

#ifdef HAS_MOISTURE_SENSOR
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT && i < IO_HISTORY_MAX_MOISTURE;
         ++i) {
        // An empty accumulator averages to 0.0, which would be written as a
        // genuine zero reading and defeat the whole not-fitted-vs-read-zero
        // contract the record layout exists for.
        record.moisture[i] = (g_soilMoisture[i].getSamples() == 0)
                               ? NAN
                               : g_soilMoisture[i].getAverage();
    }
#endif
#ifdef HAS_LUMINOSITY_SENSOR
    record.luminosity = (g_luminosity.getSamples() == 0) ? NAN : g_luminosity.getAverage();
#endif
#ifdef HAS_DHT_SENSOR
    record.temperature = (g_temperature.getSamples() == 0) ? NAN : g_temperature.getAverage();
    record.airHumidity = (g_airHumidity.getSamples() == 0) ? NAN : g_airHumidity.getAverage();
#endif
#ifdef HAS_WATER_LEVEL_SENSOR
    record.waterLevel = (g_waterLevel.getSamples() == 0) ? NAN : g_waterLevel.getAverage();
#endif

    ioHistory.append(record);
}

// Day-of-epoch each schedule last fired on. 0 is 1970-01-01, which no synced
// clock ever reports, so a fresh boot cannot look like "already fired today".
static uint32_t g_scheduleLastFired[SCHEDULE_COUNT] = { 0 };

// How late a schedule may fire and still count. Wide enough to survive a task
// that blocked for a few minutes, narrow enough that a device booting at noon
// does not immediately run the 06:30 watering it slept through.
static const int g_scheduleCatchUpMinutes = 10;

static void
schedulesTaskHandler()
{
    if (config.scheduleCount == 0) {
        return;
    }

    // Local time, and only once NTP has answered: firing on a 1970 clock would
    // water at an arbitrary moment and then never again.
    const time_t now = time(NULL);
    if (now < g_safeTimestamp) {
        return;
    }

    struct tm local;
    if (!localtime_r(&now, &local)) {
        return;
    }

    const uint32_t minuteOfEpoch = (uint32_t)(now / 60);
    const int minuteOfDay = local.tm_hour * 60 + local.tm_min;

    for (unsigned i = 0; i < config.scheduleCount; ++i) {
        const Schedule& sch = config.schedules[i];
        if (!sch.enabled) {
            continue;
        }
        if ((sch.days & (uint8_t)(1u << local.tm_wday)) == 0) {
            continue;
        }

        // A catch-up window, not an exact-minute match. This is a BACKGROUND
        // task, and execute() runs at most one of those per loop(): a single
        // ping round (three 2 s timeouts), a TalkBack socket (5 s) or an MQTT
        // drain over TLS can easily push all three ticks of a 20 s task past
        // the target minute. On an exact match that misses the watering for
        // the whole day, silently and indistinguishably from a schedule that
        // is switched off.
        const int lateBy = minuteOfDay - (sch.hour * 60 + sch.minute);
        if (lateBy < 0 || lateBy > g_scheduleCatchUpMinutes) {
            continue;
        }

        // Fire once per calendar day per schedule. Keyed on the day rather
        // than the minute, because with a catch-up window several ticks now
        // qualify and a minute key would let every one of them fire.
        const uint32_t dayOfEpoch = minuteOfEpoch / (24 * 60);
        if (g_scheduleLastFired[i] == dayOfEpoch) {
            continue;
        }

        if (lateBy > 0) {
            logger.warning("Schedule '" + sch.name + "' is " + String(lateBy) +
                           " min late; firing anyway.");
        }

        g_scheduleLastFired[i] = dayOfEpoch;
        logger.info("Schedule '" + sch.name + "' firing " +
                    config.relayName[sch.relay] + " for " +
                    String(sch.durationMs) + " ms");
        // startRelay() applies the same ceiling and the already-running guard
        // as a manual activation; a schedule gets no privileges.
        startRelay(sch.relay, sch.durationMs);
    }
}

static void
logBackupTaskHandler()
{
    logger.backup();
}

void
tasksSetup()
{
    logger.info("Tasks setup...");

    g_taskScheduler.addTask(&g_ioTask);
    g_taskScheduler.addTask(&g_relaysTask);
    g_taskScheduler.addTask(&g_ledBlinkTask);
    g_taskScheduler.addTask(&g_clockUpdateTask);
    g_taskScheduler.addTask(&g_checkInternetTask);
    g_taskScheduler.addTask(&g_logBackupTask);
    g_taskScheduler.addTask(&g_historyTask);
    g_taskScheduler.addTask(&g_schedulesTask);
    g_taskScheduler.addTask(&g_mqttTask);
    g_taskScheduler.addTask(&g_talkBackTask);
#ifdef HAS_MOISTURE_SENSOR
    g_taskScheduler.addTask(&g_checkMoistureTask);
#endif
#ifdef HAS_DHT_SENSOR
    g_taskScheduler.addTask(&g_dhtTask);
#endif

    sensorsSetup();

    relaysSetup();

    talkBack.setTalkBackID(g_talkBackID);
    talkBack.setAPIKey(g_talkBackAPIKey);
    talkBack.begin(g_wifiClient);

    // Relay timing must be live before the blocking waits below: they can hold
    // setup() for minutes, and a relay commanded in that window still has to
    // switch off on schedule.
    g_relaysTask.enable();
    g_ledBlinkTask.enable();
    if (!g_criticalRunner.start()) {
        logger.fatal("Failed to start the critical task runner.");
    }

    // Both waits are BOUNDED. They used to be unbounded, and that turned any
    // outage into a dead device: with WiFi up and the web server answering, a
    // DNS hiccup on the NTP pool kept setup() spinning "Syncing clock..." for
    // ever, so no sensor was read, no relay timer was armed from the scheduler
    // and no history was written — observed on the live board, stuck for
    // minutes with a perfectly good network.
    //
    // Nothing here actually needs to finish before loop() starts:
    // checkInternet and clockUpdate are periodic tasks that keep retrying.
    logger.info("Waiting for internet connection...");
    unsigned long waitStart = millis();
    while (!g_hasInternet && (millis() - waitStart < g_bootWaitMaxMs)) {
        checkInternetTaskHandler();
        delay(1000);
    }
    if (!g_hasInternet) {
        logger.warning("No internet after " + String(g_bootWaitMaxMs / 1000) +
                       " s. Continuing offline; checkInternet keeps retrying.");
    }

    waitStart = millis();
    while ((g_bootTime < g_safeTimestamp) &&
           (millis() - waitStart < g_bootWaitMaxMs)) {
        clockUpdateTaskHandler();
        delay(2000);
        g_bootTime = time(NULL);
    }
    if (g_bootTime < g_safeTimestamp) {
        logger.warning("Clock not synced. Timestamps stay invalid and history "
                       "is skipped until NTP answers.");
    }

    mqttSetup();
    mqttAddField(g_bootTimeField, String(g_bootTime));

    g_ioTask.enableDelayed(g_ioTaskPeriod);
#ifdef HAS_DHT_SENSOR
    sensorsSetupDht();
    g_dhtTask.enableDelayed(g_dhtTaskPeriod);
#endif
    g_clockUpdateTask.enableDelayed(g_clockUpdateTaskPeriod);
    g_checkInternetTask.enableDelayed(g_checkInternetTaskPeriod);
    g_mqttTask.enableDelayed(g_mqttTaskPeriod);
    g_talkBackTask.enableDelayed(g_talkBackTaskPeriod);
    g_logBackupTask.enableDelayed(g_logBackupTaskPeriod);

    if (config.scheduleCount > 0) {
        g_schedulesTask.enableDelayed(g_schedulesTaskPeriod);
        for (unsigned i = 0; i < config.scheduleCount; ++i) {
            const Schedule& sch = config.schedules[i];
            char when[8];
            snprintf(when, sizeof(when), "%02u:%02u", sch.hour, sch.minute);
            logger.info(String("  schedule '") + sch.name + "' " + when + " -> " +
                        config.relayName[sch.relay] + " " +
                        String(sch.durationMs) + " ms, days 0b" +
                        String(sch.days, BIN));
        }
    }

    // Enabled only when the buffer actually opened: an append into a file that
    // failed to format would log an error every period, forever.
    if (ioHistory.ready()) {
        const unsigned period = (unsigned)config.historyPeriodSec * 1000u;
        g_historyTask.setPeriod(period);
        g_historyTask.enableDelayed(period);
        logger.info("io_history: logging every " +
                    String(config.historyPeriodSec) + " s");
    }

    logger.info("Tasks setup done!");
    logger.backup();
}

static volatile bool g_restartRequested = false;
static volatile unsigned long g_restartDeadline = 0;

void
requestRestart()
{
    g_restartDeadline = millis() + 500;
    g_restartRequested = true;
}

void
tasksLoop()
{
    g_taskScheduler.execute();
    mqttLoop();

    // Gives the queued HTTP response time to leave before the reboot.
    if (g_restartRequested && ((long)(millis() - g_restartDeadline) >= 0)) {
        logger.warning("Restarting on request.");
        ESP.restart();
    }
}

void
mqttEnable(bool enable)
{
    if (enable == true) {
        logger.info("MQTT enabled.");
    } else {
        logger.info("MQTT disabled.");
    }

    g_mqttEnabled = enable;
}
