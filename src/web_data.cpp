#include "SPIFFS.h"
#include "BuildConfig.h"
#include "core/accumulator_v2.h"
#include "core/config.h"
#include "core/tasks.h"
#include "network/thingsboard.h"
#include "network/web.h"
#include "network/web_data.h"
#include <Arduino_JSON.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

// Rendered by webUpdateDataCache() from loop() and served verbatim by the
// request handler, which runs on the async_tcp task. The mutex covers the
// String itself; the sensor accumulators are only ever touched by the writer.
static String g_dataJson = "{}";
SemaphoreHandle_t g_dataMutex = nullptr;

static unsigned
getSignalStrength()
{
    static AccumulatorV2 rssiAcc(60);

    auto rssi = WiFi.RSSI();
    unsigned val = 0;
    if (rssi <= -100) {
        val = 0;
    } else if (rssi >= -50) {
        val = 100;
    } else {
        val = 2 * (rssi + 100);
    }
    rssiAcc.add(val);

    return rssiAcc.getAverage();
}

static void
addAccumulator(JSONVar& inputs, const char* name, AccumulatorV2& acc)
{
    JSONVar entry;
    entry["val"] = String(acc.getLast());
    entry["avg"] = String(acc.getAverage());
    entry["var"] = String(acc.variance);
    inputs[name] = entry;
}

void
webUpdateDataCache()
{
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    time_t now = mktime(&timeinfo);
    int uptime = now - g_bootTime;
    int minutes = uptime / 60;
    int hours = minutes / 60;
    int days = hours / 24;
    char buffer[32];

    JSONVar statusJson;
    statusJson["Hostname"] = g_hostname;
    statusJson["Firmware"] = FW_VERSION;
    strftime(buffer, sizeof(buffer), "%F %T", &timeinfo);
    statusJson["Date/Time"] = String(buffer);
    if (g_bootTime > g_safeTimestamp) {
        snprintf(buffer,
                 sizeof(buffer),
                 "%dd %dh %dm %ds",
                 days,
                 hours % 24,
                 minutes % 60,
                 uptime % 60);
        statusJson["Uptime"] = String(buffer);
    }
#ifdef HAS_DHT_SENSOR
    if (g_dhtTotalReads > 0) {
        statusJson["DHT Error Rate"] =
          String((float)g_dhtReadErrors / (float)g_dhtTotalReads * 100, 2);
    }
#endif
    statusJson["Internet"] = String((g_hasInternet) ? "online" : "offline");
    statusJson["Signal Strength"] = String(getSignalStrength()) + "%";
    statusJson["Ping"] = String(g_pingTime.getAverage()) + "ms";
    statusJson["Connection Loss Count"] = String(g_connectionLossCount);
    statusJson["MQTT"] = String((g_mqttEnabled) ? "enabled" : "disabled");
    {
        // Empty unless the broker has announced an image, so the row only
        // appears while there is something to say about it.
        const String fota = tbFotaStatus();
        if (fota.length() > 0) {
            statusJson["Cloud Update"] = fota;
        }
    }
    statusJson["Packages Sent"] = String(g_packagesSent);
    statusJson["Watering Cycles"] = String(g_wateringCycles);
    // Surfaced because the filesystem is the resource that silently runs out:
    // the history buffer, the log backups and every web asset share it.
    statusJson["Filesystem"] = String(SPIFFS.usedBytes() / 1024) + " / " +
                               String(SPIFFS.totalBytes() / 1024) + " KB";

    JSONVar inputsJson;
#ifdef HAS_MOISTURE_SENSOR
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
        const String name = config.soilMoistureName[i];
        addAccumulator(inputsJson, name.c_str(), g_soilMoisture[i]);

        // Empty unless the probe has been calibrated against air and water.
        const String state = moistureState(i);
        if (state.length() > 0) {
            inputsJson[name.c_str()]["state"] = state;
        }
    }
#endif

#ifdef HAS_LUMINOSITY_SENSOR
    addAccumulator(inputsJson, config.luminosityName.c_str(), g_luminosity);
#endif

#ifdef HAS_DHT_SENSOR
    // One pin, two channels: a name given to the DHT reads as a prefix so both
    // channels stay distinguishable.
    {
        const String prefix =
          config.dhtName.length() > 0 ? (config.dhtName + " ") : String("");
        const String tempName = prefix + "Temperature";
        const String humName =
          config.dhtName.length() > 0 ? (prefix + "Humidity") : String("Air Humidity");
        addAccumulator(inputsJson, tempName.c_str(), g_temperature);
        addAccumulator(inputsJson, humName.c_str(), g_airHumidity);
    }
#endif

#ifdef HAS_WATER_LEVEL_SENSOR
    addAccumulator(inputsJson, config.waterLevelName.c_str(), g_waterLevel);
#endif

#ifdef HAS_FLOW_SENSOR
    addAccumulator(inputsJson, config.flowName.c_str(), g_flowRate);
    {
        // A running total has no window, so it does not fit the val/avg/var
        // shape the other inputs use; it gets its own entry.
        JSONVar total;
        total["val"] = String(flowTotalLitres(), 3);
        total["avg"] = String(flowTotalLitres(), 3);
        total["var"] = String(0);
        const String totalName = config.flowName + " Total";
        inputsJson[totalName.c_str()] = total;
    }
#endif

#ifdef HAS_FLOAT_SWITCH
    {
        // Binary: reporting a number here would invite a chart of 0s and 1s.
        JSONVar entry;
        entry["val"] = String(floatRaised() ? 1 : 0);
        entry["avg"] = String(floatRaised() ? 1 : 0);
        entry["var"] = String(0);
        entry["state"] = floatRaised() ? "Raised" : "Lowered";
        inputsJson[config.floatName.c_str()] = entry;
    }
#endif

    JSONVar outputsJson;
    for (unsigned i = 0; i < RELAY_COUNT; ++i) {
        outputsJson[config.relayName[i].c_str()] =
          String(relayIsOn(i) ? 1 : 0);
    }

    JSONVar relaysJson;
    for (unsigned i = 0; i < RELAY_COUNT; ++i) {
        JSONVar relay;
        relay["index"] = (int)i;
        relay["name"] = config.relayName[i];
        relay["on"] = relayIsOn(i) ? 1 : 0;
        relay["remaining"] = (double)relayRemaining(i);
        relaysJson[i] = relay;
    }

    JSONVar responseJson;
    responseJson["Status"] = statusJson;
    responseJson["Inputs"] = inputsJson;
    responseJson["Outputs"] = outputsJson;
    responseJson["Relays"] = relaysJson;
    responseJson["Channel"] = String(g_thingSpeakChannelNumber);

    String rendered = JSON.stringify(responseJson);

    if (g_dataMutex == nullptr) {
        g_dataJson = rendered;
        return;
    }

    if (xSemaphoreTake(g_dataMutex, portMAX_DELAY) == pdTRUE) {
        g_dataJson = rendered;
        xSemaphoreGive(g_dataMutex);
    }
}

void
handleDataJson(AsyncWebServerRequest* request)
{
    digitalWrite(LED_BUILTIN, 1);

    String payload;
    if ((g_dataMutex != nullptr) &&
        (xSemaphoreTake(g_dataMutex, portMAX_DELAY) == pdTRUE)) {
        payload = g_dataJson;
        xSemaphoreGive(g_dataMutex);
    } else {
        payload = g_dataJson;
    }

    request->send(200, "application/json", payload);

    digitalWrite(LED_BUILTIN, 0);
}
