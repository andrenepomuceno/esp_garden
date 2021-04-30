#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ThingSpeak.h>
#include <time.h>
#include "secret.h"

const char indexHtml[] PROGMEM = R"EOF(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><link href="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/css/bootstrap.min.css" rel="stylesheet" integrity="sha384-eOJMYsd53ii+scO/bJGFsiCZc+5NDVN2yr8+0RDqr0Ql0h+rP48ckxlpbzKgwra6" crossorigin="anonymous"><title>ESP Garden</title></head><body><div class="container"><h1>ESP Garden</h1><h2>Status</h2><table class="table table-striped"><tbody id="tbody-status"></tbody></table><h2>Inputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-inputs"></tbody></table><h2>Outputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-outputs"></tbody></table></div><script src="https://code.jquery.com/jquery-3.6.0.min.js" integrity="sha256-/xUj+3OJU5yExlq6GSYGSHk7tPXikynS7ogEvDej/m4=" crossorigin="anonymous"></script><script src="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/js/bootstrap.bundle.min.js" integrity="sha384-JEW9xMcG8R+pH31jmWH6WWP0WintQrMb4s7ZOdauHnUtxwoG2vI5DkLtS3qm9Ekf" crossorigin="anonymous"></script><script>function fillTable(id, data, index=true){ var tbody=$(id); tbody.empty(); for (var i=0; i < data.length; i++){ var tr=$('<tr/>'); if (index) tr.append($('<th/>').html(i).attr("scope", "row")); for (var key in data[i]){ tr.append($('<td/>').html(key)); tr.append($('<td/>').html(data[i][key]));} tbody.append(tr);}} function refresh(){ setTimeout(refresh, 1 * 1000); var info={ "Status": [ { "Date": Date(Date.now())}, { "Time": "time"}, { "Uptime": "uptime"} ], "Inputs": [ { "a0": (Math.random() * 4096).toFixed()}, { "a1": (Math.random() * 4096).toFixed()}, { "a2": (Math.random() * 4096).toFixed()} ], "Outputs": [ { "gpio0": "0"}, { "gpio1": "1"}, { "gpio2": "2"} ]}; $.getJSON("/data.json", function (info){ fillTable("#tbody-status", info["Status"], false); fillTable("#tbody-inputs", info["Inputs"]); fillTable("#tbody-outputs", info["Outputs"]);});} $(function onReady(){ refresh();}); </script></body><footer></footer></html>
)EOF";

const int GPIO0 = 0;
const int GPIO23 = 23;

const int ADC_READ_SIZE = 4;
const int GPIO_READ_SIZE = 4;

WebServer server(80);
WiFiClient client;

time_t bootTime = 0;
uint16_t adcRead[ADC_READ_SIZE];
uint8_t gpioRead[GPIO_READ_SIZE];
int tsErrors = 0;

void handleRoot()
{
  digitalWrite(LED_BUILTIN, 1);
  server.send(200, "text/html", indexHtml);
  digitalWrite(LED_BUILTIN, 0);
}

void handleDataJson()
{
  digitalWrite(LED_BUILTIN, 1);

  struct tm timeinfo;
  getLocalTime(&timeinfo);
  time_t now = mktime(&timeinfo);
  time_t uptime = now - bootTime;
  int minutes = uptime / 60;
  int hours = minutes / 60;
  int seconds = uptime % 60;

  String json = "{";

  json += "\"Status\":[";
  json += "{\"Date\":\"" + String(timeinfo.tm_year + 1900) + "/" + String(timeinfo.tm_mon + 1) + "/" + String(timeinfo.tm_mday) + "\"},";
  json += "{\"Time\":\"" + String(timeinfo.tm_hour) + ":" + String(timeinfo.tm_min) + ":" + String(timeinfo.tm_sec) + "\"},";
  json += "{\"Uptime\":\"" + String(hours) + ":" + String(minutes) + ":" + String(seconds) + "\"},";
  json += "{\"Errors\":\"" + String(tsErrors) + "\"}";
  json += "],";

  json += "\"Inputs\":[";
  json += "{\"A0\":\"" + String(adcRead[0]) + "\"},";
  json += "{\"A3\":\"" + String(adcRead[1]) + "\"},";
  json += "{\"A6\":\"" + String(adcRead[2]) + "\"},";
  json += "{\"GPIO0\":\"" + String(gpioRead[0]) + "\"}";
  json += "],";

  json += "\"Outputs\":[";
  json += "{\"GPIO23\":\"" + String(gpioRead[1]) + "\"}";
  json += "]";

  json += "}";

  server.send(200, "text/json", json);

  digitalWrite(LED_BUILTIN, 0);
}

void handleSet()
{
  digitalWrite(LED_BUILTIN, 1);

  for (int i = 0; i < server.args(); ++i)
  {
    Serial.println(server.argName(i) + " = " + server.arg(i));
  }

  server.send(200, "text/html", "");

  digitalWrite(LED_BUILTIN, 0);
}

void serverTask(void*)
{
  for (;;)
  {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void sensorsTask(void*)
{
  memset(adcRead, 0, sizeof(adcRead));
  memset(gpioRead, 0, sizeof(gpioRead));

  for (;;)
  {
    adcRead[0] = analogRead(A0);
    adcRead[1] = analogRead(A3);
    adcRead[2] = analogRead(A6);

    gpioRead[0] = digitalRead(0);
    gpioRead[1] = digitalRead(23);

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void tsTask(void*)
{
  ThingSpeak.begin(client);
  ThingSpeak.writeField(channelNumber, 3, bootTime, apiKey);

  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(30 * 1000));

    digitalWrite(LED_BUILTIN, 1);

    ThingSpeak.setField(1, adcRead[0]);
    ThingSpeak.setField(2, adcRead[1]);
    int status = ThingSpeak.writeFields(channelNumber, apiKey);
    if (status != 200)
    {
      ++tsErrors;
      Serial.println("ThingSpeak.writeFields failed with code " + String(status));
    }

    digitalWrite(LED_BUILTIN, 0);
  }
}

void setup(void)
{
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, 0);

  pinMode(GPIO23, OUTPUT);
  digitalWrite(GPIO23, 0);

  pinMode(GPIO0, INPUT);

  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("");
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin("esp32"))
  {
    Serial.println("MDNS responder started");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data.json", HTTP_GET, handleDataJson);
  server.on("/set", HTTP_POST, handleSet);
  server.begin();
  Serial.println("HTTP server started");

  configTime(0, -3 * 60 * 60, "pool.ntp.org");
  delay(1000);
  bootTime = time(NULL);

  xTaskCreate(sensorsTask, "sensorsTask", 1024, NULL, 2, NULL);
  xTaskCreate(serverTask, "serverTask", 4 * 1024, NULL, 2, NULL);
  xTaskCreate(tsTask, "tsTask", 4 * 1024, NULL, 2, NULL);
}

void loop(void)
{
}