#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ThingSpeak.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secret.h"

const int led = 2;
const int io0 = 0;
const int io23 = 23;
const char indexHtml[] PROGMEM = R"EOF(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><link href="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/css/bootstrap.min.css" rel="stylesheet" integrity="sha384-eOJMYsd53ii+scO/bJGFsiCZc+5NDVN2yr8+0RDqr0Ql0h+rP48ckxlpbzKgwra6" crossorigin="anonymous"><title>ESP Garden</title></head><body><div class="container"><h1>ESP Garden</h1><h2>Status</h2><p id="p-date"></p><p id="p-time"></p><p id="p-uptime"></p><h2>Inputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-inputs"></tbody></table><h2>Outputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-outputs"></tbody></table></div><script src="https://code.jquery.com/jquery-3.6.0.min.js" integrity="sha256-/xUj+3OJU5yExlq6GSYGSHk7tPXikynS7ogEvDej/m4=" crossorigin="anonymous"></script><script src="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/js/bootstrap.bundle.min.js" integrity="sha384-JEW9xMcG8R+pH31jmWH6WWP0WintQrMb4s7ZOdauHnUtxwoG2vI5DkLtS3qm9Ekf" crossorigin="anonymous"></script><script>function fillTable(id, data){ var tbody=$(id); tbody.empty(); for (var i=0; i < data.length; i++){ var tr=$('<tr/>'); tr.append($('<th/>').html(i).attr("scope", "row")); for (var key in data[i]){ tr.append($('<td/>').html(key)); tr.append($('<td/>').html(data[i][key]));} tbody.append(tr);}} function refresh(){ setTimeout(refresh, 1 * 1000); /*var info={ "Date": Date(Date.now()), "Time": "time", "Uptime": "uptime", "Inputs": [ { "a0": (Math.random() * 1024).toFixed()}, { "a1": (Math.random() * 1024).toFixed()}, { "a2": (Math.random() * 1024).toFixed()} ], "Outputs": [ { "gpio0": "0"}, { "gpio1": "1"}, { "gpio2": "2"} ]};*/ $.getJSON("/data.json", function (info){ $("#p-date").text("Date: " + info["Date"]); $("#p-time").text("Time: " + info["Time"]); $("#p-uptime").text("Uptime: " + info["Uptime"]); fillTable("#tbody-inputs", info["Inputs"]); fillTable("#tbody-outputs", info["Outputs"]);});} $(function onReady(){ refresh();}); </script></body><footer></footer></html>
)EOF";
time_t bootTime = 0;

WebServer server(80);
WiFiClient client;

void handleRoot()
{
  digitalWrite(led, 1);
  server.send(200, "text/html", indexHtml);
  digitalWrite(led, 0);
}

void handleDataJson()
{
  digitalWrite(led, 1);
  
  StaticJsonDocument<256> doc;
  char str[256];

  doc["Date"] = "date";
  doc["Time"] = "time";
  doc["Uptime"] = "uptime";

  time_t now = time(NULL);
  time_t uptime = now - bootTime;
  int minutes = uptime/60;
  int hours = minutes/60;
  snprintf(str, sizeof(str), "%02d:%02d:%02d", hours, minutes%60, uptime%60);
  doc["Uptime"] = str;
  
  struct tm timeinfo;
  if(getLocalTime(&timeinfo) != 0)
  {
    strftime(str, sizeof(str), "%Y/%m/%d", &timeinfo);
    doc["Date"] = str;
    strftime(str, sizeof(str), "%H:%M:%S", &timeinfo);
    doc["Time"] = str;
  }

  JsonObject inputs0 = doc["Inputs"].createNestedObject();
  inputs0["A0"] = analogRead(A0);
  JsonObject inputs1 = doc["Inputs"].createNestedObject();
  inputs1["A3"] = analogRead(A3);
  JsonObject inputs2 = doc["Inputs"].createNestedObject();
  inputs2["A6"] = analogRead(A6);
  
  JsonObject outputs0 = doc["Outputs"].createNestedObject();
  outputs0["io23"] = digitalRead(io23);
  
  serializeJson(doc, str);
  server.send(200, "text/json", str);

  digitalWrite(led, 0);
}

void handleSet()
{
  digitalWrite(led, 1);
  
  server.send(200, "text/html", "");

  digitalWrite(led, 0);
}

void setup(void)
{
  pinMode(led, OUTPUT);
  digitalWrite(led, 0);
  
  pinMode(io23, OUTPUT);
  digitalWrite(io23, 0);
  
  pinMode(io0, INPUT);
  
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

  configTime(0, -3*60*60, "pool.ntp.org");
  delay(1000);
  
  bootTime = time(NULL);
  
  ThingSpeak.begin(client);
}

int i = 0;
int s = 0;

void loop(void)
{ 
  server.handleClient();

  if ((i++ % 100) == 0) {
    if (s == 0) {
      s = 1;
    } else {
      s = 0;
    }
    digitalWrite(io23, s);
  }

  delay(10);
}
