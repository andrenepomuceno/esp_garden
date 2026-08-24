#include "core/telemetry.h"
#include "BuildConfig.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/relays.h"
#include "core/sensors.h"
#include "core/tasks.h"
#include "network/mqtt.h"
#include <Arduino_JSON.h>
#include <list>

// Second soil probe on ThingSpeak: config.thingSpeakMoisture2Field, 0 to keep
// it off. This is configuration and not a build flag because the answer is
// per-device -- field 4 is free on a board without a water level sensor and
// taken on a board with one -- and because reusing a number rewrites the
// meaning of everything already stored under it, which is a decision for
// whoever owns the channel, not for whoever runs the compiler.
//
// Probes 3 and beyond have no ThingSpeak field at all. Eight fields is the
// ceiling, and it is the reason this firmware also speaks ThingsBoard.

static String g_mqttMessage = "";

static const char* const g_thingsBoardTelemetryTopic = "v1/devices/me/telemetry";

unsigned g_packagesSent = 0;

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

// ThingsBoard telemetry: one JSON object, arbitrary keys. This is the reason
// to support it at all — the ThingSpeak channel's eight fields are spoken for,
// which is what keeps probes 2 and 3 off the cloud entirely.
//
// Written through chained subscripts on a named JSONVar, never by returning one
// by value: that library's move-assign for rvalues yields null children.
static String
buildThingsBoardPayload()
{
    JSONVar telemetry;

    for (unsigned i = 0; i < config.moistureCount; ++i) {
        if (g_soilMoisture[i].getSamples() == 0) {
            continue; // never sampled: sending 0 would read as a real value
        }
        const String key = "moisture" + String(i + 1);
        telemetry[key.c_str()] = (double)g_soilMoisture[i].getAverage();

        const String state = moistureState(i);
        if (state.length() > 0) {
            const String stateKey = "moisture" + String(i + 1) + "State";
            telemetry[stateKey.c_str()] = state;
        }
    }
    if (g_luminosity.getSamples() > 0) {
        telemetry["luminosity"] = (double)g_luminosity.getAverage();
    }
    if (g_temperature.getSamples() > 0) {
        telemetry["temperature"] = (double)g_temperature.getAverage();
    }
    if (g_airHumidity.getSamples() > 0) {
        telemetry["airHumidity"] = (double)g_airHumidity.getAverage();
    }
    if (g_dhtTotalReads > 0) {
        telemetry["dhtErrorRate"] =
          (double)g_dhtReadErrors * 100.0 / (double)g_dhtTotalReads;
    }
    if (g_waterLevel.getSamples() > 0) {
        telemetry["waterLevel"] = (double)g_waterLevel.getAverage();
    }
    // The cumulative total only exists in RAM and resets on every reboot, so
    // without publishing it the questions a flow meter is installed to answer —
    // how much did last night's watering actually deliver, is the line dripping
    // while the relay is off — cannot be asked afterwards. ThingsBoard has no
    // field limit, which is what makes this free here and impossible on the
    // ThingSpeak side.
    if (config.flowFitted) {
        if (g_flowRate.getSamples() > 0) {
            telemetry["flowRate"] = (double)g_flowRate.getAverage();
        }
        // Guarded, unlike the rate: a running total has no getSamples() to say
        // "never fed", so an unfitted meter would publish a perfectly real
        // 0 litres that no dashboard can tell from a line that delivered
        // nothing. Same reason the history record guards it.
        telemetry["flowTotalLitres"] = (double)flowTotalLitres();
    }
    if (config.floatFitted) {
        // Without the guard every board publishes reservoirRaised:false, which
        // is indistinguishable from a genuinely empty tank — the exact case
        // IO_HISTORY_FLAG_FLOAT_VALID exists for.
        telemetry["reservoirRaised"] = floatRaised();
    }

    // relayN is what the relay is doing RIGHT NOW; relayNRan says whether it
    // ran at any point in the period just published. Both are needed and they
    // answer different questions: the first drives a live indicator, the second
    // is the only one that can see a five-second watering between two
    // one-minute publishes.
    const uint16_t fired = relayStickyTake(RELAY_STICKY_TELEMETRY);
    uint16_t mask = 0;
    for (unsigned i = 0; i < config.relayCount && i < 16; ++i) {
        const bool on = relayIsOn(i);
        const bool ran = (fired & (uint16_t)(1u << i)) != 0;

        const String key = "relay" + String(i + 1);
        telemetry[key.c_str()] = on;
        const String ranKey = key + "Ran";
        telemetry[ranKey.c_str()] = ran;

        if (on) {
            mask |= (uint16_t)(1u << i);
        }
    }
    telemetry["relayMask"] = (int)mask;
    telemetry["relayRanMask"] = (int)fired;

    if (g_pendingWateringMs > 0) {
        telemetry["wateringMs"] = (int)g_pendingWateringMs;
        g_pendingWateringMs = 0;
    }

    telemetry["ping"] = (double)g_pingTime.getAverage();
    telemetry["connectionLoss"] = (int)g_connectionLossCount;
    telemetry["wateringCycles"] = (int)g_wateringCycles;
    telemetry["firmware"] = FW_VERSION;

    return JSON.stringify(telemetry);
}

void
telemetryPublish()
{
    static std::list<String> msgQueue;

    if (!g_mqttEnabled || !g_hasInternet) {
        logger.info("MQTT skipped.");
        logger.info("g_mqttEnabled = " + String(g_mqttEnabled) +
                    " g_hasInternet = " + String(g_hasInternet));
        return;
    }

    if (mqttIsThingsBoard()) {
        msgQueue.push_back(buildThingsBoardPayload());
        g_mqttMessage = ""; // the ThingSpeak accumulator is unused here
    } else {

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

    digitalWrite(LED_BUILTIN, 1);
    int errors = 0;
    const unsigned maxMsgQueueSize = 60 * 60 * 1000 / g_mqttTaskPeriod;
    while (msgQueue.size() > maxMsgQueueSize) {
        logger.warning("msgQueue is full, discarding messages..");
        msgQueue.pop_front();
    }
    while (msgQueue.size() > 0) {
        const bool success =
          mqttIsThingsBoard()
            ? mqttPublishTopic(g_thingsBoardTelemetryTopic, msgQueue.front())
            : mqttPublish(g_thingSpeakChannelNumber, msgQueue.front());

        if (success) {
            ++g_packagesSent;
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
