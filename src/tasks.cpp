#include "core/tasks.h"
#include "BuildConfig.h"
#include "core/io_history.h"
#include "core/moisture_model.h"
#include "network/thingsboard.h"
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
// Once a day, as asked. The model accumulates across runs rather than being
// refitted, so the period is how fast evidence ages, not how fresh the answer
// is — classification itself happens on every reading.
DECLARE_TASK(moistureModel, 24 * 60 * 60 * 1000); // 24 h
// Period comes from config.historyPeriodSec; this is only the fallback used
// until tasksSetup() calls setPeriod().
DECLARE_TASK(history, 60 * 1000);               // 1 min
// Runs at 20 s so a schedule cannot be missed inside its minute, and fires at
// most once per minute per schedule.
DECLARE_TASK(schedules, 20 * 1000);             // 20 s
DECLARE_TASK(mqtt, 1 * 60 * 1000);              // 1 min
DECLARE_TASK(talkBack, 1 * 60 * 1000);          // 1 min
DECLARE_TASK(checkMoisture, 4 * 60 * 60 * 1000); // 4 h
// At the DHT11's sampling floor: the Adafruit driver returns its cached
// reading rather than an error when polled faster than once per second.
DECLARE_TASK(dht, 1 * 1000); // 1 s

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

// Set by relayStartedHook on any thread, consumed by the io task. A single
// unsigned written by one producer and cleared by one consumer needs no lock:
// the worst interleaving loses a duration, not memory.
static volatile unsigned g_wateringStartedMs = 0;

static float g_moistureBeforeWatering[MOISTURE_MAX] = { 0.0 };

static WiFiClient g_wifiClient;
static TalkBack talkBack;

bool g_hasInternet = false;
time_t g_bootTime = 0;
bool g_mqttEnabled = true;
bool g_ledBlinkEnabled = false;
unsigned g_connectionLossCount = 0;

// Seam with src/relays.cpp: the reservoir interlock. Lives here rather than in
// relays.cpp because it couples a relay to a SENSOR, and relays.cpp is the one
// module that must stay about switching.
bool
relayStartAllowed(unsigned index, String& reason)
{
    // loadFile() clears floatInterlock when no float switch is declared, so a
    // sensor removed in /devices.html cannot leave a veto behind that refuses
    // every watering on a reading nothing produces.
    if (!config.floatInterlock) {
        return true;
    }

    // The refill relay is the remedy for an empty reservoir. Blocking it would
    // mean an empty tank could never be filled — the interlock deadlocking the
    // system it exists to protect.
    if ((int)index == config.floatFillRelay) {
        return true;
    }

    if (!floatRaised()) {
        reason = config.floatName + " reads empty";
        return false;
    }

    return true;
}

// Turns the bits the switching code recorded into telemetry, once a second.
//
// The device used to report relays by SAMPLING them into the 1-minute payload,
// so a five-second watering was invisible: the sample landed between the
// events. The history record solved this with a sticky mask long ago and the
// telemetry never got it. Now there are both — an immediate event for the
// transition, and a sticky mask so the periodic payload says "this ran during
// the period" rather than "it happens to be on right now".
static void
publishRelayEvents()
{
    RelayPendingEvent pending[RELAY_MAX];
    relayTakePendingEvents(pending);

    // The watering bookkeeping relayStartedHook could not do safely from a
    // request handler. All of it touches state only this task may touch.
    if (g_wateringStartedMs > 0) {
        const unsigned duration = g_wateringStartedMs;
        g_wateringStartedMs = 0;
        ++g_wateringCycles;
        g_pendingWateringMs = duration;
        mqttAddField(g_wateringField, String(duration));
        g_checkMoistureTask.enableDelayed(g_checkMoistureTaskPeriod);
    }

    for (unsigned i = 0; i < config.relayCount; ++i) {
        if (pending[i].refused) {
            JSONVar refusal;
            refusal["relayRefused"] = config.relayName[i];
            refusal["relay"] = (int)i;
            refusal["reason"] = pending[i].reason;
            tbPublishEvent(JSON.stringify(refusal));
        }

        if (!pending[i].started && !pending[i].ended) {
            continue;
        }

        JSONVar event;
        const String key = "relay" + String(i + 1) + "Event";
        // Both flags can be set in the same second — a 500 ms activation
        // starts and finishes between two drains. Reporting "ended" then is
        // right: what the operator needs to know is that it ran and is done.
        event[key.c_str()] = pending[i].started
                               ? (pending[i].ended ? "ran" : "started")
                               : "stopped";
        event["relay"] = (int)i;
        event["relayName"] = config.relayName[i];
        if (pending[i].started) {
            event["durationMs"] = (int)pending[i].duration;
        }
        tbPublishEvent(JSON.stringify(event));
    }
}

// The float switch has exactly the relay's problem and a worse consequence: a
// reservoir that runs empty and is refilled between two publishes never
// happened, as far as the cloud is concerned — and "the tank ran dry" is the
// one event an operator most needs to see. So the TRANSITION is published, not
// the level.
static void
publishFloatEvents()
{
    if (!config.floatFitted) {
        return;
    }

    static bool known = false;
    static bool lastRaised = false;

    const bool raised = floatRaised();
    if (known && raised == lastRaised) {
        return;
    }

    // The first reading after boot is reported too: an operator coming back to
    // a device needs to know the current state, not only the next change.
    JSONVar event;
    event["reservoirRaised"] = raised;
    event["reservoirEvent"] = known ? (raised ? "refilled" : "emptied")
                                    : "initial reading";
    tbPublishEvent(JSON.stringify(event));

    known = true;
    lastRaised = raised;
}

// Seam with src/relays.cpp: startRelay() calls this once the relay is
// energised, so relay switching itself stays free of the watering bookkeeping.
void
relayStartedHook(unsigned index, unsigned int duration)
{
    if (index != 0) {
        return;
    }

    // This runs on WHICHEVER THREAD asked for the relay — async_tcp for
    // /control, loop() for TalkBack and schedules. So it does exactly one
    // thing that is safe from all of them, and everything else waits for the
    // io task in publishRelayEvents().
    //
    // What used to be here and could not stay:
    //   - g_soilMoisture[i].getAverage(), which walks a list the io task is
    //     writing and updates a shared member. The documented trap, again.
    //   - mqttAddField(), which appends to the global String telemetryPublish()
    //     concurrently reads and clears — an unsynchronised reallocation.
    //   - g_checkMoistureTask.enableDelayed(), which mutates the scheduler's
    //     task list while execute() walks it; the library documents that as
    //     unsafe after start().
    //
    // moistureReading() IS safe from any thread — it is the snapshot the io
    // task publishes under a spinlock — so the pre-watering baseline is taken
    // here rather than deferred, where it would be a second late and a second
    // of watering wrong.
    for (unsigned i = 0; i < config.moistureCount; ++i) {
        g_moistureBeforeWatering[i] = moistureReading(i).average;
    }
    g_wateringStartedMs = duration;
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

    // Relay transitions, at 1 Hz rather than at the 1-minute publish. The
    // critical runner and startRelay() only set bits; this is where they become
    // messages, on a background task where building a String is allowed.
    publishRelayEvents();
    publishFloatEvents();
}

static void
dhtTaskHandler()
{
    sensorsReadDht();
}

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

static void
checkMoistureTaskHandler()
{
    for (unsigned i = 0; i < config.moistureCount; ++i) {
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
    // Ten minutes of appends without a panic clears the boot-loop interlock.
    // Long enough that a writer which crashes on its first append — the failure
    // this guards against — never reaches it, and short enough that a real
    // reboot for any other reason does not leave a strike behind to be counted
    // against an unrelated future one.
    if (millis() > 10UL * 60UL * 1000UL) {
        historyGuardClear();
    }

    // A record stamped before the clock synced is undatable, and it would sit
    // in the history pretending to be from 1970.
    const time_t now = time(NULL);
    if (now < g_safeTimestamp) {
        return;
    }

    IoRecord record = {};
    record.timestamp = (uint32_t)now;

    // Everything that fired during the period, not just what happens to be on
    // at this instant.
    uint16_t mask = relayStickyTake(RELAY_STICKY_HISTORY);

    for (unsigned i = 0; i < config.relayCount && i < 16; ++i) {
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
    record.flowRate = NAN;
    record.flowTotal = NAN;

    for (unsigned i = 0; i < config.moistureCount &&
         i < IO_HISTORY_MAX_MOISTURE;
         ++i) {
        // An empty accumulator averages to 0.0, which would be written as a
        // genuine zero reading and defeat the whole not-fitted-vs-read-zero
        // contract the record layout exists for.
        record.moisture[i] = (g_soilMoisture[i].getSamples() == 0)
                               ? NAN
                               : g_soilMoisture[i].getAverage();
    }
    record.luminosity = (g_luminosity.getSamples() == 0) ? NAN : g_luminosity.getAverage();
    record.temperature = (g_temperature.getSamples() == 0) ? NAN : g_temperature.getAverage();
    record.airHumidity = (g_airHumidity.getSamples() == 0) ? NAN : g_airHumidity.getAverage();
    record.waterLevel = (g_waterLevel.getSamples() == 0) ? NAN : g_waterLevel.getAverage();
    record.flowRate = (g_flowRate.getSamples() == 0) ? NAN : g_flowRate.getAverage();
    // The running total is the point of storing flow at all: it answers how
    // much a watering actually delivered, and it only lives in RAM otherwise —
    // gone at every reboot, brownout and OTA.
    // Guarded, unlike the accumulators above: a running total has no
    // getSamples() to say "never fed", so an unfitted meter would write a
    // perfectly real 0.0 litres and the chart would show a flat line instead
    // of a gap.
    if (config.flowFitted) {
        record.flowTotal = (float)flowTotalLitres();
    }

    // Same reason, and it is the exact case IO_HISTORY_FLAG_FLOAT_VALID was
    // added for: without the guard a board with no float switch records a
    // valid reading of "lowered", indistinguishable from a genuinely empty
    // reservoir, forever.
    if (config.floatFitted) {
        record.flags |= IO_HISTORY_FLAG_FLOAT_VALID;
        if (floatRaised()) {
            record.flags |= IO_HISTORY_FLAG_FLOAT_RAISED;
        }
    }

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

// Three passes over the whole history buffer — seconds of SPIFFS reads, which
// on the cooperative pump stalls every other BACKGROUND task for that long.
// Acceptable once a day, and the reason relay timing is critical rather than
// background: a pump switching off does not wait for this.
static void
moistureModelTaskHandler()
{
    moistureModelTrain();
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
    g_taskScheduler.addTask(&g_moistureModelTask);
    g_taskScheduler.addTask(&g_historyTask);
    g_taskScheduler.addTask(&g_schedulesTask);
    g_taskScheduler.addTask(&g_mqttTask);
    g_taskScheduler.addTask(&g_talkBackTask);
    g_taskScheduler.addTask(&g_checkMoistureTask);
    g_taskScheduler.addTask(&g_dhtTask);

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
    sensorsSetupDht();
    // Only when one is declared. Ticking at 1 Hz into a handler that returns
    // at its null check wastes a scheduler slot, and the bucket caps at 16.
    if (config.dhtFitted) {
        g_dhtTask.enableDelayed(g_dhtTaskPeriod);
    }
    g_clockUpdateTask.enableDelayed(g_clockUpdateTaskPeriod);
    g_checkInternetTask.enableDelayed(g_checkInternetTaskPeriod);
    g_mqttTask.enableDelayed(g_mqttTaskPeriod);
    g_talkBackTask.enableDelayed(g_talkBackTaskPeriod);
    g_logBackupTask.enableDelayed(g_logBackupTaskPeriod);
    // Trains 5 minutes after boot as well as daily: a device that is power
    // cycled every evening would otherwise never reach its 24 h tick, and the
    // history it just reloaded is exactly the evidence it needs.
    if (config.moistureCount > 0) {
        g_moistureModelTask.enableDelayed(5 * 60 * 1000);
    }

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
