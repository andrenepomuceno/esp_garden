#include "core/filesystem.h"
#include "BuildConfig.h"
#include "core/accumulator_v2.h"
#include "core/config.h"
#include <esp_heap_caps.h>
#include "core/probe_health.h"
#include "core/sensors.h"
#include "core/tasks.h"
#include "network/mqtt.h"
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
    if (g_dhtTotalReads > 0) {
        statusJson["DHT Error Rate"] =
          String((float)g_dhtReadErrors / (float)g_dhtTotalReads * 100, 2);
    }
    statusJson["Internet"] = String((g_hasInternet) ? "online" : "offline");
    statusJson["Signal Strength"] = String(getSignalStrength()) + "%";
    statusJson["Ping"] = String(g_pingTime.getAverage()) + "ms";
    statusJson["Connection Loss Count"] = String(g_connectionLossCount);
    // Two different questions, and conflating them is what made a three-year
    // outage invisible. "MQTT" is the operator's switch — index.js binds a
    // checkbox to it, so its values stay "enabled"/"disabled". "MQTT Link" is
    // what the device actually has.
    statusJson["MQTT"] = String((g_mqttEnabled) ? "enabled" : "disabled");
    if (!g_mqttEnabled) {
        statusJson["MQTT Link"] = "disabled";
    } else if (mqttIsConnected()) {
        statusJson["MQTT Link"] = "connected";
    } else {
        // The state code names the refusal instead of leaving it to be
        // guessed: -2 covers a failed TLS handshake, which is where a stale CA
        // pin lands and where this device sat, unnoticed, from 2023 to 2026.
        statusJson["MQTT Link"] =
          "down (rc=" + String(mqttState()) + ")";
    }

    // The age of the last ACCEPTED publish. `Packages Sent` counts up from
    // zero at every boot, so on a device that has been up an hour it reads the
    // same whether the broker is taking everything or has taken nothing since
    // the second minute.
    if (g_lastPublishTime == 0) {
        statusJson["Last Publish"] = g_mqttEnabled ? "never" : "n/a";
    } else {
        const time_t now = time(NULL);
        const long age = (long)now - (long)g_lastPublishTime;
        char buffer[32];
        if (age < 0) {
            // The clock moved backwards under us — an NTP correction, most
            // likely. Better to say so than to print a negative age.
            snprintf(buffer, sizeof buffer, "clock stepped");
        } else if (age < 120) {
            snprintf(buffer, sizeof buffer, "%lds ago", age);
        } else if (age < 7200) {
            snprintf(buffer, sizeof buffer, "%ldm ago", age / 60);
        } else {
            snprintf(buffer, sizeof buffer, "%ldh ago", age / 3600);
        }
        statusJson["Last Publish"] = String(buffer);
    }
    // Only while it is actually blocking something. A refusal that leaves no
    // trace in the UI is indistinguishable from a relay button that does not
    // work, which is how a safety feature becomes a bug report.
    if (config.floatInterlock && !floatRaised()) {
        statusJson["Interlock"] =
          "pumps blocked — " + config.floatName + " reads empty";
    }
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
    // Heap, because it is the resource that runs out without a trace. This
    // board has 320 KB of DRAM and no PSRAM; a TLS handshake holds 30-45 KB,
    // /history.json peaks around 56 KB building its response, and every
    // Arduino_JSON payload allocates freely. Until these two numbers existed,
    // a device that stopped responding while someone browsed left nothing to
    // reason from.
    //
    // Free is now; Min Free is the low-water mark since boot — which is the one
    // that matters, because the spike that kills the device is over by the time
    // anyone asks.
    statusJson["Free Heap"] = String(ESP.getFreeHeap() / 1024) + " KB";
    statusJson["Min Free Heap"] = String(ESP.getMinFreeHeap() / 1024) + " KB";
    statusJson["Largest Block"] =
      String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024) + " KB";

    // Surfaced because the filesystem is the resource that silently runs out:
    // the history buffer, the log backups and every web asset share it.
    statusJson["Filesystem"] = String(FILESYSTEM.usedBytes() / 1024) + " / " +
                               String(FILESYSTEM.totalBytes() / 1024) + " KB";

    JSONVar inputsJson;
    for (unsigned i = 0; i < config.moistureCount; ++i) {
        const String name = config.soilMoistureName[i];
        addAccumulator(inputsJson, name.c_str(), g_soilMoisture[i]);

        // Empty unless the probe has been calibrated against air and water.
        const String state = moistureState(i);
        if (state.length() > 0) {
            inputsJson[name.c_str()]["state"] = state;
        }

        // Present ONLY when something is wrong, the same way `state` is absent
        // rather than empty. The dashboard is read at a glance, and a column
        // that says "connected" on every healthy probe trains the eye to skip
        // the one case that matters. /moisture.json carries the evidence; this
        // is the flag that sends you there.
        const ProbeHealthReport health = probeHealthReport(i);
        if (health.verdict != PROBE_UNKNOWN &&
            health.verdict != PROBE_CONNECTED) {
            inputsJson[name.c_str()]["fault"] =
              probeVerdictName(health.verdict);
        }
    }

    // Every entry below is gated on the sensor being FITTED. addAccumulator
    // does not check getSamples(), so an unfitted channel would report a
    // confident "0.00" — and /devices.html builds its inventory from these
    // keys, so it would list hardware nobody installed.
    if (config.luminosityFitted) {
        addAccumulator(inputsJson, config.luminosityName.c_str(), g_luminosity);
    }

    // One pin, two channels: a name given to the DHT reads as a prefix so both
    // channels stay distinguishable.
    if (config.dhtFitted) {
        const String prefix =
          config.dhtName.length() > 0 ? (config.dhtName + " ") : String("");
        const String tempName = prefix + "Temperature";
        const String humName =
          config.dhtName.length() > 0 ? (prefix + "Humidity") : String("Air Humidity");
        addAccumulator(inputsJson, tempName.c_str(), g_temperature);
        addAccumulator(inputsJson, humName.c_str(), g_airHumidity);
    }

    if (config.waterLevelFitted) {
        addAccumulator(inputsJson, config.waterLevelName.c_str(), g_waterLevel);
    }

    if (config.flowFitted) {
        addAccumulator(inputsJson, config.flowName.c_str(), g_flowRate);

        // A running total has no window, so it does not fit the val/avg/var
        // shape the other inputs use; it gets its own entry.
        JSONVar total;
        total["val"] = String(flowTotalLitres(), 3);
        total["avg"] = String(flowTotalLitres(), 3);
        total["var"] = String(0);
        const String totalName = config.flowName + " Total";
        inputsJson[totalName.c_str()] = total;
    }

    if (config.floatFitted) {
        // Binary: reporting a number here would invite a chart of 0s and 1s.
        JSONVar entry;
        entry["val"] = String(floatRaised() ? 1 : 0);
        entry["avg"] = String(floatRaised() ? 1 : 0);
        entry["var"] = String(0);
        entry["state"] = floatRaised() ? "Raised" : "Lowered";
        inputsJson[config.floatName.c_str()] = entry;
    }

    JSONVar outputsJson;
    for (unsigned i = 0; i < config.relayCount; ++i) {
        outputsJson[config.relayName[i].c_str()] =
          String(relayIsOn(i) ? 1 : 0);
    }

    JSONVar relaysJson;
    for (unsigned i = 0; i < config.relayCount; ++i) {
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
