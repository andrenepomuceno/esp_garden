#include "accumulator.h"
#include "secret.h"
#include "tasks.h"
#include <AsyncElegantOTA.h>
#include <AsyncTCP.h>
#include <ESP32Ping.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <time.h>

static const char indexHtml[] PROGMEM =
  R"EOF(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><link href="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/css/bootstrap.min.css" rel="stylesheet" integrity="sha384-eOJMYsd53ii+scO/bJGFsiCZc+5NDVN2yr8+0RDqr0Ql0h+rP48ckxlpbzKgwra6" crossorigin="anonymous"><title>ESP Garden</title></head><body><div class="container"><h1>&#x1F331; ESP Garden</h1><h2>Status</h2><table class="table table-striped"><tbody id="tbody-status"></tbody></table><h2>Inputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-inputs"></tbody></table><h2>Outputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-outputs"></tbody></table><script src="https://code.jquery.com/jquery-3.6.0.min.js" integrity="sha256-/xUj+3OJU5yExlq6GSYGSHk7tPXikynS7ogEvDej/m4=" crossorigin="anonymous"></script><script src="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/js/bootstrap.bundle.min.js" integrity="sha384-JEW9xMcG8R+pH31jmWH6WWP0WintQrMb4s7ZOdauHnUtxwoG2vI5DkLtS3qm9Ekf" crossorigin="anonymous"></script><script>function fillTable(id, data, index=true){ var tbody=$(id); tbody.empty(); for (var i=0; i < data.length; i++){ var tr=$('<tr/>'); if (index) tr.append($('<th/>').html(i).attr("scope", "row")); for (var key in data[i]){ tr.append($('<td/>').html(key)); tr.append($('<td/>').html(data[i][key]));} tbody.append(tr);}} function refresh(){ setTimeout(refresh, 1 * 1000); $.ajax({ dataType: "json", url: "/data.json", timeout: 500, success: function (info){ fillTable("#tbody-status", info["Status"], false); fillTable("#tbody-inputs", info["Inputs"]); fillTable("#tbody-outputs", info["Outputs"]);}});} $(function onReady(){ refresh();}); </script></body><footer></footer></html>)EOF";

static AsyncWebServer g_webServer(80);

void
handleRoot(AsyncWebServerRequest* request)
{
  digitalWrite(LED_BUILTIN, 1);
  request->send_P(200, "text/html", indexHtml);
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
    }
  }

  request->send(200);

  digitalWrite(LED_BUILTIN, 0);
}

void
setup(void)
{
  Serial.begin(115200);
  Serial.println("");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, 0);

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

  tasksSetup();

  Serial.println("Setup done!");
}

void
loop(void)
{
  tasksLoop();
  AsyncElegantOTA.loop();
}
