#include "accumulator.h"
#include "config.h"
#include "html.h"
#include "logger.h"
#include "tasks.h"
#include <AsyncElegantOTA.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <time.h>

static const char g_indexHtml[] PROGMEM =
  R"EOF(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><link href="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/css/bootstrap.min.css" rel="stylesheet" integrity="sha384-eOJMYsd53ii+scO/bJGFsiCZc+5NDVN2yr8+0RDqr0Ql0h+rP48ckxlpbzKgwra6" crossorigin="anonymous"><title>ESP Garden</title><style>.control-panel{ display: flex;} .control-panel .row{ align-items: center;} </style></head><body><div class="container"><h1>&#x1F331; ESP Garden</h1><h2>Status</h2><table class="table table-striped"><tbody id="tbody-status"></tbody></table><h2>Inputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Name</th><th scope="col">Value</th></tr></thead><tbody id="tbody-inputs"></tbody></table><h2>Outputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Name</th><th scope="col">Value</th></tr></thead><tbody id="tbody-outputs"></tbody></table><h2>Control</h2><div class="control-panel"><div class="row"><div class="col"><div class="form-check form-switch"><input id="input-thingspeak" class="form-check-input" type="checkbox"><label class="form-check-label" for="input-thingspeak">ThingSpeak</label></div></div><div class="col"><div class="form-check form-switch"><input id="input-watering" class="form-check-input" type="checkbox"><label class="form-check-label" for="input-watering">Watering</label></div></div><div class="col"><button id="button-reset" type="button" class="btn btn-primary">Reset</button></div></div></div><h2>Logs</h2><textarea class="form-control" id="textarea-logs" rows="8" readonly></textarea><div class="form-check form-switch"><input id="input-scroll" class="form-check-input checked" type="checkbox" checked><label class="form-check-label" for="input-scroll">Automatic scroll</label></div><script src="https://code.jquery.com/jquery-3.6.0.min.js" integrity="sha256-/xUj+3OJU5yExlq6GSYGSHk7tPXikynS7ogEvDej/m4=" crossorigin="anonymous"></script><script src="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/js/bootstrap.bundle.min.js" integrity="sha384-JEW9xMcG8R+pH31jmWH6WWP0WintQrMb4s7ZOdauHnUtxwoG2vI5DkLtS3qm9Ekf" crossorigin="anonymous"></script><script>function fillTable(id, data, indexCol=true){ var i=0; var tbody=$(id); tbody.empty(); for (var key in data){ var tr=$('<tr/>'); if (indexCol==true) tr.append($('<th/>').html(i++).attr("scope", "row")); tr.append($('<td/>').html(key)); tr.append($('<td/>').html(data[key])); tbody.append(tr);}} function refresh(){ setTimeout(refresh, 1 * 1000); $.ajax({ dataType: "json", url: "/data.json", timeout: 500, success: function (info){ fillTable("#tbody-status", info["Status"], false); fillTable("#tbody-inputs", info["Inputs"]); fillTable("#tbody-outputs", info["Outputs"]); $("#input-watering").prop("checked", (info["Outputs"]["Watering"]==1) ? true : false); $("#input-thingspeak").prop("checked", (info["Status"]["ThingSpeak"]=="enabled") ? true : false);}}); $.ajax({ url: "/logs", timeout: 500, success: function (log){ $("#textarea-logs").text(log); if ($("#input-scroll").prop("checked")==true){ $("#textarea-logs").scrollTop($("#textarea-logs")[0].scrollHeight);}}});} $(function onReady(){ refresh();}); $("#input-watering").click(function (event){ event.preventDefault(); if (this.checked && confirm("Start watering?")){ $.post("/control",{ "watering": "enable"});}}); $("#input-thingspeak").click(function (event){ event.preventDefault(); if (this.checked){ $.post("/control",{ "thingSpeak": "enable"});} else{ $.post("/control",{ "thingSpeak": "disable"});}}); $("#button-reset").click(function (event){ event.preventDefault(); if (confirm("Reset now?")){ $.post("/control",{ "reset": "1"});}}); </script></body><footer></footer></html>)EOF";

static AsyncWebServer g_webServer(80);

bool g_wifiConnected = false;
bool g_hasNetwork = false;

unsigned
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
handleRoot(AsyncWebServerRequest* request)
{
    digitalWrite(LED_BUILTIN, 1);
    request->send_P(200, "text/html", g_indexHtml);
    digitalWrite(LED_BUILTIN, 0);
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

    String json = "{";

    json += "\"Status\":{";
    json += "\"Hostname\":\"" + String(g_hostname) + "\"";
    strftime(buffer, sizeof(buffer), "%F %T", &timeinfo);
    json += ",\"Date/Time\":\"" + String(buffer) + "\"";
    if (g_bootTime > g_safeTimestamp) {
        snprintf(buffer,
                 sizeof(buffer),
                 "%dd %dh %dm %ds",
                 days,
                 hours % 24,
                 minutes % 60,
                 uptime % 60);
        json += ",\"Uptime\":\"" + String(buffer) + "\"";
    }
    json += ",\"Internet\":\"" +
            String((g_hasInternet) ? "online" : "offline") + "\"";
    json += ",\"Signal Strength\":\"" + String(getSignalStrength()) + "%\"";
    json += ",\"ThingSpeak\":\"" +
            String((g_thingSpeakEnabled) ? "enabled" : "disabled") + "\"";
    json += ",\"Packages Sent\":\"" + String(g_packagesSent) + "\"";
    json += ",\"Watering Cycles\":\"" + String(g_wateringCycles) + "\"";
#ifdef HAS_DHT_SENSOR
    json += ",\"DHT Read Errors\":\"" + String(g_dhtReadErrors) + "\"";
#endif
    json += "},";

    json += "\"Inputs\":{";
    json += "\"Soil Moisture\":\"" + String(g_soilMoisture.getLast()) + "/" +
            String(g_soilMoisture.getAverage()) + "\"";
#ifdef HAS_LUMINOSITY_SENSOR
    json += ",\"Luminosity\":\"" + String(g_luminosity.getLast()) + "/" +
            String(g_luminosity.getAverage()) + "\"";
#endif
#ifdef HAS_DHT_SENSOR
    json += ",\"Temperature\":\"" + String(g_temperature.getLast()) + "\"";
    json += ",\"Air Humidity\":\"" + String(g_airHumidity.getLast()) + "\"";
#endif
    json += "},";

    json += "\"Outputs\":{";
    json += "\"Watering\":\"" + String(g_wateringState) + "\"";
    json += "}";

    json += "}";

    request->send(200, "text/json", json);

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
    request->send(200, "text/html", output);

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
    logger.println("Wifi disconnected. Reconnecting...");
    g_wifiConnected = false;
    g_hasNetwork = false;

    WiFi.begin(g_ssid, g_wifiPassword);
}

void
webSetup()
{
    digitalWrite(LED_BUILTIN, 1);

    WiFi.mode(WIFI_STA);
    WiFi.onEvent(wifiConnected, SYSTEM_EVENT_STA_CONNECTED);
    WiFi.onEvent(wifiGotIP, SYSTEM_EVENT_STA_GOT_IP);
    WiFi.onEvent(wifiDisconnected, SYSTEM_EVENT_STA_DISCONNECTED);
    WiFi.begin(g_ssid, g_wifiPassword);

    if (MDNS.begin(g_hostname) == false) {
        logger.println("Error starting mDNS!");
    }

    g_webServer.on("/", HTTP_GET, handleRoot);
    g_webServer.on("/data.json", HTTP_GET, handleDataJson);
    g_webServer.on("/control", HTTP_POST, handleControl);
    g_webServer.on("/logs", HTTP_GET, handleLogs);

    AsyncElegantOTA.begin(&g_webServer, g_otaUser, g_otaPassword);

    g_webServer.begin();

    digitalWrite(LED_BUILTIN, 0);

    logger.println("Web setup done!");
}

void
webLoop()
{
    // AsyncElegantOTA.loop();
}