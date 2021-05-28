#include "accumulator.h"
#include "html.h"
#include "secret.h"
#include "tasks.h"
#include <AsyncElegantOTA.h>
#include <AsyncTCP.h>
#include <ESP32Ping.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <time.h>

static AsyncWebServer g_webServer(80);

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

    json += "\"Status\":[";
    strftime(buffer, sizeof(buffer), "%F", &timeinfo);
    json += "{\"Date\":\"" + String(buffer) + "\"},";
    strftime(buffer, sizeof(buffer), "%T", &timeinfo);
    json += "{\"Time\":\"" + String(buffer) + "\"},";
    snprintf(buffer,
             sizeof(buffer),
             "%dd %dh %dm %ds",
             days,
             hours % 24,
             minutes % 60,
             uptime % 60);
    json += "{\"Uptime\":\"" + String(buffer) + "\"},";
    json += "{\"Packages Sent\":\"" + String(g_packagesSent) + "\"},";
    json += "{\"DHT Read Errors\":\"" + String(g_dhtReadErrors) + "\"}";
    if (g_tsErrors > 0) {
        json += ",{\"Errors\":\"" + String(g_tsErrors) + "\"}";
        strftime(buffer, sizeof(buffer), "%T", localtime(&g_tsLastError));
        json += ",{\"Last Error\":\"" + String(buffer) + "\"}";
        json += ",{\"Last Code\":\"" + String(g_tsLastCode) + "\"}";
    }
    json += "],";

    json += "\"Inputs\":[";
    json += "{\"Soil Moisture\":\"" + String(g_soilMoisture.getLast()) + "/" +
            String(g_soilMoisture.getAverage()) + "\"},";
    json += "{\"Luminosity\":\"" + String(g_luminosity.getLast()) + "/" +
            String(g_luminosity.getAverage()) + "\"},";
    json += "{\"Temperature\":\"" + String(g_temperature.getLast()) + "\"},";
    json += "{\"Air Humidity\":\"" + String(g_airHumidity.getLast()) + "\"},";
    json += "{\"Button State\":\"" + String(g_buttonState) + "\"}";
    json += "],";

    json += "\"Outputs\":[";
    json += "{\"Watering\":\"" + String(g_wateringState) + "\"}";
    json += "]";

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
        } else if ((param->name() == "teamSpeak") &&
                   (param->value() == "toogle")) {
            // TODO
        }
    }

    request->send(200);

    digitalWrite(LED_BUILTIN, 0);
}

void
webSetup()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_ssid, g_password);

    digitalWrite(LED_BUILTIN, 1);
    Serial.print("Connecting to wifi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.print(".");
    }
    Serial.println("");
    digitalWrite(LED_BUILTIN, 0);

    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("esp32") == false) {
        Serial.println("Error starting mDNS");
    }

    g_webServer.on("/", HTTP_GET, handleRoot);
    g_webServer.on("/data.json", HTTP_GET, handleDataJson);
    g_webServer.on("/control", HTTP_POST, handleControl);

    AsyncElegantOTA.begin(&g_webServer, g_otaUser, g_otaPassword);
    g_webServer.begin();

    digitalWrite(LED_BUILTIN, 1);
    Serial.print("Checking internet conection");
    while (Ping.ping(IPAddress(8, 8, 8, 8), 1) == false) {
        delay(1000);
        Serial.print(".");
    }
    Serial.println("");
    digitalWrite(LED_BUILTIN, 0);
}

void
webLoop()
{
    AsyncElegantOTA.loop();
}