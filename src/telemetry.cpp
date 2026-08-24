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

// Second soil probe. Field 4 is free only on a board without a water level
// sensor; where both exist the slot has to be chosen deliberately, because
// reusing a number rewrites the meaning of everything already stored under it.
// Set -D MOISTURE2_FIELD=<n> in platformio.ini to pick one; leaving it unset
// keeps the probe on the dashboard and out of the cloud.
#if (MOISTURE_SENSOR_COUNT > 1) && !defined(MOISTURE2_FIELD) &&                \
  !defined(HAS_WATER_LEVEL_SENSOR)
#define MOISTURE2_FIELD 4
#endif

#if (MOISTURE_SENSOR_COUNT > 1) && !defined(MOISTURE2_FIELD)
#warning "Second soil probe is dashboard-only: no ThingSpeak field assigned (set -D MOISTURE2_FIELD=<n>)."
#endif

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

#ifdef HAS_MOISTURE_SENSOR
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
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
#endif
#ifdef HAS_LUMINOSITY_SENSOR
    if (g_luminosity.getSamples() > 0) {
        telemetry["luminosity"] = (double)g_luminosity.getAverage();
    }
#endif
#ifdef HAS_DHT_SENSOR
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
#endif
#ifdef HAS_WATER_LEVEL_SENSOR
    if (g_waterLevel.getSamples() > 0) {
        telemetry["waterLevel"] = (double)g_waterLevel.getAverage();
    }
#endif
#ifdef HAS_FLOW_SENSOR
    // The cumulative total only exists in RAM and resets on every reboot, so
    // without publishing it the questions a flow meter is installed to answer —
    // how much did last night's watering actually deliver, is the line dripping
    // while the relay is off — cannot be asked afterwards. ThingsBoard has no
    // field limit, which is what makes this free here and impossible on the
    // ThingSpeak side.
    if (g_flowRate.getSamples() > 0) {
        telemetry["flowRate"] = (double)g_flowRate.getAverage();
    }
    telemetry["flowTotalLitres"] = (double)flowTotalLitres();
#endif
#ifdef HAS_FLOAT_SWITCH
    telemetry["reservoirRaised"] = floatRaised();
#endif

    uint16_t mask = 0;
    for (unsigned i = 0; i < RELAY_COUNT && i < 16; ++i) {
        const String key = "relay" + String(i + 1);
        const bool on = relayIsOn(i);
        telemetry[key.c_str()] = on;
        if (on) {
            mask |= (uint16_t)(1u << i);
        }
    }
    telemetry["relayMask"] = (int)mask;

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

#ifdef HAS_MOISTURE_SENSOR
        mqttAddField(g_soilMoistureField,
                     FLOAT_TO_STRING(g_soilMoisture[0].getAverage()));
#if (MOISTURE_SENSOR_COUNT > 1) && defined(MOISTURE2_FIELD)
        mqttAddField(MOISTURE2_FIELD,
                     FLOAT_TO_STRING(g_soilMoisture[1].getAverage()));
#endif
#endif

#ifdef HAS_LUMINOSITY_SENSOR
        mqttAddField(g_luminosityField, FLOAT_TO_STRING(g_luminosity.getAverage()));
#endif

#ifdef HAS_DHT_SENSOR
        mqttAddField(g_temperatureField,
                     FLOAT_TO_STRING(g_temperature.getAverage()));
        mqttAddField(g_airHumidityField,
                     FLOAT_TO_STRING(g_airHumidity.getAverage()));
#endif

#ifdef HAS_WATER_LEVEL_SENSOR
        mqttAddField(g_waterLevelField, FLOAT_TO_STRING(g_waterLevel.getAverage()));
#endif

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
