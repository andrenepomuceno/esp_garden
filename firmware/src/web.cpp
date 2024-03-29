#include "SPIFFS.h"
#include "accumulator_v2.h"
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
    statusJson["Hostname"] = g_hostname;
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
    JSONVar soilMoisture;
    soilMoisture["val"] = String(g_soilMoisture.getLast());
    soilMoisture["avg"] = String(g_soilMoisture.getAverage());
    soilMoisture["var"] = String(g_soilMoisture.variance);
    inputsJson["Soil Moisture"] = soilMoisture;
#endif

#ifdef HAS_LUMINOSITY_SENSOR
    JSONVar luminosity;
    luminosity["val"] = String(g_luminosity.getLast());
    luminosity["avg"] = String(g_luminosity.getAverage());
    luminosity["var"] = String(g_luminosity.variance);
    inputsJson["Luminosity"] = luminosity;
#endif

#ifdef HAS_DHT_SENSOR
    JSONVar temperature;
    temperature["val"] = String(g_temperature.getLast());
    temperature["avg"] = String(g_temperature.getAverage());
    temperature["var"] = String(g_temperature.variance);
    inputsJson["Temperature"] = temperature;

    JSONVar airHumidity;
    airHumidity["val"] = String(g_airHumidity.getLast());
    airHumidity["avg"] = String(g_airHumidity.getAverage());
    airHumidity["var"] = String(g_airHumidity.variance);
    inputsJson["Air Humidity"] = airHumidity;
#endif

#ifdef HAS_WATER_LEVEL_SENSOR
    JSONVar waterLevel;
    waterLevel["val"] = String(g_waterLevel.getLast());
    waterLevel["avg"] = String(g_waterLevel.getAverage());
    waterLevel["var"] = String(g_waterLevel.variance);
    inputsJson["Water Level"] = waterLevel;
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

    WiFi.begin(g_ssid.c_str(), g_wifiPassword.c_str());
}

void
webSetup()
{
    logger.println("Web setup...");

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(g_hostname.c_str());
    WiFi.onEvent(wifiConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent(wifiGotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(wifiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.begin(g_ssid.c_str(), g_wifiPassword.c_str());

    if (MDNS.begin(g_hostname.c_str()) == false) {
        logger.println("Error starting mDNS!");
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
    g_webServer.serveStatic("/", SPIFFS, "/")
      .setAuthentication(g_otaUser.c_str(), g_otaPassword.c_str());
    g_webServer.onNotFound(
      [](AsyncWebServerRequest* request) { request->send(404); });

    AsyncElegantOTA.begin(
      &g_webServer, g_otaUser.c_str(), g_otaPassword.c_str());

    g_webServer.begin();

    logger.println("Web setup done!");
}
