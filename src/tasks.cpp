#include "core/tasks.h"
#include "BuildConfig.h"
#include "core/cloud_model.h"
#include "core/et0_model.h"
#include "core/io_history.h"
#include "core/moisture_model.h"
#include "network/thingsboard.h"
#include "core/logger.h"
#include "core/relays.h"
#include "core/sensors.h"
#include "core/telemetry.h"
#include "network/custom_login.h"
#include "network/mqtt.h"
#if USE_TALKBACK
#include "network/talkback.h"
#endif
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
#if USE_TALKBACK
DECLARE_TASK(talkBack, 1 * 60 * 1000);          // 1 min
#endif
DECLARE_TASK(checkMoisture, 4 * 60 * 60 * 1000); // 4 h
// Air temperature and humidity, from whichever ambient part this board has —
// a DHT11 on one wire or an SHT40 on I2C. ONE task for both: it reads at most
// one of them, and a task per sensor KIND is exactly how a firmware walks into
// the 16-slot cap that addTask() overruns silently.
//
// 1 s is the DHT11's sampling floor, below which the Adafruit driver returns
// its cached reading rather than an error. The SHT40 would happily go faster
// and gains nothing by it: air does not move that quickly, and each read costs
// ~10 ms of a background tick the cloud model and the relay events share.
DECLARE_TASK(ambient, 1 * 1000); // 1 s

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

static float g_moistureBeforeWatering[MOISTURE_MAX] = { 0.0 };

#if USE_TALKBACK
// Exists only to hand TalkBack a transport. It is the ONLY consumer, so the
// client goes with the flag rather than sitting as an unused WiFiClient.
static WiFiClient g_wifiClient;
static TalkBack talkBack;
#endif

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

// A relay IRRIGATES when some probe names it in `moisture[i].relay`. That is
// the config the classifier already uses to label its training data, so this
// adds no key and no second place to keep in step — and the reservoir pump,
// which no probe names, is excluded for the right reason rather than by index.
//
// It used to be `index != 0`, the single-relay contract from when relay 0 WAS
// the watering relay. On a multi-zone board that silently counted one zone and
// ignored the rest: measured on 6224, a 10 s run of Zona 3 (relay 1) left
// `wateringCycles` at 0 and published no `wateringMs`.
static bool
relayWaters(unsigned index)
{
    for (unsigned i = 0; i < config.moistureCount; ++i) {
        if (config.moistureRelay[i] >= 0 &&
            (unsigned)config.moistureRelay[i] == index) {
            return true;
        }
    }
    return false;
}

static void
publishRelayEvents()
{
    RelayPendingEvent pending[RELAY_MAX];
    relayTakePendingEvents(pending);

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
        // No bare `relay` key. It carried a ZERO-based index while every
        // relayN telemetry key is one-based, so a stored record read
        // {relay: "2", relayName: "Reservatorio", relay3Event: "started"} and
        // anything joining the two was off by one. relayNEvent already names
        // the relay at the correct index, which makes the bare key redundant as
        // well as wrong — and dropping it is the fix that does not create a
        // second meaning for a key already in the stored series, which
        // renumbering it would.
        event["relayName"] = config.relayName[i];
        if (pending[i].started) {
            event["durationMs"] = (int)pending[i].duration;
        }
        tbPublishEvent(JSON.stringify(event));

        // The watering bookkeeping relayStartedHook could not do safely from a
        // request handler. All of it touches state only this task may touch.
        //
        // Derived from pending[] rather than from a single shared slot, which
        // is what it used to be: two zones starting inside one 1 Hz drain
        // overwrote each other and one cycle went uncounted. `wateringMs` is
        // still one value and the last zone in the tick wins it — the per-relay
        // truth is in relayNEvent's own durationMs, published just above.
        if (pending[i].started && relayWaters(i)) {
            ++g_wateringCycles;
            g_pendingWateringMs = pending[i].duration;
#if USE_THINGSPEAK
            mqttAddField(g_wateringField, String(pending[i].duration));
#endif
            g_checkMoistureTask.enableDelayed(g_checkMoistureTaskPeriod);
        }
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

// A cloud transient lasts seconds to minutes and the periodic payload is built
// once every five minutes, so sampling it would miss most of them entirely --
// the rule this tree wrote down after the telemetry spent months sampling
// relays. An episode is therefore an EVENT: one message when it opens and one
// when it closes, carrying the length nothing else could reconstruct.
//
// The STATE is not here. A sky state sits still for tens of minutes and then
// steps, which is the third mechanism -- publish on change with a heartbeat
// under it -- and it leaves through telemetryPublishStepChanges() with the
// relay states and the reservoir contact.
static void
publishCloudEvents(int events)
{
    if ((events & (CLOUD_EVENT_TRANSIENT_BEGAN | CLOUD_EVENT_TRANSIENT_ENDED)) ==
        0) {
        return;
    }

    const CloudReport report = cloudReport();

    JSONVar event;
    event["cloudEvent"] =
      (events & CLOUD_EVENT_TRANSIENT_BEGAN) ? "transient began" : "transient ended";
    event["cloudVariability"] = (double)report.variability;
    if (report.clearness >= 0.0f) {
        event["cloudClearness"] = (double)report.clearness;
    }
    if (events & CLOUD_EVENT_TRANSIENT_ENDED) {
        // Only meaningful at the close: the peak and the length are what an
        // operator reads the episode by, and neither is knowable while it runs.
        event["cloudTransientMin"] = (int)report.transientMinutes;
        event["cloudTransientPeak"] = (double)report.transientPeak;
    }
    tbPublishEvent(JSON.stringify(event));
}

// A day's reference evapotranspiration, published the moment the day closes.
//
// It is an EVENT rather than a step value, and the sampling rule is why: ET0
// updates exactly once every 24 hours, so riding the periodic payload would
// restate the same number 287 times a day, and riding the step publisher would
// re-send it on every 900 s heartbeat for the same nothing. One message a day,
// carrying the whole observation.
//
// The extremes and the coverage go WITH it, in the same message. A daily
// maximum has no averaging in it -- one short excursion owns the day, and this
// board has produced exactly that: a 38-minute patch of direct sun took the
// sensor to 45 C on 2026-09-03 and more than doubled that day's estimate. The
// number alone cannot be re-examined later; the number beside the range it came
// from can. Same reasoning as the `Sd` error bars on the continuous channels.
static void
publishEt0Event()
{
    const Et0Report report = et0Report();

    JSONVar event;
    event["et0"] = (double)report.et0Mm;
    event["et0TempMin"] = (double)report.tMin;
    event["et0TempMax"] = (double)report.tMax;
    event["et0Hours"] = (int)report.hoursCovered;
    tbPublishEvent(JSON.stringify(event));
}

// Seam with src/relays.cpp: startRelay() calls this once the relay is
// energised, so relay switching itself stays free of the watering bookkeeping.
void
relayStartedHook(unsigned index, unsigned int duration)
{
    if (!relayWaters(index)) {
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
    // Only the probes THIS relay feeds. Resetting every probe's baseline was
    // harmless on a one-relay board, where every probe was in the watered zone;
    // on a multi-zone board it threw away the baseline of a zone that was not
    // watered, so its next rise was measured from the wrong starting point.
    for (unsigned i = 0; i < config.moistureCount; ++i) {
        if (config.moistureRelay[i] >= 0 &&
            (unsigned)config.moistureRelay[i] == index) {
            g_moistureBeforeWatering[i] = moistureReading(i).average;
        }
    }
    (void)duration; // the count and the duration are taken from pending[]
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

    // Relay transitions, at 1 Hz rather than at the periodic publish. The
    // critical runner and startRelay() only set bits; this is where they become
    // messages, on a background task where building a String is allowed.
    publishRelayEvents();
    publishFloatEvents();
    publishCloudEvents(cloudModelTick());
    if (et0ModelTick()) {
        publishEt0Event();
    }

    // Every step VALUE that moved, on the same 1 Hz beat and for the same
    // reason: an asynchronous change belongs in asynchronous telemetry, not in
    // a payload built once every five minutes. Both leave through the outbox
    // tbLoop() drains on every loop() iteration, so latency is milliseconds.
    telemetryPublishStepChanges();

#if USE_CUSTOM_LOGIN
    // /sessions.json, when the session purge marked it stale. The purge itself
    // runs on the async_tcp task at the top of EVERY guarded request — a
    // LittleFS write there lands on whichever request arrives first, plausibly
    // the one carrying a 1.2 MB OTA image. Same shape as publishRelayEvents()
    // above: the hot path sets a flag, this tick does the work. Usually a
    // no-op; it fires a handful of times in a device's life.
    customLogin.flushPendingSessionSave();
#endif
}

static void
ambientTaskHandler()
{
    sensorsReadAmbient();
}

void
mqttTaskHandler()
{
    telemetryPublish();
}

// A boot that reaches loop() without a clock retries at this period instead of
// the task's declared 24 h. It has to be applied in THREE places, because the
// scheduler reschedules from an absolute `_nextRunTime`: setPeriod() alone
// changes what happens after the next run and never brings that run closer.
static const unsigned long g_clockRetryPeriod = 60UL * 1000UL;

void
clockUpdateTaskHandler()
{
    logger.info("Syncing clock...");

    if (!g_hasInternet) {
        logger.warning("Syncing skipped, no internet connection.");
        // Measured on 6224, 2026-09-16: this return skipped the setPeriod()
        // below, so a boot whose WiFi took 50 s of the 60 s internet budget
        // left the task on 24 h. checkInternet had the network back seconds
        // later and nothing told the clock — the device sat at 1970 for 17.6 h
        // with no history written, because the history and schedule tasks both
        // refuse an unsynced clock.
        g_clockUpdateTask.setPeriod(g_clockRetryPeriod);
        return;
    }

    configTime(
      0, 0, "0.br.pool.ntp.org", "1.br.pool.ntp.org", "2.br.pool.ntp.org");

    setenv("TZ", g_timezone.c_str(), 1); // America/Sao Paulo
    tzset();

    // While the clock is still unset, come back in a minute instead of a day —
    // otherwise a boot that missed NTP stays undatable until tomorrow.
    if (time(NULL) < g_safeTimestamp) {
        g_clockUpdateTask.setPeriod(g_clockRetryPeriod);
    } else {
        g_bootTime = (g_bootTime < g_safeTimestamp) ? time(NULL) : g_bootTime;
        g_clockUpdateTask.setPeriod(g_clockUpdateTaskPeriod);
    }
}

#if USE_TALKBACK
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
#endif // USE_TALKBACK

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

                // The event half. The retry period covers a boot; this covers
                // an outage at any other time, when the clock task may be
                // parked up to 24 h out. enable() sets _nextRunTime to now,
                // which setPeriod() cannot do.
                if (time(NULL) < g_safeTimestamp) {
                    logger.info("Clock still unset; asking for a sync now.");
                    g_clockUpdateTask.enable();
                }

                if (connectionLostTime != 0) {
                    time_t downTime = time(NULL) - connectionLostTime;
                    logger.info("Down time: " + String(downTime) + " s");

                    // The ThingSpeak channel's `status` string. ThingsBoard
                    // gets the same event as `connectionLoss`, published on
                    // change by telemetryPublishStepChanges().
#if USE_THINGSPEAK
                    mqttAddStatus("Im back online! Downtime: " +
                                  String(downTime));
#endif
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

#if USE_THINGSPEAK
        mqttAddStatus("Internet connection lost.");
#endif
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
    // The REGISTRATION goes with the flag, not just the body. A registered task
    // whose handler returns immediately still takes one of the 16 slots in this
    // bucket, and addTask() past the cap drops silently — so a no-op task is
    // not free, it is a slot charged to a feature that is switched off.
#if USE_TALKBACK
    g_taskScheduler.addTask(&g_talkBackTask);
#endif
    g_taskScheduler.addTask(&g_checkMoistureTask);
    g_taskScheduler.addTask(&g_ambientTask);

    // Before sensorsSetup(), which sizes every accumulator window from it.
    // g_mqttTaskPeriod is only the compiled fallback the task was constructed
    // on; the real period is mqtt.publishSec, exactly as the history task
    // takes its period from config.historyPeriodSec.
    const unsigned mqttPeriod = mqttPublishPeriodMs();
    g_mqttTask.setPeriod(mqttPeriod);

    // The ping accumulator is a file-scope object, so it was sized at static
    // init from the compiled period — before config.json had been read. Same
    // trap as the DHT instance, same fix: re-size it once the real period is
    // known, or its average covers a different interval from every other
    // channel in the payload it shares.
    g_pingTime.setMaxLen(mqttPeriod / g_checkInternetTaskPeriod);

    sensorsSetup();

    cloudModelSetup();

    et0ModelSetup();

    relaysSetup();

#if USE_TALKBACK
    talkBack.setTalkBackID(g_talkBackID);
    talkBack.setAPIKey(g_talkBackAPIKey);
    talkBack.begin(g_wifiClient);
#endif

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
#if USE_THINGSPEAK
    mqttAddField(g_bootTimeField, String(g_bootTime));
#endif

    g_ioTask.enableDelayed(g_ioTaskPeriod);
    sensorsSetupAmbient();
    // Only when one is declared. Ticking at 1 Hz into a handler that returns
    // at its null check wastes a scheduler slot, and the bucket caps at 16.
    // ambientFitted() and not dhtFitted: one question, one answer, asked
    // everywhere the old flag was — the enable that gets forgotten is the one
    // that leaves a fitted sensor silently unread.
    if (config.ambientFitted()) {
        g_ambientTask.enableDelayed(g_ambientTaskPeriod);
    }
    // Arming the 24 h period here regardless of whether NTP answered is what
    // turned a few seconds of slow WiFi into a day without a clock: the FIRST
    // run was a day away, so the short retry period above never got a chance
    // to apply. Arm short when the clock is still unset.
    g_clockUpdateTask.enableDelayed((time(NULL) < g_safeTimestamp)
                                      ? g_clockRetryPeriod
                                      : g_clockUpdateTaskPeriod);
    g_checkInternetTask.enableDelayed(g_checkInternetTaskPeriod);
    g_mqttTask.enableDelayed(mqttPeriod);
#if USE_TALKBACK
    g_talkBackTask.enableDelayed(g_talkBackTaskPeriod);
#endif
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
