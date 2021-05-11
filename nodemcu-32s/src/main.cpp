#include "secret.h"
#include <Adafruit_Sensor.h>
#include <AsyncElegantOTA.h>
#include <AsyncTCP.h>
#include <DHT.h>
#include <DHT_U.h>
#include <ESP32Ping.h>
#include <ESPAsyncWebServer.h>
#include <ThingSpeak.h>
#include <WiFi.h>
#include <time.h>

// TODO: circular buffer

static const char indexHtml[] PROGMEM =
  R"EOF(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><link href="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/css/bootstrap.min.css" rel="stylesheet" integrity="sha384-eOJMYsd53ii+scO/bJGFsiCZc+5NDVN2yr8+0RDqr0Ql0h+rP48ckxlpbzKgwra6" crossorigin="anonymous"><title>ESP Garden</title></head><body><div class="container"><h1>&#x1F331; ESP Garden</h1><h2>Status</h2><table class="table table-striped"><tbody id="tbody-status"></tbody></table><h2>Inputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-inputs"></tbody></table><h2>Outputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-outputs"></tbody></table><script src="https://code.jquery.com/jquery-3.6.0.min.js" integrity="sha256-/xUj+3OJU5yExlq6GSYGSHk7tPXikynS7ogEvDej/m4=" crossorigin="anonymous"></script><script src="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/js/bootstrap.bundle.min.js" integrity="sha384-JEW9xMcG8R+pH31jmWH6WWP0WintQrMb4s7ZOdauHnUtxwoG2vI5DkLtS3qm9Ekf" crossorigin="anonymous"></script><script>function fillTable(id, data, index=true){ var tbody=$(id); tbody.empty(); for (var i=0; i < data.length; i++){ var tr=$('<tr/>'); if (index) tr.append($('<th/>').html(i).attr("scope", "row")); for (var key in data[i]){ tr.append($('<td/>').html(key)); tr.append($('<td/>').html(data[i][key]));} tbody.append(tr);}} function refresh(){ setTimeout(refresh, 1 * 1000); $.ajax({ dataType: "json", url: "/data.json", timeout: 500, success: function (info){ fillTable("#tbody-status", info["Status"], false); fillTable("#tbody-inputs", info["Inputs"]); fillTable("#tbody-outputs", info["Outputs"]);}});} $(function onReady(){ refresh();}); </script></body><footer></footer></html>)EOF";

static const unsigned ADC_READ_SIZE = 2;
static const unsigned A0_INDEX = 0;
static const unsigned A3_INDEX = 1;

static const unsigned GPIO_READ_SIZE = 1;
static const unsigned GPIO0_INDEX = 0;

static const unsigned MAX_RETRIES = 3;
static const unsigned RETRY_DELAY = 5000;

static AsyncWebServer g_webServer(80);
static WiFiClient g_wifiClient;

static time_t g_bootTime = 0;
static unsigned g_packagesSent = 0;

static unsigned g_tsErrors = 0;
static time_t g_tsLastError = 0;
static int g_tsLastCode = 200;

static uint16_t g_adcRead[ADC_READ_SIZE];
static uint8_t g_gpioRead[GPIO_READ_SIZE];

static const unsigned g_dhtPin = 23;
static DHT_Unified g_dht(g_dhtPin, DHT11);
static float g_temperature = 0.0;
static float g_airHumidity = 0.0;

void
syncClock()
{
  configTime(0, -3 * 60 * 60, "pool.ntp.org");
  delay(2000);
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
  json += "{\"PackagesSent\":\"" + String(g_packagesSent) + "\"}";
  if (g_tsErrors > 0) {
    json += ",{\"Errors\":\"" + String(g_tsErrors) + "\"}";
    strftime(buffer, sizeof(buffer), "%T", localtime(&g_tsLastError));
    json += ",{\"LastError\":\"" + String(buffer) + "\"}";
    json += ",{\"LastCode\":\"" + String(g_tsLastCode) + "\"}";
  }
  json += "],";

  json += "\"Inputs\":[";
  json += "{\"A0\":\"" + String(g_adcRead[A0_INDEX]) + "\"},";
  json += "{\"A3\":\"" + String(g_adcRead[A3_INDEX]) + "\"},";
  json += "{\"Temperature\":\"" + String(g_temperature) + "\"},";
  json += "{\"AirHumidity\":\"" + String(g_airHumidity) + "\"},";
  json += "{\"GPIO0\":\"" + String(g_gpioRead[GPIO0_INDEX]) + "\"}";
  json += "],";

  json += "\"Outputs\":[";
  // json += "{\"GPIO23\":\"" + String(g_gpioRead[GPIO23_INDEX]) + "\"}";
  json += "]";

  json += "}";

  request->send(200, "text/json", json);

  digitalWrite(LED_BUILTIN, 0);
}

void
handleSet(AsyncWebServerRequest* request)
{
  digitalWrite(LED_BUILTIN, 1);

  for (int i = 0; i < request->params(); ++i) {
    AsyncWebParameter* param = request->getParam(i);
    Serial.println(param->name() + " = " + param->value());
  }

  request->send(200);

  digitalWrite(LED_BUILTIN, 0);
}

void
sensorsTask(void*)
{
  unsigned count = 0;

  memset(g_adcRead, 0, sizeof(g_adcRead));
  memset(g_gpioRead, 0, sizeof(g_gpioRead));

  g_temperature = 0.0;
  g_airHumidity = 0.0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    g_adcRead[A0_INDEX] = analogRead(A0);
    g_adcRead[A3_INDEX] = analogRead(A3);

    if ((count % 5) == 0) {
      sensors_event_t event;
      g_dht.temperature().getEvent(&event);
      if (isnan(event.temperature) == false) {
        g_temperature = event.temperature;
      }
      g_dht.humidity().getEvent(&event);
      if (isnan(event.relative_humidity) == false) {
        g_airHumidity = event.relative_humidity;
      }
    }

    g_gpioRead[GPIO0_INDEX] = digitalRead(0);

    ++count;
  }
}

void
tsTask(void*)
{
  ThingSpeak.begin(g_wifiClient);
  ThingSpeak.setField(8, g_bootTime);

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(60 * 1000));

    int retries = 0;
    while (retries < MAX_RETRIES) {
      ThingSpeak.setField(1, g_adcRead[A0_INDEX]);
      ThingSpeak.setField(2, g_adcRead[A3_INDEX]);

      ThingSpeak.setField(6, g_temperature);
      ThingSpeak.setField(7, g_airHumidity);

      digitalWrite(LED_BUILTIN, 1);
      int status = ThingSpeak.writeFields(g_channelNumber, g_apiKey);
      digitalWrite(LED_BUILTIN, 0);

      if (status == 200) {
        ++g_packagesSent;
        break;
      }

      ++retries;
      if (retries >= MAX_RETRIES) {
        ++g_tsErrors;
        g_tsLastError = time(NULL);
        g_tsLastCode = status;
        break;
      }

      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY));
    }
  }
}

void
clockUpdateTask(void*)
{
  int minutes = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(60 * 1000));

    ++minutes;
    if (minutes >= 24 * 60) {
      minutes = 0;
      syncClock();
    }
  }
}

void
setup(void)
{
  Serial.begin(115200);
  Serial.println("");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, 0);

  pinMode(0, INPUT);

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
  g_webServer.on("/set", HTTP_POST, handleSet);

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
  g_bootTime = time(NULL);

  xTaskCreate(sensorsTask, "sensorsTask", 2 * 1024, NULL, 2, NULL);
  xTaskCreate(tsTask, "tsTask", 4 * 1024, NULL, 2, NULL);
  xTaskCreate(clockUpdateTask, "clockUpdateTask", 4 * 1024, NULL, 1, NULL);

  Serial.println("Setup done!");
}

void
loop(void)
{
  AsyncElegantOTA.loop();
}
