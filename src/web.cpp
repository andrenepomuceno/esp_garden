#include "SPIFFS.h"
#include "BuildConfig.h"
#include "core/accumulator_v2.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/tasks.h"
#include <Arduino_JSON.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

static AsyncWebServer g_webServer(80);

bool g_wifiConnected = false;
bool g_hasNetwork = false;

// Rendered by webUpdateDataCache() from loop() and served verbatim by the
// request handler, which runs on the async_tcp task. The mutex covers the
// String itself; the sensor accumulators are only ever touched by the writer.
static String g_dataJson = "{}";
static SemaphoreHandle_t g_dataMutex = nullptr;

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
    statusJson["Packages Sent"] = String(g_packagesSent);
    statusJson["Watering Cycles"] = String(g_wateringCycles);

    JSONVar inputsJson;
#ifdef HAS_MOISTURE_SENSOR
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
        // A single probe keeps the historical label so existing dashboards and
        // the simulator do not have to special-case one device.
        String name = (MOISTURE_SENSOR_COUNT == 1)
                        ? String("Soil Moisture")
                        : ("Soil Moisture " + String(i + 1));
        addAccumulator(inputsJson, name.c_str(), g_soilMoisture[i]);
    }
#endif

#ifdef HAS_LUMINOSITY_SENSOR
    addAccumulator(inputsJson, "Luminosity", g_luminosity);
#endif

#ifdef HAS_DHT_SENSOR
    addAccumulator(inputsJson, "Temperature", g_temperature);
    addAccumulator(inputsJson, "Air Humidity", g_airHumidity);
#endif

#ifdef HAS_WATER_LEVEL_SENSOR
    addAccumulator(inputsJson, "Water Level", g_waterLevel);
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

void
handleControl(AsyncWebServerRequest* request)
{
    digitalWrite(LED_BUILTIN, 1);

    // relay + relayTime address any relay; watering / wateringTime are the
    // legacy spelling for relay 0 and stay supported.
    if (request->hasParam("relay", true)) {
        const unsigned index =
          request->getParam("relay", true)->value().toInt();
        unsigned duration = g_wateringDefaultTime;
        if (request->hasParam("relayTime", true)) {
            duration = request->getParam("relayTime", true)->value().toInt();
        }
        startRelay(index, duration);
    }

    for (int i = 0; i < request->params(); ++i) {
        const AsyncWebParameter* param = request->getParam(i);
        if ((param->name() == "watering") && (param->value() == "enable")) {
            startWatering();
        } else if (param->name() == "wateringTime") {
            startWatering(param->value().toInt());
        } else if (param->name() == "mqtt") {
            if (param->value() == "enable") {
                mqttEnable(true);
            } else if (param->value() == "disable") {
                mqttEnable(false);
            }
        } else if ((param->name() == "reset") && (param->value() == "1")) {
            ESP.restart();
        }
    }

    request->send(200);

    digitalWrite(LED_BUILTIN, 0);
}

void
handleLogs(AsyncWebServerRequest* request)
{
    digitalWrite(LED_BUILTIN, 1);

    String output = logger.read();
    request->send(200, "text/plain", output);

    digitalWrite(LED_BUILTIN, 0);
}

static bool g_otaEnabled = false;

static void
handleUpdateEnable(AsyncWebServerRequest* request)
{
    g_otaEnabled = true;
    logger.info("[OTA] Enabled OTA");
    request->send(200);
}

static void
handleUpdateRequest(AsyncWebServerRequest* request)
{
    if (!g_otaEnabled) {
        request->send(400);
        return;
    }

    bool error = Update.hasError();
    int code = error ? 500 : 200;
    const char* content = error ? "FAIL" : "OK";
    AsyncWebServerResponse* response =
      request->beginResponse(code, "text/plain", content);
    response->addHeader("Connection", "close");
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);

    // Disarm either way: a flag left set accepts an unsolicited image for as
    // long as the device stays up.
    g_otaEnabled = false;

    if (!error) {
        delay(500);
        ESP.restart();
    }
}

static void
handleUpdateUpload(AsyncWebServerRequest* request,
                   String filename,
                   size_t index,
                   uint8_t* data,
                   size_t len,
                   bool final)
{
    if (!g_otaEnabled) {
        request->send(400);
        return;
    }

    if (!index) {
        logger.info("[OTA] Starting update: " + filename);
        int cmd = (filename == "filesystem") ? U_SPIFFS : U_FLASH;
        if (request->hasParam("MD5", true)) {
            Update.setMD5(request->getParam("MD5", true)->value().c_str());
        }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
            logger.error(String("[OTA] ") + Update.errorString());
            return request->send(400, "text/plain", "OTA could not begin");
        }
    }

    if (len) {
        if (Update.write(data, len) != len) {
            logger.error(String("[OTA] ") + Update.errorString());
            return request->send(400, "text/plain", "OTA could not write");
        }
    }

    if (final) {
        if (!Update.end(true)) {
            logger.error(String("[OTA] ") + Update.errorString());
            return request->send(400, "text/plain", "OTA could not end");
        }
        logger.info("[OTA] Complete!");
    }
}

static void
wifiConnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    logger.info("Wifi connected.");
    g_wifiConnected = true;
}

static void
wifiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
    logger.info("IP address: " + WiFi.localIP().toString());
    g_hasNetwork = true;
}

static void
wifiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (g_wifiConnected) {
        logger.warning("Wifi disconnected. Reconnecting...");
    }

    g_wifiConnected = false;
    g_hasNetwork = false;

    WiFi.begin(g_ssid.c_str(), g_wifiPassword.c_str());
}

void
webSetup()
{
    logger.info("Web setup...");

    g_dataMutex = xSemaphoreCreateMutex();
    webUpdateDataCache();

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(g_hostname.c_str());
    WiFi.onEvent(wifiConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent(wifiGotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(wifiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.begin(g_ssid.c_str(), g_wifiPassword.c_str());

    if (MDNS.begin(g_hostname.c_str()) == false) {
        logger.warning("Error starting mDNS!");
    }

    g_webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        digitalWrite(LED_BUILTIN, 1);
        request->send(SPIFFS, "/index.html", "text/html");
        digitalWrite(LED_BUILTIN, 0);
    });
    g_webServer.on(
      "/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* request) {
          digitalWrite(LED_BUILTIN, 1);
          request->send(SPIFFS, "/favicon.ico", "image/x-icon");
          digitalWrite(LED_BUILTIN, 0);
      });

    g_webServer.on("/data.json", HTTP_GET, handleDataJson);
    g_webServer.on("/control", HTTP_POST, handleControl);
    g_webServer.on("/logs", HTTP_GET, handleLogs);
    g_webServer.on("/updateEnable", HTTP_POST, handleUpdateEnable);
    g_webServer.on(
      "/update", HTTP_POST, handleUpdateRequest, handleUpdateUpload);
    g_webServer.serveStatic("/", SPIFFS, "/")
      .setAuthentication(g_otaUser.c_str(), g_otaPassword.c_str());
    g_webServer.onNotFound(
      [](AsyncWebServerRequest* request) { request->send(404); });

    g_webServer.begin();

    logger.info("Web setup done!");
}
