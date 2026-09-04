#include "core/telemetry.h"
#include "BuildConfig.h"
#include "core/cloud_model.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/probe_health.h"
#include "core/relays.h"
#include "core/sensors.h"
#include "core/step_publisher.h"
#include "core/tasks.h"
#include "network/mqtt.h"
#include "network/thingsboard.h"
#include <Arduino_JSON.h>
#include <list>
#include <math.h>

// Second soil probe on ThingSpeak: config.thingSpeakMoisture2Field, 0 to keep
// it off. This is configuration and not a build flag because the answer is
// per-device -- field 4 is free on a board without a water level sensor and
// taken on a board with one -- and because reusing a number rewrites the
// meaning of everything already stored under it, which is a decision for
// whoever owns the channel, not for whoever runs the compiler.
//
// Probes 3 and beyond have no ThingSpeak field at all. Eight fields is the
// ceiling, and it is the reason this firmware also speaks ThingsBoard.

#if USE_THINGSPEAK
static String g_mqttMessage = "";
#endif

static const char* const g_thingsBoardTelemetryTopic = "v1/devices/me/telemetry";

unsigned g_packagesSent = 0;
uint32_t g_lastPublishTime = 0;

unsigned
mqttPublishPeriodMs()
{
    return (unsigned)config.mqttPublishSec * 1000u;
}

#if USE_THINGSPEAK
void
mqttAddField(int field, String val)
{
    g_mqttMessage += "field" + String(field) + "=" + val + "&";
}

void
mqttAddStatus(String status)
{
    g_mqttMessage += "status='" + status + "'&";
}
#endif

// ---------------------------------------------------------------------------
// Step values -- published on CHANGE, never sampled
// ---------------------------------------------------------------------------

// The comparison itself lives in core/step_publisher.h, deliberately free of
// Arduino so test_step_publisher can reach it: the deadband and the millis()
// rollover are exactly the kind of arithmetic that returns plausible answers
// when it is wrong instead of failing.
//
// Slots, not a map. The key set is fixed -- RELAY_MAX pairs plus a handful of
// singletons -- so an array indexed by an enum costs nothing and allocates
// nothing, which is the rule everything on the 1 Hz io task is held to.
enum StepSlot
{
    STEP_RELAY = 0,
    STEP_RELAY_RAN = STEP_RELAY + RELAY_MAX,
    STEP_RELAY_MASK = STEP_RELAY_RAN + RELAY_MAX,
    STEP_RELAY_RAN_MASK,
    STEP_CONNECTION_LOSS,
    STEP_WATERING_CYCLES,
    STEP_DHT_ERROR_RATE,
    STEP_FLOW_RATE,
    STEP_FLOW_TOTAL,
    STEP_RESERVOIR_RAISED,
    STEP_CLOUD_STATE,
    STEP_COUNT
};

static StepValue g_step[STEP_COUNT] = {};

// A float compared with != differs in its last bits for ever, so every float
// key would publish on every tick and the whole mechanism would buy nothing.
// Each number below is the smallest CHANGE worth a datapoint on that channel,
// not a numerical epsilon -- the question is "would an operator act on this
// difference", not "are these bit-identical".
//
// A counter or a contact takes 0: those values are whole numbers a double
// represents exactly, so the comparison is genuinely exact.
static const double g_stepExact = 0.0;
// Litres per minute. The payload has always rendered two decimals, so anything
// below this is a change nobody could read off a chart.
static const double g_flowRateDeadband = 0.05;
// Litres. A cumulative total only moves when water actually passes, and 5 ml is
// well under a single pulse of a 450 pulses/litre meter.
static const double g_flowTotalDeadband = 0.005;
// Percentage points. errors/reads drifts a little on EVERY read once any error
// exists, so an exact comparison here would publish at 1 Hz for the rest of the
// device's uptime. Half a point is the resolution the number is acted on at.
static const double g_dhtErrorRateDeadband = 0.5;

// Slot-addressed wrapper over stepValueDue(), so the call sites below read as
// one line per key.
static bool
stepDue(unsigned slot,
        double value,
        double deadband,
        uint32_t nowMs,
        uint32_t heartbeatMs)
{
    return stepValueDue(g_step[slot], value, deadband, nowMs, heartbeatMs);
}

void
telemetryPublishStepChanges()
{
    // tbPublishEvent() is a no-op on ThingSpeak, and taking the sticky mask
    // below would then DISCARD activations the ThingSpeak path never reads --
    // exactly what a per-consumer take-and-clear mask exists to prevent.
    if (!mqttIsThingsBoard()) {
        return;
    }

    // The operator turned publishing off. Nothing is built and nothing is
    // recorded as published, so the first tick after it comes back on re-sends
    // every key rather than leaving the cloud on values from before the gap.
    if (!g_mqttEnabled) {
        return;
    }

    const uint32_t now = millis();
    const uint32_t heartbeat = (uint32_t)config.mqttHeartbeatSec * 1000u;

    JSONVar telemetry;
    unsigned keys = 0;

    // relayN is what the relay is doing RIGHT NOW; relayNRan says whether it
    // ran at any point since this function last looked. Both are needed and
    // they answer different questions: the second is the only one that can see
    // an activation shorter than the gap between two looks, which at 1 Hz means
    // a relay that started and stopped inside one second.
    const uint16_t fired = relayStickyTake(RELAY_STICKY_TELEMETRY);
    uint16_t mask = 0;
    for (unsigned i = 0; i < config.relayCount && i < 16; ++i) {
        const bool on = relayIsOn(i);
        const bool ran = (fired & (uint16_t)(1u << i)) != 0;

        // The key Strings are built INSIDE the branches, not once above them.
        // This runs at 1 Hz for the life of the device and almost every call
        // publishes nothing, so a String per relay per second would be pure
        // heap churn in the steady state -- the rule AccumulatorV2 was
        // rewritten for, and the shape publishRelayEvents() already uses.
        if (stepDue(
              STEP_RELAY + i, on ? 1.0 : 0.0, g_stepExact, now, heartbeat)) {
            const String key = "relay" + String(i + 1);
            telemetry[key.c_str()] = on;
            ++keys;
        }
        if (stepDue(STEP_RELAY_RAN + i,
                    ran ? 1.0 : 0.0,
                    g_stepExact,
                    now,
                    heartbeat)) {
            const String ranKey = "relay" + String(i + 1) + "Ran";
            telemetry[ranKey.c_str()] = ran;
            ++keys;
        }

        if (on) {
            mask |= (uint16_t)(1u << i);
        }
    }

    if (stepDue(STEP_RELAY_MASK, (double)mask, g_stepExact, now, heartbeat)) {
        telemetry["relayMask"] = (int)mask;
        ++keys;
    }
    if (stepDue(
          STEP_RELAY_RAN_MASK, (double)fired, g_stepExact, now, heartbeat)) {
        telemetry["relayRanMask"] = (int)fired;
        ++keys;
    }

    if (stepDue(STEP_CONNECTION_LOSS,
                (double)g_connectionLossCount,
                g_stepExact,
                now,
                heartbeat)) {
        telemetry["connectionLoss"] = (int)g_connectionLossCount;
        ++keys;
    }
    if (stepDue(STEP_WATERING_CYCLES,
                (double)g_wateringCycles,
                g_stepExact,
                now,
                heartbeat)) {
        telemetry["wateringCycles"] = (int)g_wateringCycles;
        ++keys;
    }

    // The sky state: it sits still for tens of minutes and then steps, which is
    // exactly what publish-on-change is for. Two rules it does not share with
    // the other step keys:
    //
    //  - It is only published while the model HAS an answer. Outside the fitted
    //    daylight window there is no clear-sky reference and therefore no
    //    state, and a heartbeat re-stating "no reference" every fifteen minutes
    //    all night is the waste this whole mechanism exists to remove.
    //  - The transition back is still said once, the same exception
    //    addProbeFault() makes: without it the cloud shows yesterday's sky for
    //    ever, and an operator reading "latest" at midnight is told it is clear.
    {
        const CloudReport cloud = cloudReport();
        const bool known = cloud.state != CLOUD_UNKNOWN;
        const bool wasKnown = g_step[STEP_CLOUD_STATE].valid &&
                              g_step[STEP_CLOUD_STATE].value !=
                                (double)CLOUD_UNKNOWN;
        if ((known || wasKnown) && stepDue(STEP_CLOUD_STATE,
                                           (double)cloud.state,
                                           g_stepExact,
                                           now,
                                           heartbeat)) {
            telemetry["cloudState"] =
              known ? cloudStateName(cloud.state) : "no reference";
            ++keys;
        }
    }

    if (g_dhtTotalReads > 0) {
        const double rate =
          (double)g_dhtReadErrors * 100.0 / (double)g_dhtTotalReads;
        if (stepDue(STEP_DHT_ERROR_RATE,
                    rate,
                    g_dhtErrorRateDeadband,
                    now,
                    heartbeat)) {
            telemetry["dhtErrorRate"] = rate;
            ++keys;
        }
    }

    // The cumulative total only exists in RAM and resets on every reboot, so
    // without publishing it the questions a flow meter is installed to answer --
    // how much did last night's watering actually deliver, is the line dripping
    // while the relay is off -- cannot be asked afterwards. ThingsBoard has no
    // field limit, which is what makes this free here and impossible on the
    // ThingSpeak side.
    if (config.flowFitted) {
        if (g_flowRate.getSamples() > 0) {
            const double rate = (double)g_flowRate.getAverage();
            if (stepDue(
                  STEP_FLOW_RATE, rate, g_flowRateDeadband, now, heartbeat)) {
                telemetry["flowRate"] = rate;
                ++keys;
            }
        }
        // Guarded, unlike the rate: a running total has no getSamples() to say
        // "never fed", so an unfitted meter would publish a perfectly real
        // 0 litres that no dashboard can tell from a line that delivered
        // nothing. Same reason the history record guards it.
        const double total = flowTotalLitres();
        if (stepDue(
              STEP_FLOW_TOTAL, total, g_flowTotalDeadband, now, heartbeat)) {
            telemetry["flowTotalLitres"] = total;
            ++keys;
        }
    }

    if (config.floatFitted) {
        // Without the guard every board publishes reservoirRaised:false, which
        // is indistinguishable from a genuinely empty tank -- the exact case
        // IO_HISTORY_FLAG_FLOAT_VALID exists for.
        const bool raised = floatRaised();
        if (stepDue(STEP_RESERVOIR_RAISED,
                    raised ? 1.0 : 0.0,
                    g_stepExact,
                    now,
                    heartbeat)) {
            telemetry["reservoirRaised"] = raised;
            ++keys;
        }
    }

    if (keys == 0) {
        return;
    }

    tbPublishEvent(JSON.stringify(telemetry));
}

// ---------------------------------------------------------------------------
// The periodic payload -- continuous channels only
// ---------------------------------------------------------------------------

// Suffix for the instantaneous twin of a continuous channel.
//
// The AVERAGE keeps the ORIGINAL key name, and that is not an arbitrary choice.
// Every one of `moisture1`, `temperature`, `airHumidity`, `luminosity`,
// `waterLevel` and `ping` has carried getAverage() since the ThingsBoard
// payload existed, and there are already days of history stored under those
// names. Redefining one would leave a single series whose meaning changes
// part-way through with nothing in the data to say where -- the same defect as
// renumbering a relay index, and worse, because a number that merely shifts is
// harder to notice than one that is plainly wrong.
static const char* const g_instantSuffix = "Now";

// The spread of the window, as an ERROR BAR. Standard deviation and not the
// variance the accumulator stores, because an error bar is drawn in the
// measurement's own units and units-squared does not overlay a chart.
//
// It answers a question the archive could not answer before: "had this reading
// settled?" A mean of 74.0 at sd 0.05 and a mean of 74.0 at sd 30 are different
// claims, and until now only the first number was kept. Reconstructing that
// after the fact meant polling the live device, which is exactly what could not
// be done for anything already past.
static const char* const g_spreadSuffix = "Sd";

// Both numbers, deliberately. The window mean is what a trend is read from; the
// instantaneous value is the only one that can show a spike, and at a 300 s
// publish period a window is 300 samples deep -- easily enough to flatten a
// watering into nothing.
static void
addContinuous(JSONVar& telemetry, const String& key, AccumulatorV2& acc)
{
    if (acc.getSamples() == 0) {
        return; // never sampled: sending 0 would read as a real value
    }
    // getAverage() is load-bearing rather than incidental: it is what refreshes
    // `variance`, so the spread below is only meaningful after this call.
    telemetry[key.c_str()] = (double)acc.getAverage();
    const String instantKey = key + g_instantSuffix;
    telemetry[instantKey.c_str()] = (double)acc.getLast();
    // Clamped at zero: the incremental sums can land a hair below it on a
    // window of identical samples, and sqrt of a negative is a NaN that
    // serialises as null and reads as a missing sensor.
    const String spreadKey = key + g_spreadSuffix;
    telemetry[spreadKey.c_str()] = (double)sqrtf(fmaxf(acc.variance, 0.0f));
}

// The verdict last published per probe, so a fault that CLEARS is said once.
// PROBE_UNKNOWN is 0, which is what a probe that has never been judged should
// start at, so the zero initialiser is the correct one.
static int g_lastFaultVerdict[MOISTURE_MAX] = {};

// Published ONLY when there is a fault, exactly as /data.json omits the field
// and as `state` is omitted rather than sent empty. A key that says "fine" on
// every healthy probe every period is the waste this whole change is about, and
// it trains the eye to skip the one case that matters.
//
// The exception is the transition back: without one publish of the cleared
// verdict the cloud keeps showing the last fault for ever, and an operator
// reading "latest" would still be chasing a probe that was fixed a week ago.
// That single datapoint is worth what it costs.
static void
addProbeFault(JSONVar& telemetry, unsigned index)
{
    const ProbeHealthReport health = probeHealthReport(index);
    const bool faulty =
      health.verdict != PROBE_UNKNOWN && health.verdict != PROBE_CONNECTED;
    const int last = g_lastFaultVerdict[index];
    const bool cleared =
      !faulty && last != PROBE_UNKNOWN && last != PROBE_CONNECTED;

    if (!faulty && !cleared) {
        return;
    }

    // probeVerdictName() has no string for PROBE_UNKNOWN, and an empty value in
    // a timeseries reads as a bug rather than as "the evidence went away".
    const char* const name = probeVerdictName(health.verdict);
    const String verdict = (name[0] != '\0') ? String(name) : String("unknown");

    const String key = "moisture" + String(index + 1) + "Fault";
    telemetry[key.c_str()] = verdict;
    g_lastFaultVerdict[index] = health.verdict;
}

// ThingsBoard telemetry: one JSON object, arbitrary keys. This is the reason
// to support it at all — the ThingSpeak channel's eight fields are spoken for,
// which is what keeps probes 2 and 3 off the cloud entirely.
//
// Written through chained subscripts on a named JSONVar, never by returning one
// by value: that library's move-assign for rvalues yields null children.
//
// STEP VALUES ARE NOT HERE. Relay state, the sticky masks, the counters, the
// flow totals and the reservoir contact all leave through
// telemetryPublishStepChanges() the moment they move. What is left is the set
// of channels that genuinely has something new to say on every tick.
static String
buildThingsBoardPayload(float cloudVariability)
{
    JSONVar telemetry;

    for (unsigned i = 0; i < config.moistureCount; ++i) {
        const String key = "moisture" + String(i + 1);

        // Before the sample guard: a probe whose accumulator was never fed
        // still has a health verdict worth reporting, and a fault that clears
        // must be said even if the reading behind it went away.
        addProbeFault(telemetry, i);

        if (g_soilMoisture[i].getSamples() == 0) {
            continue; // never sampled: sending 0 would read as a real value
        }
        addContinuous(telemetry, key, g_soilMoisture[i]);

        const String state = moistureState(i);
        if (state.length() > 0) {
            const String stateKey = key + "State";
            telemetry[stateKey.c_str()] = state;
        }
    }

    addContinuous(telemetry, "luminosity", g_luminosity);

    // The one cloud quantity that is a LEVEL, so the one that rides this tick.
    //
    // At a 300 s period the stored `luminosity` mean keeps almost nothing about
    // what happened INSIDE the window: measured on the 60 s archive resampled
    // to 300 s, the difference between consecutive published means recovers
    // 5.1 % of the windows whose clearness index moved by 0.10 or more, at a
    // 1 % false-positive rate. The within-window spread recovers 80 %. Without
    // this key the archive cannot answer "was the sky flickering" at all.
    //
    // It is the mean |dk| per minute and NOT the accumulator's variance,
    // because the variance is of the LEVEL and the level ramps all morning:
    // over five-minute windows with a range of five points or more, a smooth
    // ramp and a flickering sky both sit at a level sd of 3.2, while the
    // normalised step statistic separates them 1.1 against 3.6. probe_health.h
    // records making the same mistake once, on the same kind of signal.
    //
    // Taken by the caller, not here: cloudTakeVariability() is take-and-clear
    // and must have exactly ONE caller for the reason relayStickyTake() does.
    // telemetryPublish() drains it on every tick, including the ticks this
    // function is never reached on, so the mean always covers one publish
    // period rather than however long the backend happened to be ThingSpeak or
    // publishing happened to be switched off.
    if (cloudVariability >= 0.0f) {
        telemetry["cloudVariability"] = (double)cloudVariability;
    }
    addContinuous(telemetry, "temperature", g_temperature);
    addContinuous(telemetry, "airHumidity", g_airHumidity);
    addContinuous(telemetry, "waterLevel", g_waterLevel);

    if (g_pendingWateringMs > 0) {
        telemetry["wateringMs"] = (int)g_pendingWateringMs;
        g_pendingWateringMs = 0;
    }

    // Guarded like every other accumulator. It was the one exception, and on a
    // device that has not completed a ping it published 0.0 — which reads as a
    // zero-millisecond round trip, not as "no measurement". Same reason the
    // moisture and DHT fields are skipped when unsampled.
    addContinuous(telemetry, "ping", g_pingTime);

    // `firmware` is deliberately NOT here any more.
    //
    // It is a string that changes at a reboot and at no other moment, so a copy
    // in every periodic payload was ~1400 datapoints a day restating a constant.
    // Dropping it loses nothing: tbOnConnect() publishes `current_fw_version`
    // as a client attribute — the key ThingsBoard itself reads to decide an
    // update landed — AND a `firmware` TELEMETRY point inside the boot event,
    // once per connection. So the timeseries still carries a `firmware` value
    // stamped at every moment the version could possibly have changed, under
    // the same key it always used.

    return JSON.stringify(telemetry);
}

void
telemetryPublish()
{
    static std::list<String> msgQueue;

    // Drained on EVERY tick, above every early return. See
    // buildThingsBoardPayload().
    const float cloudVariability = cloudTakeVariability();

    // The payload is BUILT AND QUEUED before the connectivity check, not after.
    //
    // The early return used to sit above both push_back calls, so a thirty
    // minute outage produced zero queued payloads and every reading in it was
    // lost — the queue only ever held messages whose publish failed while
    // g_hasInternet was still true, a window checkInternet closes within 15 s.
    // A queue that empties itself the moment connectivity drops is not a queue
    // for surviving an outage, which is the only reason this one exists.
    //
    // g_mqttEnabled is different: it means the operator turned publishing off,
    // and honouring that by filling RAM with an hour of undelivered payloads
    // would be the wrong reading of the instruction.
    const bool offline = !g_hasInternet;
    if (!g_mqttEnabled) {
        logger.info("MQTT disabled by configuration; nothing built or queued.");
        return;
    }

    // Above the queue, not below it. A payload this build has no way to deliver
    // is not worth an hour of RAM waiting for a link that will never be
    // attempted — and unlike the `offline` case there is nothing to wait for:
    // no amount of connectivity makes a backend appear in an image that was not
    // built with it. The refusal is stated once at boot and stands permanently
    // in /data.json; see mqttBackendSupported().
    if (!mqttBackendSupported()) {
        return;
    }

    if (mqttIsThingsBoard()) {
        msgQueue.push_back(buildThingsBoardPayload(cloudVariability));
#if USE_THINGSPEAK
        g_mqttMessage = ""; // the ThingSpeak accumulator is unused here
#endif
    }
#if USE_THINGSPEAK
    else {

        // A field is only sent when the sensor behind it is fitted. Sending a
        // never-fed accumulator would publish 0.00 as though it were a
        // reading, and on ThingSpeak an absent field and a zero are stored very
        // differently: the first leaves a gap, the second becomes history.
        if (config.moistureCount > 0) {
            mqttAddField(g_soilMoistureField,
                         FLOAT_TO_STRING(g_soilMoisture[0].getAverage()));
        }
        if (config.moistureCount > 1 && config.thingSpeakMoisture2Field > 0) {
            mqttAddField(config.thingSpeakMoisture2Field,
                         FLOAT_TO_STRING(g_soilMoisture[1].getAverage()));
        }

        if (config.luminosityFitted) {
            mqttAddField(g_luminosityField,
                         FLOAT_TO_STRING(g_luminosity.getAverage()));
        }

        if (config.dhtFitted) {
            mqttAddField(g_temperatureField,
                         FLOAT_TO_STRING(g_temperature.getAverage()));
            mqttAddField(g_airHumidityField,
                         FLOAT_TO_STRING(g_airHumidity.getAverage()));
        }

        if (config.waterLevelFitted) {
            mqttAddField(g_waterLevelField,
                         FLOAT_TO_STRING(g_waterLevel.getAverage()));
        }

        mqttAddField(g_pingField, String(g_pingTime.getAverage()));

        char timestamp[64];
        time_t now = time(nullptr);
        strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
        g_mqttMessage += "created_at='" + String(timestamp) + "'";

        msgQueue.push_back(g_mqttMessage);
        g_mqttMessage = "";
    }
#endif

    // An hour of buffer at whatever period is configured, so lengthening the
    // publish interval shortens the queue instead of stretching what it covers.
    // mqtt.publishSec is clamped to 60..300 s at load, so this is 60 messages
    // at the floor and 12 at the ceiling and can never reach zero.
    const unsigned maxMsgQueueSize = 60 * 60 * 1000 / mqttPublishPeriodMs();

    if (offline) {
        // Queued above, sent when the link returns. The drain below would only
        // pile up failed publishes and log noise.
        logger.info("MQTT: offline, payload queued (" +
                    String(msgQueue.size()) + " waiting)");
        while (msgQueue.size() > maxMsgQueueSize) {
            msgQueue.pop_front();
        }
        return;
    }

    digitalWrite(LED_BUILTIN, 1);
    int errors = 0;
    while (msgQueue.size() > maxMsgQueueSize) {
        logger.warning("msgQueue is full, discarding messages..");
        msgQueue.pop_front();
    }
    while (msgQueue.size() > 0) {
        // With USE_THINGSPEAK off, mqttBackendSupported() above has already
        // guaranteed this is the ThingsBoard backend, so there is no second
        // arm to choose and g_thingSpeakChannelNumber is never read.
        const bool success =
#if USE_THINGSPEAK
          mqttIsThingsBoard()
            ? mqttPublishTopic(g_thingsBoardTelemetryTopic, msgQueue.front())
            : mqttPublish(g_thingSpeakChannelNumber, msgQueue.front());
#else
          mqttPublishTopic(g_thingsBoardTelemetryTopic, msgQueue.front());
#endif

        if (success) {
            ++g_packagesSent;
            g_lastPublishTime = (uint32_t)time(NULL);
            msgQueue.pop_front();
            errors = 0;
        } else {
            logger.error("mqttPublish failed.");
            ++errors;
            if (errors > 3) {
                logger.warning("Giving up for now...");
                break;
            }
        }
    }
    digitalWrite(LED_BUILTIN, 0);
}
