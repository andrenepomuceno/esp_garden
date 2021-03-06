#include "SPIFFS.h"
#include "accumulator.h"
#include "config.h"
#include "html.h"
#include "logger.h"
#include "tasks.h"
#include <Arduino_JSON.h>
#include <AsyncElegantOTA.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <time.h>

static AsyncWebServer g_webServer(80);

bool g_wifiConnected = false;
bool g_hasNetwork = false;

static unsigned
getSignalStrength()
{
    auto rssi = WiFi.RSSI();
    if (rssi <= -100) {
        return 0;
    } else if (rssi >= -50) {
        return 100;
    } else {
        return 2 * (rssi + 100);
    }
}

void
handleDataJson(AsyncWebServerRequest* request)
{
    digitalWrite(LED_BUILTIN, 1);

    struct tm timeinfo;
    getLocalTime(&timeinfo);
    time_t now = mktime(&timeinfo);
    int uptime = now - g_bootTime;
    int minutes = uptime / 60;
    int hours = minutes / 60;
    int days = hours / 24;
    char buffer[32];

    JSONVar statusJson;
    statusJson["Hostname"] = String(g_hostname);
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
    statusJson["Internet"] = String((g_hasInternet) ? "online" : "offline");
    statusJson["Signal Strength"] = String(getSignalStrength()) + "%";
    statusJson["ThingSpeak"] =
      String((g_thingSpeakEnabled) ? "enabled" : "disabled");
    statusJson["Packages Sent"] = String(g_packagesSent);
    statusJson["Watering Cycles"] = String(g_wateringCycles);
#ifdef HAS_DHT_SENSOR
    statusJson["DHT Read Errors"] = String(g_dhtReadErrors);
#endif

    JSONVar inputsJson;
#ifdef HAS_MOISTURE_SENSOR
    inputsJson["Soil Moisture"] = String(g_soilMoisture.getLast()) + "/" +
                                  String(g_soilMoisture.getAverage());
#endif
#ifdef HAS_LUMINOSITY_SENSOR
    inputsJson["Luminosity"] =
      String(g_luminosity.getLast()) + "/" + String(g_luminosity.getAverage());
#endif
#ifdef HAS_DHT_SENSOR
    inputsJson["Temperature"] = String(g_temperature.getLast());
    inputsJson["Air Humidity"] = String(g_airHumidity.getLast());
#endif

    JSONVar outputsJson;
    outputsJson["Watering"] = String(g_wateringState);

    JSONVar responseJson;
    responseJson["Status"] = statusJson;
    responseJson["Inputs"] = inputsJson;
    responseJson["Outputs"] = outputsJson;
    responseJson["Channel"] = String(g_thingSpeakChannelNumber);

    request->send(200, "application/json", JSON.stringify(responseJson));

    digitalWrite(LED_BUILTIN, 0);
}

void
handleControl(AsyncWebServerRequest* request)
{
    digitalWrite(LED_BUILTIN, 1);

    for (int i = 0; i < request->params(); ++i) {
        AsyncWebParameter* param = request->getParam(i);
        if ((param->name() == "watering") && (param->value() == "enable")) {
            startWatering();
        } else if (param->name() == "wateringTime") {
            startWatering(param->value().toInt());
        } else if (param->name() == "thingSpeak") {
            if (param->value() == "enable") {
                thingSpeakEnable(true);
            } else if (param->value() == "disable") {
                thingSpeakEnable(false);
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

static void
wifiConnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    logger.println("Wifi connected.");
    g_wifiConnected = true;
}

static void
wifiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
    logger.println("IP address: " + WiFi.localIP().toString());
    g_hasNetwork = true;
}

static void
wifiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (g_wifiConnected) {
        logger.println("Wifi disconnected. Reconnecting...");
    }

    g_wifiConnected = false;
    g_hasNetwork = false;

    WiFi.begin(g_ssid, g_wifiPassword);
}

void
webSetup()
{
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(g_hostname);
    WiFi.onEvent(wifiConnected, SYSTEM_EVENT_STA_CONNECTED);
    WiFi.onEvent(wifiGotIP, SYSTEM_EVENT_STA_GOT_IP);
    WiFi.onEvent(wifiDisconnected, SYSTEM_EVENT_STA_DISCONNECTED);
    WiFi.begin(g_ssid, g_wifiPassword);

    if (MDNS.begin(g_hostname) == false) {
        logger.println("Error starting mDNS!");
    }

    g_webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        digitalWrite(LED_BUILTIN, 1);
        request->send(SPIFFS, "/index.html", "text/html");
        digitalWrite(LED_BUILTIN, 0);
    });
    g_webServer.on("/data.json", HTTP_GET, handleDataJson);
    g_webServer.on("/control", HTTP_POST, handleControl);
    g_webServer.on("/logs", HTTP_GET, handleLogs);
    g_webServer.serveStatic("/", SPIFFS, "/");
    g_webServer.onNotFound(
      [](AsyncWebServerRequest* request) { request->send(404); });

    AsyncElegantOTA.begin(&g_webServer, g_otaUser, g_otaPassword);

    g_webServer.begin();

    logger.println("Web setup done!");
}

void
webLoop()
{
    // AsyncElegantOTA.loop();
}