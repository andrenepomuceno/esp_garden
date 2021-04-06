#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ThingSpeak.h>
#include <ArduinoJson.h>
#include "secret.h"

const int led = 2;
const char indexHtml[] PROGMEM = R"EOF(<html><head> <meta charset="utf-8"> <script src="https://ajax.googleapis.com/ajax/libs/jquery/3.5.1/jquery.min.js"></script> <title>wemos</title> <style>table, th, td{border: 1px solid black;}</style></head><body> <script>function drawTable(selector, myList){$(selector).empty(); var columns=addAllColumnHeaders(myList, selector); for (var i=0; i < myList.length; i++){var row$=$('<tr/>'); for (var colIndex=0; colIndex < columns.length; colIndex++){var cellValue=myList[i][columns[colIndex]]; if (cellValue==null) cellValue=""; row$.append($('<td/>').html(cellValue));}$(selector).append(row$);}}function addAllColumnHeaders(myList, selector){var columnSet=[]; var headerTr$=$('<tr/>'); for (var i=0; i < myList.length; i++){var rowHash=myList[i]; for (var key in rowHash){if ($.inArray(key, columnSet)==-1){columnSet.push(key); headerTr$.append($('<th/>').html(key));}}}$(selector).append(headerTr$); return columnSet;}function render(info){$("#p-date").text("Date: " + info["Date"]); $("#p-time").text("Time: " + info["Time"]); $("#p-uptime").text("Uptime: " + info["Uptime"]); drawTable("#t-sensors", info["Inputs"]); drawTable("#t-schedule", info["Outputs"]);}function refresh(){setTimeout(refresh, 5*1000); $.getJSON("data.json", function (data){render(data);});}$(function onReady(){refresh();}); </script> <h1>WEMOS</h1> <h2>Status</h2> <p id="p-date"></p><p id="p-time"></p><p id="p-uptime"></p><h2>Inputs</h2> <p><table id="t-sensors"></table></p><h2>Outputs</h2> <p><table id="t-schedule"></table></p></body></html>
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
