#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ThingSpeak.h>
#include <time.h>
#include "secret.h"

const int BOARD_LED = 2;
const int GPIO0 = 0;
const int GPIO23 = 23;
const char indexHtml[] PROGMEM = R"EOF(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><link href="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/css/bootstrap.min.css" rel="stylesheet" integrity="sha384-eOJMYsd53ii+scO/bJGFsiCZc+5NDVN2yr8+0RDqr0Ql0h+rP48ckxlpbzKgwra6" crossorigin="anonymous"><title>ESP Garden</title></head><body><div class="container"><h1>ESP Garden</h1><h2>Status</h2><p id="p-date"></p><p id="p-time"></p><p id="p-uptime"></p><h2>Inputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-inputs"></tbody></table><h2>Outputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-outputs"></tbody></table></div><script src="https://code.jquery.com/jquery-3.6.0.min.js" integrity="sha256-/xUj+3OJU5yExlq6GSYGSHk7tPXikynS7ogEvDej/m4=" crossorigin="anonymous"></script><script src="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/js/bootstrap.bundle.min.js" integrity="sha384-JEW9xMcG8R+pH31jmWH6WWP0WintQrMb4s7ZOdauHnUtxwoG2vI5DkLtS3qm9Ekf" crossorigin="anonymous"></script><script>function fillTable(id, data){ var tbody=$(id); tbody.empty(); for (var i=0; i < data.length; i++){ var tr=$('<tr/>'); tr.append($('<th/>').html(i).attr("scope", "row")); for (var key in data[i]){ tr.append($('<td/>').html(key)); tr.append($('<td/>').html(data[i][key]));} tbody.append(tr);}} function refresh(){ setTimeout(refresh, 1 * 1000); /*var info={ "Date": Date(Date.now()), "Time": "time", "Uptime": "uptime", "Inputs": [ { "a0": (Math.random() * 1024).toFixed()}, { "a1": (Math.random() * 1024).toFixed()}, { "a2": (Math.random() * 1024).toFixed()} ], "Outputs": [ { "gpio0": "0"}, { "gpio1": "1"}, { "gpio2": "2"} ]};*/ $.getJSON("/data.json", function (info){ $("#p-date").text("Date: " + info["Date"]); $("#p-time").text("Time: " + info["Time"]); $("#p-uptime").text("Uptime: " + info["Uptime"]); fillTable("#tbody-inputs", info["Inputs"]); fillTable("#tbody-outputs", info["Outputs"]);});} $(function onReady(){ refresh();}); </script></body><footer></footer></html>
)EOF";
time_t bootTime = 0;

WebServer server(80);
WiFiClient client;

const int ADC_MAX = 8;
int adcRead[ADC_MAX];
const int MAX_GPIO = 32;
uint8_t gpioRead[MAX_GPIO];

void handleRoot()
{
  digitalWrite(BOARD_LED, 1);
  server.send(200, "text/html", indexHtml);
  digitalWrite(BOARD_LED, 0);
}

void handleDataJson()
{
  digitalWrite(BOARD_LED, 1);

  String json = "{";

  char str[64];
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  strftime(str, sizeof(str), "%Y/%m/%d", &timeinfo);
  json += "\"Date\":\"" + String(str) + "\",";

  strftime(str, sizeof(str), "%H:%M:%S", &timeinfo);
  json += "\"Time\":\"" + String(str) + "\",";

  time_t now = time(NULL);
  time_t uptime = now - bootTime;
  int minutes = uptime / 60;
  int hours = minutes / 60;
  snprintf(str, sizeof(str), "%02d:%02d:%02d", hours, minutes % 60, uptime % 60);
  json += "\"Uptime\":\"" + String(str) + "\",";

  json += "\"Inputs\":[";
  json += "{\"A0\":\"" + String(adcRead[0]) + "\"},";
  json += "{\"A3\":\"" + String(adcRead[3]) + "\"},";
  json += "{\"A6\":\"" + String(adcRead[6]) + "\"},";
  json += "{\"GPIO0\":\"" + String(gpioRead[0]) + "\"}";
  json += "],";

  json += "\"Outputs\":[";
  json += "{\"GPIO23\":\"" + String(gpioRead[23]) + "\"}";
  json += "]";

  json += "}";

  server.send(200, "text/json", json);

  digitalWrite(BOARD_LED, 0);
}

void handleSet()
{
  digitalWrite(BOARD_LED, 1);

  for (int i = 0; i < server.args(); ++i)
  {
    Serial.println(server.argName(i) + " = " + server.arg(i));
  }

  server.send(200, "text/html", "");

  digitalWrite(BOARD_LED, 0);
}

void serverTask(void *param)
{
  for (;;)
  {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void sensorsTask(void *param)
{
  memset(adcRead, 0, sizeof(adcRead));
  memset(gpioRead, 0, sizeof(gpioRead));

  for (;;)
  {
    adcRead[0] = analogRead(A0);
    adcRead[3] = analogRead(A3);
    adcRead[6] = analogRead(A6);

    gpioRead[0] = digitalRead(0);
    gpioRead[23] = digitalRead(23);

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void tsTask(void *)
{
  ThingSpeak.begin(client);

  for (;;) {
    vTaskDelay(30 * pdMS_TO_TICKS(1000));

    ThingSpeak.setField(1, adcRead[0]);
    ThingSpeak.setField(2, adcRead[3]);
    ThingSpeak.writeFields(channelNumber, apiKey);
  }
}

void setup(void)
{
  pinMode(BOARD_LED, OUTPUT);
  digitalWrite(BOARD_LED, 0);

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
  xTaskCreate(serverTask, "serverTask", 16 * 1024, NULL, 2, NULL);
  xTaskCreate(tsTask, "tsTask", 16*1024, NULL, 2, NULL);
}

void loop(void)
{
}