#include "accumulator.h"
#include "secret.h"
#include <Adafruit_Sensor.h>
#include <AsyncElegantOTA.h>
#include <AsyncTCP.h>
#include <DHT.h>
#include <DHT_U.h>
#include <ESP32Ping.h>
#include <ESPAsyncWebServer.h>
#include <TaskScheduler.h>
#include <ThingSpeak.h>
#include <WiFi.h>
#include <time.h>

static const char indexHtml[] PROGMEM =
  R"EOF(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><link href="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/css/bootstrap.min.css" rel="stylesheet" integrity="sha384-eOJMYsd53ii+scO/bJGFsiCZc+5NDVN2yr8+0RDqr0Ql0h+rP48ckxlpbzKgwra6" crossorigin="anonymous"><title>ESP Garden</title></head><body><div class="container"><h1>&#x1F331; ESP Garden</h1><h2>Status</h2><table class="table table-striped"><tbody id="tbody-status"></tbody></table><h2>Inputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-inputs"></tbody></table><h2>Outputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-outputs"></tbody></table><script src="https://code.jquery.com/jquery-3.6.0.min.js" integrity="sha256-/xUj+3OJU5yExlq6GSYGSHk7tPXikynS7ogEvDej/m4=" crossorigin="anonymous"></script><script src="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/js/bootstrap.bundle.min.js" integrity="sha384-JEW9xMcG8R+pH31jmWH6WWP0WintQrMb4s7ZOdauHnUtxwoG2vI5DkLtS3qm9Ekf" crossorigin="anonymous"></script><script>function fillTable(id, data, index=true){ var tbody=$(id); tbody.empty(); for (var i=0; i < data.length; i++){ var tr=$('<tr/>'); if (index) tr.append($('<th/>').html(i).attr("scope", "row")); for (var key in data[i]){ tr.append($('<td/>').html(key)); tr.append($('<td/>').html(data[i][key]));} tbody.append(tr);}} function refresh(){ setTimeout(refresh, 1 * 1000); $.ajax({ dataType: "json", url: "/data.json", timeout: 500, success: function (info){ fillTable("#tbody-status", info["Status"], false); fillTable("#tbody-inputs", info["Inputs"]); fillTable("#tbody-outputs", info["Outputs"]);}});} $(function onReady(){ refresh();}); </script></body><footer></footer></html>)EOF";

Accumulator g_soilMoisture;
Accumulator g_luminosity;
Accumulator g_temperature;
Accumulator g_airHumidity;

static const unsigned g_buttonGPIO = 0;
static bool g_buttonState = false;

static const unsigned g_wateringGPIO = 22;
static bool g_wateringState = false;

static AsyncWebServer g_webServer(80);
static WiFiClient g_wifiClient;

static time_t g_bootTime = 0;
static unsigned g_packagesSent = 0;

static unsigned g_tsErrors = 0;
static time_t g_tsLastError = 0;
static int g_tsLastCode = 200;

static const unsigned g_dhtPin = 23;
static DHT_Unified g_dht(g_dhtPin, DHT11);
static unsigned g_dhtReadErrors = 0;

void
ioTaskHandler();
void
dhtTaskHandler();
void
tsTaskHandler();
void
syncClock();
void
wateringTaskHandler();

static const unsigned g_tsTaskPeriod = 60 * 1000;
static const unsigned g_clockUpdateTaskPeriod = 24 * 60 * 60 * 1000;
static const unsigned g_dhtTaskPeriod = 5 * 1000;
static const unsigned g_ioTaskPeriod = 1000;
static const unsigned g_wateringTaskPeriod = 1000;
static const unsigned g_wateringIterations = 10;

static Scheduler g_taskScheduler;
static Task g_ioTask(g_ioTaskPeriod,
                     TASK_FOREVER,
                     &ioTaskHandler,
                     &g_taskScheduler);
static Task g_dhtTask(g_dhtTaskPeriod,
                      TASK_FOREVER,
                      &dhtTaskHandler,
                      &g_taskScheduler);
static Task g_tsTask(g_tsTaskPeriod,
                     TASK_FOREVER,
                     &tsTaskHandler,
                     &g_taskScheduler);
static Task g_clockUpdateTask(g_clockUpdateTaskPeriod,
                              TASK_FOREVER,
                              &syncClock,
                              &g_taskScheduler);
static Task g_wateringTask(g_wateringTaskPeriod,
                           TASK_FOREVER,
                           &wateringTaskHandler,
                           &g_taskScheduler);

void
syncClock()
{
  int tzOffset = -3 * 60 * 60;
  configTime(
    0, tzOffset, "0.br.pool.ntp.org", "1.br.pool.ntp.org", "2.br.pool.ntp.org");
}

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
  json += "{\"Soil Moisture\":\"" + String(g_soilMoisture.getLast()) +
          "/" + String(g_soilMoisture.getAverage()) + "\"},";
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
      g_wateringTask.enable();
    }
  }

  request->send(200);

  digitalWrite(LED_BUILTIN, 0);
}

void
ioTaskHandler()
{
  g_soilMoisture.add(analogRead(A0));
  g_luminosity.add(analogRead(A3));

  g_buttonState = (digitalRead(g_buttonGPIO) > 0) ? (false) : (true);
  g_wateringState = (digitalRead(g_wateringGPIO) > 0) ? (true) : (false);
}

void
dhtTaskHandler()
{
  sensors_event_t event;
  g_dht.temperature().getEvent(&event);
  if (isnan(event.temperature) == false) {
    g_temperature.add(event.temperature);
  } else {
    ++g_dhtReadErrors;
  }
  g_dht.humidity().getEvent(&event);
  if (isnan(event.relative_humidity) == false) {
    g_airHumidity.add(event.relative_humidity);
  } else {
    ++g_dhtReadErrors;
  }
}

void
tsTaskHandler()
{
  ThingSpeak.setField(1, g_soilMoisture.getAverage());
  ThingSpeak.setField(2, g_luminosity.getAverage());

  ThingSpeak.setField(6, g_temperature.getAverage());
  ThingSpeak.setField(7, g_airHumidity.getAverage());

  digitalWrite(LED_BUILTIN, 1);
  int status = ThingSpeak.writeFields(g_channelNumber, g_apiKey);
  digitalWrite(LED_BUILTIN, 0);

  if (status == 200) {
    ++g_packagesSent;
  } else {
    ++g_tsErrors;
    g_tsLastError = time(NULL);
    g_tsLastCode = status;
  }

  g_soilMoisture.resetAverage();
  g_luminosity.resetAverage();
  g_temperature.resetAverage();
  g_airHumidity.resetAverage();
}

void
wateringTaskHandler()
{
  auto runs = g_wateringTask.getRunCounter();

  if (runs == 1) {
    digitalWrite(g_wateringGPIO, 1);
  } else if (runs > g_wateringIterations) {
    digitalWrite(g_wateringGPIO, 0);
    g_wateringTask.disable();
  }
}

void
setup(void)
{
  Serial.begin(115200);
  Serial.println("");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, 0);
  pinMode(g_wateringGPIO, OUTPUT);
  digitalWrite(g_wateringGPIO, 0);

  pinMode(g_buttonGPIO, INPUT);

  g_dht.begin();

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

  Serial.println("Updating clock...");
  syncClock();
  delay(2000);
  g_bootTime = time(NULL);

  ThingSpeak.begin(g_wifiClient);
  ThingSpeak.setField(8, g_bootTime);

  g_ioTask.enable();
  g_dhtTask.enable();
  g_tsTask.enableDelayed(g_tsTaskPeriod);
  g_clockUpdateTask.enableDelayed(g_clockUpdateTaskPeriod);

  Serial.println("Setup done!");
}

void
loop(void)
{
  g_taskScheduler.execute();
  AsyncElegantOTA.loop();
}
