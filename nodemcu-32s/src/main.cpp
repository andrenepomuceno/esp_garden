#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <ESPmDNS.h>
#include <ThingSpeak.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <time.h>

#include "secret.h"

// TODO: ota update, circular buffer

static const char indexHtml[] PROGMEM =
  R"EOF(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><link href="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/css/bootstrap.min.css" rel="stylesheet" integrity="sha384-eOJMYsd53ii+scO/bJGFsiCZc+5NDVN2yr8+0RDqr0Ql0h+rP48ckxlpbzKgwra6" crossorigin="anonymous"><title>ESP Garden</title></head><body><div class="container"><h1>ESP Garden</h1><h2>Status</h2><table class="table table-striped"><tbody id="tbody-status"></tbody></table><h2>Inputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-inputs"></tbody></table><h2>Outputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-outputs"></tbody></table><script src="https://code.jquery.com/jquery-3.6.0.min.js" integrity="sha256-/xUj+3OJU5yExlq6GSYGSHk7tPXikynS7ogEvDej/m4=" crossorigin="anonymous"></script><script src="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/js/bootstrap.bundle.min.js" integrity="sha384-JEW9xMcG8R+pH31jmWH6WWP0WintQrMb4s7ZOdauHnUtxwoG2vI5DkLtS3qm9Ekf" crossorigin="anonymous"></script><script>function fillTable(id, data, index=true){ var tbody=$(id); tbody.empty(); for (var i=0; i < data.length; i++){ var tr=$('<tr/>'); if (index) tr.append($('<th/>').html(i).attr("scope", "row")); for (var key in data[i]){ tr.append($('<td/>').html(key)); tr.append($('<td/>').html(data[i][key]));} tbody.append(tr);}} function refresh(){ setTimeout(refresh, 1 * 1000); $.ajax({ dataType: "json", url: "/data.json", timeout: 500, success: function (info){ fillTable("#tbody-status", info["Status"], false); fillTable("#tbody-inputs", info["Inputs"]); fillTable("#tbody-outputs", info["Outputs"]);}});} $(function onReady(){ refresh();}); </script></body><footer></footer></html>
  )EOF";

static const unsigned ADC_READ_SIZE = 2;
static const unsigned A0_INDEX = 0;
static const unsigned A3_INDEX = 1;

static const unsigned GPIO_READ_SIZE = 1;
static const unsigned GPIO0_INDEX = 0;

static const unsigned MAX_RETRIES = 3;
static const unsigned RETRY_DELAY = 5000;

static WebServer server(80);
static WiFiClient client;

static time_t bootTime = 0;
static unsigned packagesSent = 0;
static unsigned tsErrors = 0;
static time_t tsLastError = 0;
static int tsLastCode = 200;
static uint16_t adcRead[ADC_READ_SIZE];
static uint8_t gpioRead[GPIO_READ_SIZE];

static DHT_Unified dht(23, DHT11);
static float temperature = 0.0;
static float airHumidity = 0.0;

void
syncClock()
{
  Serial.println("Updating clock...");
  configTime(0, -3 * 60 * 60, "pool.ntp.org");
  delay(2000);
}

void
handleRoot()
{
  digitalWrite(LED_BUILTIN, 1);
  server.send(200, "text/html", indexHtml);
  digitalWrite(LED_BUILTIN, 0);
}

void
handleDataJson()
{
  digitalWrite(LED_BUILTIN, 1);

  struct tm timeinfo;
  getLocalTime(&timeinfo);
  time_t now = mktime(&timeinfo);
  int uptime = now - bootTime;
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
  json += "{\"PackagesSent\":\"" + String(packagesSent) + "\"}";
  if (tsErrors > 0) {
    json += ",{\"Errors\":\"" + String(tsErrors) + "\"}";
    strftime(buffer, sizeof(buffer), "%T", localtime(&tsLastError));
    json += ",{\"LastError\":\"" + String(buffer) + "\"}";
    json += ",{\"LastCode\":\"" + String(tsLastCode) + "\"}";
  }
  json += "],";

  json += "\"Inputs\":[";
  json += "{\"A0\":\"" + String(adcRead[A0_INDEX]) + "\"},";
  json += "{\"A3\":\"" + String(adcRead[A3_INDEX]) + "\"},";
  json += "{\"Temperature\":\"" + String(temperature) + "\"},";
  json += "{\"AirHumidity\":\"" + String(airHumidity) + "\"},";
  json += "{\"GPIO0\":\"" + String(gpioRead[GPIO0_INDEX]) + "\"}";
  json += "]";

  /*json += "\"Outputs\":[";
  json += "{\"GPIO23\":\"" + String(gpioRead[GPIO23_INDEX]) + "\"}";
  json += "]";*/

  json += "}";

  server.send(200, "text/json", json);

  digitalWrite(LED_BUILTIN, 0);
}

void
handleSet()
{
  digitalWrite(LED_BUILTIN, 1);

  for (int i = 0; i < server.args(); ++i) {
    Serial.println(server.argName(i) + " = " + server.arg(i));
  }

  server.send(200, "text/html", "");

  digitalWrite(LED_BUILTIN, 0);
}

void
serverTask(void*)
{
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void
sensorsTask(void*)
{
  memset(adcRead, 0, sizeof(adcRead));
  memset(gpioRead, 0, sizeof(gpioRead));

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    adcRead[A0_INDEX] = analogRead(A0);
    adcRead[A3_INDEX] = analogRead(A3);

    sensors_event_t event;
    dht.temperature().getEvent(&event);
    if (isnan(event.temperature) == false) {
      temperature = event.temperature;
    }
    dht.humidity().getEvent(&event);
    if (isnan(event.relative_humidity) == false) {
      airHumidity = event.relative_humidity;
    }

    gpioRead[GPIO0_INDEX] = digitalRead(0);
  }
}

void
tsTask(void*)
{
  ThingSpeak.begin(client);
  ThingSpeak.setField(8, bootTime);

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(30 * 1000));

    int retries = 0;
    while (retries < MAX_RETRIES) {
      ThingSpeak.setField(1, adcRead[A0_INDEX]);
      ThingSpeak.setField(2, adcRead[A3_INDEX]);

      ThingSpeak.setField(6, temperature);
      ThingSpeak.setField(7, airHumidity);

      digitalWrite(LED_BUILTIN, 1);
      int status = ThingSpeak.writeFields(channelNumber, apiKey);
      digitalWrite(LED_BUILTIN, 0);

      if (status == 200) {
        ++packagesSent;
        break;
      }

      ++retries;
      if (retries >= MAX_RETRIES) {
        ++tsErrors;
        tsLastError = time(NULL);
        tsLastCode = status;
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

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, 0);

  pinMode(0, INPUT);

  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin("esp32")) {
    Serial.println("MDNS responder started");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data.json", HTTP_GET, handleDataJson);
  server.on("/set", HTTP_POST, handleSet);
  server.begin();
  Serial.println("HTTP server started");

  syncClock();
  bootTime = time(NULL);

  xTaskCreate(sensorsTask, "sensorsTask", 1024, NULL, 2, NULL);
  xTaskCreate(serverTask, "serverTask", 4 * 1024, NULL, 2, NULL);
  xTaskCreate(tsTask, "tsTask", 4 * 1024, NULL, 2, NULL);
  xTaskCreate(clockUpdateTask, "clockUpdateTask", 4 * 1024, NULL, 1, NULL);
}

void
loop(void)
{}
