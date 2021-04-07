#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ThingSpeak.h>
#include <ArduinoJson.h>
#include "secret.h"

const int led = 2;
const char indexHtml[] PROGMEM = R"EOF(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><link href="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/css/bootstrap.min.css" rel="stylesheet" integrity="sha384-eOJMYsd53ii+scO/bJGFsiCZc+5NDVN2yr8+0RDqr0Ql0h+rP48ckxlpbzKgwra6" crossorigin="anonymous"><title>ESP Garden</title></head><body><div class="container"><h1>ESP Garden</h1><h2>Status</h2><p id="p-date"></p><p id="p-time"></p><p id="p-uptime"></p><h2>Inputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-inputs"></tbody></table><h2>Outputs</h2><table class="table table-striped"><thead><tr><th scope="col">#</th><th scope="col">Port</th><th scope="col">Value</th></tr></thead><tbody id="tbody-outputs"></tbody></table></div><script src="https://code.jquery.com/jquery-3.6.0.slim.min.js" integrity="sha256-u7e5khyithlIdTpu22PHhENmPcRdFiHRjhAuHcs05RI=" crossorigin="anonymous"></script><script src="https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta3/dist/js/bootstrap.bundle.min.js" integrity="sha384-JEW9xMcG8R+pH31jmWH6WWP0WintQrMb4s7ZOdauHnUtxwoG2vI5DkLtS3qm9Ekf" crossorigin="anonymous"></script><script>function fillTable(id, data){ var tbody=$(id); tbody.empty(); for (var i=0; i < data.length; i++){ var tr=$('<tr/>'); tr.append($('<th/>').html(i).attr("scope", "row")); for (var key in data[i]){ tr.append($('<td/>').html(key)); tr.append($('<td/>').html(data[i][key]));} tbody.append(tr);}} function refresh(){ setTimeout(refresh, 1 * 1000); var info={ "Date": Date(Date.now()), "Time": "time", "Uptime": "uptime", "Inputs": [ { "a0": (Math.random() * 1024).toFixed()}, { "a1": (Math.random() * 1024).toFixed()}, { "a2": (Math.random() * 1024).toFixed()} ], "Outputs": [ { "gpio0": "0"}, { "gpio1": "1"}, { "gpio2": "2"} ]}; /*$.getJSON("data.json", function (data){ info=data;});*/ $("#p-date").text("Date: " + info["Date"]); $("#p-time").text("Time: " + info["Time"]); $("#p-uptime").text("Uptime: " + info["Uptime"]); fillTable("#tbody-inputs", info["Inputs"]); fillTable("#tbody-outputs", info["Outputs"]);} $(function onReady(){ refresh();}); </script></body></html>
)EOF";

ESP8266WebServer server(80);
WiFiClient client;

void handleRoot()
{
  digitalWrite(led, 1);
  server.send(200, "text/html", indexHtml);
  digitalWrite(led, 0);
}

void handleNotFound()
{
  digitalWrite(led, 1);
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++)
  {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  digitalWrite(led, 0);
}

void handleDataJson()
{
  StaticJsonDocument<256> doc;
  static char output[256];

  doc["Date"] = "date";
  doc["Time"] = "time";
  doc["Uptime"] = "uptime";
  JsonObject Inputs_0 = doc["Inputs"].createNestedObject();
  Inputs_0["A0"] = analogRead(A0);
  JsonObject Outputs_0 = doc["Outputs"].createNestedObject();
  Outputs_0["gpio0"] = 0;
  
  serializeJson(doc, output);

  server.send(200, "text/json", output);
}

void setup(void)
{
  pinMode(led, OUTPUT);
  digitalWrite(led, 0);
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

  if (MDNS.begin("wemos"))
  {
    Serial.println("MDNS responder started");
  }

  server.on("/", handleRoot);
  server.on("data.json", handleDataJson);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");

  ThingSpeak.begin(client);
}

void loop(void)
{
  server.handleClient();
  MDNS.update();
}
