#include "SPIFFS.h"
#include "BuildConfig.h"
#include "core/config.h"
#include "core/io_history.h"
#include "core/logger.h"
#include "core/role.h"
#include "core/tasks.h"
#include "network/custom_login.h"
#include "network/web.h"
#include "network/web_config.h"
#include "network/web_data.h"
#include "network/web_ota.h"
#include "network/web_users.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
// 3.7.x no longer pulls WiFi.h in transitively the way the old fork did.
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

static AsyncWebServer g_webServer(80);

bool g_wifiConnected = false;
bool g_hasNetwork = false;

void
handleControl(AsyncWebServerRequest* request)
{
    digitalWrite(LED_BUILTIN, 1);

    bool restartRequested = false;

    // relay + relayTime address any relay; watering / wateringTime are the
    // legacy spelling for relay 0 and stay supported.
    if (request->hasParam("relay", true)) {
        const unsigned index =
          request->getParam("relay", true)->value().toInt();
        unsigned duration = g_wateringDefaultTime;
        if (request->hasParam("relayTime", true)) {
            duration = request->getParam("relayTime", true)->value().toInt();
        }
        startRelay(index, duration);
    }

    for (int i = 0; i < request->params(); ++i) {
        const AsyncWebParameter* param = request->getParam(i);
        if ((param->name() == "watering") && (param->value() == "enable")) {
            startWatering();
        } else if (param->name() == "wateringTime") {
            startWatering(param->value().toInt());
        } else if (param->name() == "mqtt") {
            if (param->value() == "enable") {
                mqttEnable(true);
            } else if (param->value() == "disable") {
                mqttEnable(false);
            }
        } else if ((param->name() == "reset") && (param->value() == "1")) {
            restartRequested = true;
        }
    }

    request->send(200);

    digitalWrite(LED_BUILTIN, 0);

    // Handed to loop() rather than done here: request->send() only queues the
    // response, and rebooting (or blocking) on the async_tcp task kills the
    // connection before it flushes, so the caller could never tell a reboot
    // from a failure.
    if (restartRequested) {
        logger.warning("Restart requested over /control.");
        requestRestart();
    }
}

void
handleLogs(AsyncWebServerRequest* request)
{
    digitalWrite(LED_BUILTIN, 1);

    String output = logger.read();
    request->send(200, "text/plain", output);

    digitalWrite(LED_BUILTIN, 0);
}

// Hard cap on one response. 1440 records is 57 KB of raw struct and roughly
// 170 KB rendered as JSON — more than half this chip's DRAM, with WiFi already
// holding a large share. Callers page with ?limit=.
static const size_t g_historyMaxResponse = 200;

// NaN marks a channel this board does not have; JSON has no NaN literal and
// null is the honest encoding. isfinite() rather than isnan(): an infinity
// serializes as `inf`, which is not valid JSON, so one bad sample would make
// the browser throw on the whole response instead of on one field.
static void
appendFloatValue(String& out, float value)
{
    if (isfinite(value)) {
        out += String(value, 2);
    } else {
        out += "null";
    }
}

static void
appendFloat(String& out, const char* key, float value)
{
    out += "\"";
    out += key;
    out += "\":";
    appendFloatValue(out, value);
}

static void
handleHistoryJson(AsyncWebServerRequest* request)
{
    if (!ioHistory.ready()) {
        request->send(503, "text/plain", "history buffer not available");
        return;
    }

    size_t limit = 100;
    if (request->hasParam("limit")) {
        const long asked = request->getParam("limit")->value().toInt();
        if (asked > 0) {
            limit = (size_t)asked;
        }
    }
    if (limit > g_historyMaxResponse) {
        limit = g_historyMaxResponse;
    }

    // ?window=<seconds> selects by time and DECIMATES to fit. Without it a 24 h
    // window is 1440 records against a 200-record cap, so the page could only
    // ever show the newest 3 h — asking for a day and getting three hours,
    // silently.
    static IoRecord buffer[g_historyMaxResponse];
    size_t count = 0;
    uint32_t stride = 1;
    uint32_t offset = IoHistory::kNewest;
    long window = 0;

    if (request->hasParam("window")) {
        window = request->getParam("window")->value().toInt();
    }

    if (window > 0) {
        const time_t now = time(NULL);
        // A clock that has not synced would make every record look in-window;
        // fall back to "everything stored" rather than inventing a range.
        const uint32_t since =
          (now > (time_t)window) ? (uint32_t)(now - window) : 0;
        uint32_t from = 0;
        // One call: locating the window and reading it have to happen under the
        // same lock, or an append() between them shifts every logical index.
        count = ioHistory.readWindow(since, buffer, limit, &stride, &from);
        offset = from;
    } else {
        if (request->hasParam("offset")) {
            const long asked = request->getParam("offset")->value().toInt();
            if (asked >= 0) {
                offset = (uint32_t)asked;
            }
        }
        count = ioHistory.read(buffer, limit, offset);
    }

    String out;
    out.reserve(count * 140 + 128);
    out += "{\"capacity\":";
    out += String(ioHistory.capacity());
    out += ",\"stored\":";
    out += String(ioHistory.stored());
    out += ",\"returned\":";
    out += String(count);
    out += ",\"offset\":";
    out += String(offset == IoHistory::kNewest
                    ? (ioHistory.stored() > count ? ioHistory.stored() - count : 0)
                    : offset);
    // The page needs the stride to say "1 point per 8 minutes" rather than
    // implying every sample is shown.
    out += ",\"stride\":";
    out += String(stride);
    out += ",\"window\":";
    out += String(window);
    out += ",\"records\":[";

    for (size_t i = 0; i < count; ++i) {
        const IoRecord& r = buffer[i];
        if (i) {
            out += ",";
        }
        out += "{\"t\":";
        out += String(r.timestamp);
        out += ",\"relays\":";
        out += String(r.relayMask);
        out += ",\"moisture\":[";
        for (unsigned m = 0; m < IO_HISTORY_MAX_MOISTURE; ++m) {
            if (m) {
                out += ",";
            }
            appendFloatValue(out, r.moisture[m]);
        }
        out += "],";
        appendFloat(out, "lum", r.luminosity);
        out += ",";
        appendFloat(out, "temp", r.temperature);
        out += ",";
        appendFloat(out, "hum", r.airHumidity);
        out += ",";
        appendFloat(out, "water", r.waterLevel);
        out += "}";
    }
    out += "]}";

    AsyncWebServerResponse* response =
      request->beginResponse(200, "application/json", out);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

// Refuses a path outright. Used to shadow files that a serveStatic below would
// otherwise expose.
static void
handleForbidden(AsyncWebServerRequest* request)
{
    request->send(403, "text/plain", "Forbidden");
}

static void
servePublicFile(const char* route, const char* path, const char* contentType)
{
    g_webServer.on(
      route, HTTP_GET, [path, contentType](AsyncWebServerRequest* request) {
          digitalWrite(LED_BUILTIN, 1);
          request->send(SPIFFS, path, contentType);
          digitalWrite(LED_BUILTIN, 0);
      });
}

static void
wifiConnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    logger.info("Wifi connected.");
    g_wifiConnected = true;
}

static void
wifiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
    logger.info("IP address: " + WiFi.localIP().toString());
    g_hasNetwork = true;
}

static void
wifiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (g_wifiConnected) {
        logger.warning("Wifi disconnected. Reconnecting...");
    }

    g_wifiConnected = false;
    g_hasNetwork = false;

    WiFi.begin(g_ssid.c_str(), g_wifiPassword.c_str());
}

void
webSetup()
{
    logger.info("Web setup...");

    g_dataMutex = xSemaphoreCreateMutex();
    webUpdateDataCache();

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(g_hostname.c_str());
    WiFi.onEvent(wifiConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent(wifiGotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(wifiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.begin(g_ssid.c_str(), g_wifiPassword.c_str());

    if (MDNS.begin(g_hostname.c_str()) == false) {
        logger.warning("Error starting mDNS!");
    }

    // Public assets: the pages themselves carry no data, and the login page has
    // to be reachable before a token exists. Everything that reads or changes
    // state is guarded below.
    servePublicFile("/", "/index.html", "text/html");
    servePublicFile("/index.html", "/index.html", "text/html");
    servePublicFile("/index.js", "/index.js", "application/javascript");
    servePublicFile("/login.html", "/login.html", "text/html");
    servePublicFile("/login.js", "/login.js", "application/javascript");
    // Vendored so the pages work with the WAN down. The config page in
    // particular exists to fix a device that cannot reach the internet, and
    // with jQuery on a CDN it rendered blank in exactly that situation.
    servePublicFile("/jquery.js", "/jquery.js", "application/javascript");
    servePublicFile("/spark-md5.js", "/spark-md5.js", "application/javascript");
    servePublicFile("/sha256.js", "/sha256.js", "application/javascript");
    servePublicFile("/auth.js", "/auth.js", "application/javascript");
    servePublicFile("/config.html", "/config.html", "text/html");
    servePublicFile("/config.js", "/config.js", "application/javascript");
    servePublicFile("/users.html", "/users.html", "text/html");
    servePublicFile("/users.js", "/users.js", "application/javascript");
    servePublicFile("/history.html", "/history.html", "text/html");
    servePublicFile("/history.js", "/history.js", "application/javascript");
    servePublicFile("/devices.html", "/devices.html", "text/html");
    servePublicFile("/devices.js", "/devices.js", "application/javascript");
    servePublicFile("/schedules.html", "/schedules.html", "text/html");
    servePublicFile("/schedules.js", "/schedules.js", "application/javascript");
    servePublicFile("/update.html", "/update.html", "text/html");
    servePublicFile("/update.js", "/update.js", "application/javascript");
    servePublicFile("/favicon.ico", "/favicon.ico", "image/x-icon");

#if USE_CUSTOM_LOGIN
    customLogin.begin();

    g_webServer.on("/nonce", HTTP_GET, [](AsyncWebServerRequest* request) {
        customLogin.handleNonce(request);
    });
    g_webServer.on("/login", HTTP_POST, [](AsyncWebServerRequest* request) {
        customLogin.handleLogin(request);
    });
    g_webServer.on("/logout", HTTP_POST, [](AsyncWebServerRequest* request) {
        customLogin.handleLogout(request);
    });

    auto& authenticated = customLogin.authenticationMiddleware;
    auto* adminOnly = customLogin.requireRole(Role::ADMIN);
    auto* operatorOnly = customLogin.requireRole(Role::OPERATOR);

    g_webServer.on("/data.json", HTTP_GET, handleDataJson)
      .addMiddleware(&authenticated);
    g_webServer.on("/history.json", HTTP_GET, handleHistoryJson)
      .addMiddleware(&authenticated);
    g_webServer.on("/control", HTTP_POST, handleControl)
      .addMiddleware(operatorOnly);
    g_webServer.on("/logs", HTTP_GET, handleLogs).addMiddleware(adminOnly);
    g_webServer.on("/config.json", HTTP_GET, handleConfigGet)
      .addMiddleware(adminOnly);
    g_webServer.on("/config.json", HTTP_POST, handleConfigPost)
      .addMiddleware(adminOnly);
    g_webServer.on("/users.json", HTTP_GET, handleUsersJson)
      .addMiddleware(adminOnly);
    g_webServer.on("/users", HTTP_POST, handleUsersPost).addMiddleware(adminOnly);
    g_webServer.on("/updateEnable", HTTP_POST, handleUpdateEnable)
      .addMiddleware(adminOnly);
    g_webServer
      .on("/update", HTTP_POST, handleUpdateRequest, handleUpdateUpload)
      .addMiddleware(adminOnly);

    // MUST be registered BEFORE the serveStatic they shadow: handlers are
    // matched in registration order, and the matcher is a PREFIX, so the
    // rotated users.bak.json variants are covered too. /config.json carries the
    // WiFi and MQTT passwords in plaintext, /users.json the password hashes and
    // salts, /sessions.json live bearer tokens — none of them may ever be
    // served, even to an admin.
    g_webServer
      .on(AsyncURIMatcher::prefix("/spiffs/users"), HTTP_GET, handleForbidden)
      .addMiddleware(adminOnly);
    g_webServer
      .on(AsyncURIMatcher::prefix("/spiffs/sessions"),
          HTTP_GET,
          handleForbidden)
      .addMiddleware(adminOnly);
    g_webServer
      .on(AsyncURIMatcher::prefix("/spiffs/config"), HTTP_GET, handleForbidden)
      .addMiddleware(adminOnly);
    g_webServer.serveStatic("/spiffs", SPIFFS, "/").addMiddleware(adminOnly);
#else
    g_webServer.on("/data.json", HTTP_GET, handleDataJson);
    g_webServer.on("/history.json", HTTP_GET, handleHistoryJson);
    g_webServer.on("/control", HTTP_POST, handleControl);
    g_webServer.on("/logs", HTTP_GET, handleLogs);
    g_webServer.on("/updateEnable", HTTP_POST, handleUpdateEnable);
    g_webServer.on(
      "/update", HTTP_POST, handleUpdateRequest, handleUpdateUpload);
#endif

    g_webServer.onNotFound(
      [](AsyncWebServerRequest* request) { request->send(404); });

    g_webServer.begin();

    logger.info("Web setup done!");
}
